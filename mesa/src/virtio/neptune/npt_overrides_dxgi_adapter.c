/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * IDXGIAdapter{,1..4} overrides.  An adapter is one chain wrapper
 * (npt_com_register_chain) with the vtbl of the highest version the host
 * adapter implements.  EnumOutputs is overridden to return
 * guest-fabricated IDXGIOutput wrappers, one per local DRM connector (or one synthetic fallback when
 * no DRM is available).  D3DMetal and DXMT return ~0 as the UMD version,
 * so CheckInterfaceSupport reports a plausible one instead (Hunt: Showdown
 * 1896 parses it with std::stoull, Obelisk issue #46).  All other adapter
 * methods pass through to the host.
 */

#include "npt_com.h"
#include "npt_display.h"
#include "npt_env.h"
#include "npt_overrides.h"
#include "npt_device.h"

#include "neptune-protocol/npt_protocol_client_idxgiadapter.h"
#include "neptune-protocol/npt_protocol_client_idxgioutput.h"
#include "neptune-protocol/npt_protocol_defs.h"

#if defined(__WINE__)
#include "nptunix/npt_unixlib.h"
#endif

#include "util/os_time.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef TRUE
#define TRUE  ((BOOL)1)
#endif
#ifndef FALSE
#define FALSE ((BOOL)0)
#endif

/* A QueryVideoMemoryInfo answer kept until expires_ns (0: none). */
struct adp3_vidmem_slot {
   int64_t expires_ns;
   HRESULT hr;
   DXGI_QUERY_VIDEO_MEMORY_INFO info;
};

struct npt_adapter_aux {
   mtx_t lock;
   bool populated;
   struct npt_output_list outputs;
   /* Node 0's local and non-local segment groups
    * (adp3_QueryVideoMemoryInfo_override). */
   SRWLOCK vidmem_lock;
   struct adp3_vidmem_slot vidmem[2];
};

static void
adp_aux_destroy(void *p)
{
   struct npt_adapter_aux *aux = p;
   if (!aux)
      return;
   mtx_destroy(&aux->lock);
   free(aux);
}

static void
adp_aux_init(struct npt_com_base *com, struct npt_device *dev, uint64_t host_id)
{
   (void)dev; (void)host_id;
   struct npt_adapter_aux *aux = com->aux;
   mtx_init(&aux->lock, mtx_plain);
   InitializeSRWLock(&aux->vidmem_lock);
   com->aux_destroy = adp_aux_destroy;
}

static void
fill_synthetic_output(struct npt_output_info *o)
{
   memset(o, 0, sizeof(*o));
   snprintf(o->name, sizeof(o->name), "Synthetic-1");
   o->current_width  = 1920;
   o->current_height = 1080;
   o->num_modes      = 1;
   o->modes[0].width        = 1920;
   o->modes[0].height       = 1080;
   o->modes[0].refresh_num  = 60;
   o->modes[0].refresh_den  = 1;
   o->modes[0].bpp          = 32;
   o->modes[0].preferred    = true;
}

static void
fetch_output_list(struct npt_output_list *list)
{
   memset(list, 0, sizeof(*list));
#if defined(__WINE__)
   if (npt_unixlib_ensure_init() == 0) {
      struct npt_unix_enumerate_outputs_params p;
      memset(&p, 0, sizeof(p));
      p.expose_all_modes = NPT_DEBUG(EXPOSE_ALL_MODES) ? 1 : 0;
      npt_wine_unix_call(npt_unix_enumerate_outputs, &p);
      *list = p.list;
   }
#endif
   if (list->num_outputs == 0) {
      /* DXVK dmabuf-backend behavior: at least one synthetic output so
       * D3D mode-enumeration paths see something. */
      fill_synthetic_output(&list->outputs[0]);
      list->num_outputs = 1;
   }
}

static bool
adp_ensure_outputs(struct npt_adapter_aux *aux)
{
   mtx_lock(&aux->lock);
   if (!aux->populated) {
      fetch_output_list(&aux->outputs);
      aux->populated = true;
   }
   bool ok = aux->outputs.num_outputs > 0;
   mtx_unlock(&aux->lock);
   return ok;
}

static HRESULT NPT_STDMETHODCALLTYPE
adp_EnumOutputs_override(void *self, UINT Output, IDXGIOutput **ppOutput)
{
   if (!ppOutput)
      return NPT_E_POINTER;
   *ppOutput = NULL;

   struct npt_com_base *adp = self;
   struct npt_adapter_aux *aux = adp->aux;
   if (!aux || !adp_ensure_outputs(aux))
      return NPT_DXGI_ERROR_NOT_FOUND;
   if (Output >= aux->outputs.num_outputs)
      return NPT_DXGI_ERROR_NOT_FOUND;

   /* Synthetic id is unique per (parent adapter, connector index). */
   uint64_t out_id = npt_com_make_guest_id(
      NPT_GUEST_KIND_OUTPUT,
      (adp->base.id << 8) ^ Output);

   struct npt_device *dev = npt_com_self_device(self);
   void *wrapper = npt_com_get_or_wrap(dev, &NPT_IID_IDXGIOutput6,
                                       out_id, adp);
   if (!wrapper)
      return NPT_E_OUTOFMEMORY;

   npt_output_aux_populate(wrapper, adp, Output,
                           &aux->outputs.outputs[Output]);
   *ppOutput = (IDXGIOutput *)wrapper;
   return NPT_S_OK;
}

/* Every success reports a made-up version, as DXVK and wined3d do: a Metal
 * backend has no Windows UMD version to preserve.  This one is the
 * DriverVersion win32u writes for every GPU under
 * HKLM\Software\Microsoft\DirectX, 35.0.15.8180 (581.80 when the vendor is
 * NVIDIA).
 *
 * Except on an AMD adapter (D3DMetal's "AMD Compatibility Mode").  Hunt:
 * Showdown 1896 prints the UMD version "%d.%02d.%02d.%04d", drops the dots
 * and reads one number (CryRenderD3D12.dll "D3D Adapter: Driver version
 * (UMD)"), and asks for at least 31.0.24027.1012 on AMD (3100240271012) and
 * 32.0.15.5599 on NVIDIA (3200155599) before it stops showing "Outdated
 * graphics driver detected".  35.0.15.8180 reads as 3500158180: enough for
 * NVIDIA, but AMD build numbers have five digits.  An AMD adapter gets
 * 35.0.21025.1024 instead, the DriverVersion win32u writes for AMD GPUs
 * (3500210251024). */
static HRESULT NPT_STDMETHODCALLTYPE
adp_CheckInterfaceSupport_override(void *self, const GUID *InterfaceName,
                                   LARGE_INTEGER *pUMDVersion)
{
   HRESULT hr = npt_idxgiadapter_default_CheckInterfaceSupport(
      self, InterfaceName, pUMDVersion);
   if (NPT_SUCCEEDED(hr) && pUMDVersion) {
      DXGI_ADAPTER_DESC desc;
      memset(&desc, 0, sizeof(desc));
      const bool amd =
         NPT_SUCCEEDED(npt_idxgiadapter_default_GetDesc(self, &desc)) &&
         desc.VendorId == 0x1002;
      pUMDVersion->QuadPart = amd ? 0x0023000052210400   /* 35.0.21025.1024 */
                                  : 0x00230000000f1ff4;  /* 35.0.15.8180 */
   }
   return hr;
}

static HRESULT NPT_STDMETHODCALLTYPE
adp3_RegisterHardwareContentProtectionTeardownStatusEvent_override(
   void *self, HANDLE hEvent, DWORD *pdwCookie)
{
   (void)self; (void)hEvent;
   if (pdwCookie) *pdwCookie = 0;
   return NPT_E_NOTIMPL;
}

static void NPT_STDMETHODCALLTYPE
adp3_UnregisterHardwareContentProtectionTeardownStatus_override(
   void *self, DWORD dwCookie)
{
   (void)self; (void)dwCookie;
}

/* The host cannot signal a guest event (an unregistered HANDLE reaches it
 * as NULL), and nothing here changes the budget QueryVideoMemoryInfo
 * reports, so the registration succeeds and the event is never set: what
 * Windows does while the budget holds still. */
static _Atomic DWORD adp3_budget_cookie;

static HRESULT NPT_STDMETHODCALLTYPE
adp3_RegisterVideoMemoryBudgetChangeNotificationEvent_override(
   void *self, HANDLE hEvent, DWORD *pdwCookie)
{
   (void)self;
   if (!hEvent || !pdwCookie)
      return NPT_DXGI_ERROR_INVALID_CALL;
   *pdwCookie = atomic_fetch_add(&adp3_budget_cookie, 1) + 1;
   return NPT_S_OK;
}

static void NPT_STDMETHODCALLTYPE
adp3_UnregisterVideoMemoryBudgetChangeNotification_override(void *self,
                                                            DWORD dwCookie)
{
   (void)self; (void)dwCookie;
}

/* QueryVideoMemoryInfo is a synchronous round trip to the render server, and
 * Unreal Engine 5 polls it thousands of times a second from its RHI threads
 * (about 2,400 calls a second in Bodycam's lobby, 170 us each: 40% of one
 * thread waiting).  The budget and usage it reports move slowly, so the
 * adapter keeps one answer per segment group for NPT_VIDMEM_CACHE_NS and
 * serves it from the guest.  Node 0 only: a multi-adapter caller still asks
 * the host. */
#define NPT_VIDMEM_CACHE_NS (100ll * 1000 * 1000)

/* The slot kept for (NodeIndex, group), or NULL when the host answers. */
static struct adp3_vidmem_slot *
adp3_vidmem_slot(struct npt_adapter_aux *aux, UINT NodeIndex,
                 DXGI_MEMORY_SEGMENT_GROUP group)
{
   if (!aux || NodeIndex != 0 || (unsigned)group >= ARRAY_SIZE(aux->vidmem))
      return NULL;
   return &aux->vidmem[group];
}

static HRESULT NPT_STDMETHODCALLTYPE
adp3_QueryVideoMemoryInfo_override(
   void *self, UINT NodeIndex, DXGI_MEMORY_SEGMENT_GROUP MemorySegmentGroup,
   DXGI_QUERY_VIDEO_MEMORY_INFO *pVideoMemoryInfo)
{
   struct npt_com_base *adp = self;
   struct npt_adapter_aux *aux = adp->aux;
   struct adp3_vidmem_slot *slot =
      pVideoMemoryInfo ? adp3_vidmem_slot(aux, NodeIndex, MemorySegmentGroup)
                       : NULL;
   if (!slot)
      return npt_idxgiadapter3_default_QueryVideoMemoryInfo(
         self, NodeIndex, MemorySegmentGroup, pVideoMemoryInfo);

   const int64_t now = os_time_get_nano();
   HRESULT hr = NPT_S_OK;
   AcquireSRWLockShared(&aux->vidmem_lock);
   const bool kept = now < slot->expires_ns;
   if (kept) {
      *pVideoMemoryInfo = slot->info;
      hr = slot->hr;
   }
   ReleaseSRWLockShared(&aux->vidmem_lock);
   if (kept)
      return hr;

   hr = npt_idxgiadapter3_default_QueryVideoMemoryInfo(
      self, NodeIndex, MemorySegmentGroup, pVideoMemoryInfo);

   AcquireSRWLockExclusive(&aux->vidmem_lock);
   slot->hr = hr;
   slot->info = *pVideoMemoryInfo;
   slot->expires_ns =
      NPT_SUCCEEDED(hr) ? os_time_get_nano() + NPT_VIDMEM_CACHE_NS : 0;
   ReleaseSRWLockExclusive(&aux->vidmem_lock);
   return hr;
}

/* A reservation changes the CurrentReservation the next query reports. */
static HRESULT NPT_STDMETHODCALLTYPE
adp3_SetVideoMemoryReservation_override(
   void *self, UINT NodeIndex, DXGI_MEMORY_SEGMENT_GROUP MemorySegmentGroup,
   UINT64 Reservation)
{
   const HRESULT hr = npt_idxgiadapter3_default_SetVideoMemoryReservation(
      self, NodeIndex, MemorySegmentGroup, Reservation);
   struct npt_com_base *adp = self;
   struct npt_adapter_aux *aux = adp->aux;
   struct adp3_vidmem_slot *slot =
      adp3_vidmem_slot(aux, NodeIndex, MemorySegmentGroup);
   if (slot) {
      AcquireSRWLockExclusive(&aux->vidmem_lock);
      slot->expires_ns = 0;
      ReleaseSRWLockExclusive(&aux->vidmem_lock);
   }
   return hr;
}

#define NPT_REGISTER_OVERRIDE_DXGI_ADAPTER4(m, f) \
   NPT_REGISTER_OVERRIDE(idxgiadapter4, m, f)
#define NPT_REGISTER_OVERRIDE_DXGI_ADAPTER3(m, f) \
   NPT_REGISTER_OVERRIDE(idxgiadapter3, m, f); \
   NPT_REGISTER_OVERRIDE_DXGI_ADAPTER4(m, f)
#define NPT_REGISTER_OVERRIDE_DXGI_ADAPTER2(m, f) \
   NPT_REGISTER_OVERRIDE(idxgiadapter2, m, f); \
   NPT_REGISTER_OVERRIDE_DXGI_ADAPTER3(m, f)
#define NPT_REGISTER_OVERRIDE_DXGI_ADAPTER1(m, f) \
   NPT_REGISTER_OVERRIDE(idxgiadapter1, m, f); \
   NPT_REGISTER_OVERRIDE_DXGI_ADAPTER2(m, f)
#define NPT_REGISTER_OVERRIDE_DXGI_ADAPTER(m, f) \
   NPT_REGISTER_OVERRIDE(idxgiadapter, m, f); \
   NPT_REGISTER_OVERRIDE_DXGI_ADAPTER1(m, f)

static const GUID *const adapter_tiers[] = {
   &NPT_IID_IDXGIAdapter,  &NPT_IID_IDXGIAdapter1, &NPT_IID_IDXGIAdapter2,
   &NPT_IID_IDXGIAdapter3, &NPT_IID_IDXGIAdapter4, NULL,
};

void
npt_overrides_dxgi_adapter_init(void)
{
   NPT_REGISTER_OVERRIDE_DXGI_ADAPTER(
      EnumOutputs, adp_EnumOutputs_override);
   NPT_REGISTER_OVERRIDE_DXGI_ADAPTER(
      CheckInterfaceSupport, adp_CheckInterfaceSupport_override);

   NPT_REGISTER_OVERRIDE_DXGI_ADAPTER3(
      RegisterHardwareContentProtectionTeardownStatusEvent,
      adp3_RegisterHardwareContentProtectionTeardownStatusEvent_override);
   NPT_REGISTER_OVERRIDE_DXGI_ADAPTER3(
      UnregisterHardwareContentProtectionTeardownStatus,
      adp3_UnregisterHardwareContentProtectionTeardownStatus_override);
   NPT_REGISTER_OVERRIDE_DXGI_ADAPTER3(
      RegisterVideoMemoryBudgetChangeNotificationEvent,
      adp3_RegisterVideoMemoryBudgetChangeNotificationEvent_override);
   NPT_REGISTER_OVERRIDE_DXGI_ADAPTER3(
      UnregisterVideoMemoryBudgetChangeNotification,
      adp3_UnregisterVideoMemoryBudgetChangeNotification_override);
   NPT_REGISTER_OVERRIDE_DXGI_ADAPTER3(
      QueryVideoMemoryInfo, adp3_QueryVideoMemoryInfo_override);
   NPT_REGISTER_OVERRIDE_DXGI_ADAPTER3(
      SetVideoMemoryReservation, adp3_SetVideoMemoryReservation_override);

   npt_com_register_family(adapter_tiers,
                           sizeof(struct npt_adapter_aux),
                           adp_aux_init);
   static const struct npt_com_chain adapter_chain = { adapter_tiers, 4 };
   npt_com_register_chain(&adapter_chain);
}
