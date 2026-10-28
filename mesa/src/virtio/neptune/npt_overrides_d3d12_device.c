/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * ID3D12Device{,1..14} overrides:
 *  - CheckFeatureSupport: skip_default because a handful of feature
 *    structs embed guest pointers that the generic in/out byte blob
 *    would smuggle to the host as garbage.  Pointer-free features pass
 *    through the generated call unchanged; FEATURE_LEVELS repacks the
 *    requested-levels array after the struct (host override rebuilds
 *    the pointer); the remaining pointer-bearing features are rejected
 *    until an app needs them.
 *  - CreateCommittedResource{,1,2} / CreateHeap{,1} /
 *    CreatePlacedResource{,1}: CPU-visible buffer heaps are re-routed
 *    onto guest-shmem-backed heaps (npt_d3d12_heap.c) so Map() becomes
 *    a zero-wire persistent pointer.  ANY failure on that path falls
 *    back to the plain generated thunk (phase-1 sync map still works).
 *  - GetDescriptorHandleIncrementSize + the descriptor heap's
 *    GetCPU/GPUDescriptorHandleForHeapStart: immutable host answers
 *    behind sync round-trips; cached after the first call (engines
 *    re-derive descriptor handles from them constantly).
 *  - CreatePipelineState and ID3D12PipelineLibrary1::LoadPipeline: a
 *    pipeline state stream travels subobject by subobject, so one the
 *    encoder cannot walk is refused here with E_INVALIDARG, as D3D12
 *    refuses it.
 *  - GetCopyableFootprints{,1}: the host replies with every output array,
 *    whichever the app asked for.  An app may pass NULL for the arrays it
 *    does not need (an upload allocator asking only for TotalBytes), and
 *    the reply decoder would leave those arrays in the stream and read
 *    TotalBytes from the wrong place; the missing outputs get scratch
 *    space here.
 *  - GetDeviceRemovedReason: DXGI_ERROR_DRIVER_INTERNAL_ERROR once the
 *    render server no longer serves the context (npt_renderer_is_lost).
 *  - The methods that cannot cross the wire are answered here, as a
 *    device without that feature answers them.  Raytracing and work
 *    graphs need state objects, whose shader identifiers and export
 *    associations the protocol does not carry, so CheckFeatureSupport
 *    reports both tiers as not supported and CreateStateObject/
 *    AddToStateObject fail; heaps over the app's own memory
 *    (OpenExistingHeapFrom*) would hand the host a guest address or
 *    HANDLE, so EXISTING_HEAPS is not supported and they fail; meta
 *    commands, whose descriptions carry host string pointers, are
 *    enumerated as none; a lifetime tracker would need the host to call
 *    the app's ID3D12LifetimeOwner.
 *  - EnqueueMakeResident: resident at once (MakeResident), then the fence
 *    is signalled from the CPU.  SetBackgroundProcessingMode has no
 *    background work to wait for: the event is set at once.
 *  - CreateCommandQueue1 pins its queue like CreateCommandQueue.
 *  - The device and pipeline library version chains
 *    (npt_com_register_chain), up to ID3D12Device14 and
 *    ID3D12PipelineLibrary1.
 */

#include "npt_com.h"
#include "npt_d3d12_heap.h"
#include "npt_device.h"
#include "npt_env.h"
#include "npt_overrides.h"
#include "npt_tls.h"

#include "neptune-protocol/npt_protocol_client_id3d12descriptorheap.h"
#include "neptune-protocol/npt_protocol_client_id3d12device.h"
#include "neptune-protocol/npt_protocol_client_id3d12fence.h"
#include "neptune-protocol/npt_protocol_client_id3d12pipelinelibrary.h"
#include "neptune-protocol/npt_protocol_guest_id3d12device.h"
#include "neptune-protocol/npt_protocol_defs.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#define NPT_REGISTER_OVERRIDE_D3D12_DEVICE14(m, f) \
   NPT_REGISTER_OVERRIDE(id3d12device14, m, f)
#define NPT_REGISTER_OVERRIDE_D3D12_DEVICE13(m, f) \
   NPT_REGISTER_OVERRIDE(id3d12device13, m, f); \
   NPT_REGISTER_OVERRIDE_D3D12_DEVICE14(m, f)
#define NPT_REGISTER_OVERRIDE_D3D12_DEVICE12(m, f) \
   NPT_REGISTER_OVERRIDE(id3d12device12, m, f); \
   NPT_REGISTER_OVERRIDE_D3D12_DEVICE13(m, f)
#define NPT_REGISTER_OVERRIDE_D3D12_DEVICE11(m, f) \
   NPT_REGISTER_OVERRIDE(id3d12device11, m, f); \
   NPT_REGISTER_OVERRIDE_D3D12_DEVICE12(m, f)
#define NPT_REGISTER_OVERRIDE_D3D12_DEVICE10(m, f) \
   NPT_REGISTER_OVERRIDE(id3d12device10, m, f); \
   NPT_REGISTER_OVERRIDE_D3D12_DEVICE11(m, f)
#define NPT_REGISTER_OVERRIDE_D3D12_DEVICE9(m, f) \
   NPT_REGISTER_OVERRIDE(id3d12device9, m, f); \
   NPT_REGISTER_OVERRIDE_D3D12_DEVICE10(m, f)
#define NPT_REGISTER_OVERRIDE_D3D12_DEVICE8(m, f) \
   NPT_REGISTER_OVERRIDE(id3d12device8, m, f); \
   NPT_REGISTER_OVERRIDE_D3D12_DEVICE9(m, f)
#define NPT_REGISTER_OVERRIDE_D3D12_DEVICE7(m, f) \
   NPT_REGISTER_OVERRIDE(id3d12device7, m, f); \
   NPT_REGISTER_OVERRIDE_D3D12_DEVICE8(m, f)
#define NPT_REGISTER_OVERRIDE_D3D12_DEVICE6(m, f) \
   NPT_REGISTER_OVERRIDE(id3d12device6, m, f); \
   NPT_REGISTER_OVERRIDE_D3D12_DEVICE7(m, f)
#define NPT_REGISTER_OVERRIDE_D3D12_DEVICE5(m, f) \
   NPT_REGISTER_OVERRIDE(id3d12device5, m, f); \
   NPT_REGISTER_OVERRIDE_D3D12_DEVICE6(m, f)
#define NPT_REGISTER_OVERRIDE_D3D12_DEVICE4(m, f) \
   NPT_REGISTER_OVERRIDE(id3d12device4, m, f); \
   NPT_REGISTER_OVERRIDE_D3D12_DEVICE5(m, f)
#define NPT_REGISTER_OVERRIDE_D3D12_DEVICE3(m, f) \
   NPT_REGISTER_OVERRIDE(id3d12device3, m, f); \
   NPT_REGISTER_OVERRIDE_D3D12_DEVICE4(m, f)
#define NPT_REGISTER_OVERRIDE_D3D12_DEVICE2(m, f) \
   NPT_REGISTER_OVERRIDE(id3d12device2, m, f); \
   NPT_REGISTER_OVERRIDE_D3D12_DEVICE3(m, f)
#define NPT_REGISTER_OVERRIDE_D3D12_DEVICE1(m, f) \
   NPT_REGISTER_OVERRIDE(id3d12device1, m, f); \
   NPT_REGISTER_OVERRIDE_D3D12_DEVICE2(m, f)
#define NPT_REGISTER_OVERRIDE_D3D12_DEVICE(m, f) \
   NPT_REGISTER_OVERRIDE(id3d12device, m, f); \
   NPT_REGISTER_OVERRIDE_D3D12_DEVICE1(m, f)

/* ------------------------------------------------------------------ */
/* immutable-getter caches                                             */
/* ------------------------------------------------------------------ */

/* Handle increment sizes are a property of the host implementation
 * (identical for every device the VM can create); 0 = not yet
 * fetched (real increments are never 0 for valid types). */
#define NPT_D3D12_DESCRIPTOR_HEAP_TYPES 4
static _Atomic UINT
   dev12_increment_size_cache[NPT_D3D12_DESCRIPTOR_HEAP_TYPES];

static UINT NPT_STDMETHODCALLTYPE
dev12_GetDescriptorHandleIncrementSize_override(
   void *self, D3D12_DESCRIPTOR_HEAP_TYPE DescriptorHeapType)
{
   if ((unsigned)DescriptorHeapType >= NPT_D3D12_DESCRIPTOR_HEAP_TYPES) {
      return npt_id3d12device_default_GetDescriptorHandleIncrementSize(
         self, DescriptorHeapType);
   }
   UINT cached = atomic_load_explicit(
      &dev12_increment_size_cache[DescriptorHeapType],
      memory_order_relaxed);
   if (cached)
      return cached;
   UINT size = npt_id3d12device_default_GetDescriptorHandleIncrementSize(
      self, DescriptorHeapType);
   if (size)
      atomic_store_explicit(
         &dev12_increment_size_cache[DescriptorHeapType], size,
         memory_order_relaxed);
   return size;
}

/* Descriptor heap handle starts are immutable per heap; fetched once
 * and answered guest-side afterwards. */
static const GUID *const descriptorheap12_tiers[] = {
   &NPT_IID_ID3D12DescriptorHeap, NULL,
};

struct npt_d3d12_descriptorheap_aux {
   D3D12_CPU_DESCRIPTOR_HANDLE cpu_start;
   D3D12_GPU_DESCRIPTOR_HANDLE gpu_start;
   _Atomic bool cpu_valid;
   _Atomic bool gpu_valid;
};

static void
npt_d3d12_descriptorheap_aux_destroy(void *aux_raw)
{
   free(aux_raw);
}

static void
npt_d3d12_descriptorheap_aux_init(struct npt_com_base *com,
                                  struct npt_device *dev, uint64_t host_id)
{
   struct npt_d3d12_descriptorheap_aux *aux = com->aux;
   atomic_store_explicit(&aux->cpu_valid, false, memory_order_relaxed);
   atomic_store_explicit(&aux->gpu_valid, false, memory_order_relaxed);
   com->aux_destroy = npt_d3d12_descriptorheap_aux_destroy;
   (void)dev; (void)host_id;
}

static struct npt_d3d12_descriptorheap_aux *
descriptorheap12_aux(void *self)
{
   struct npt_com_base *com = self;
   if (!com || com->aux_destroy != npt_d3d12_descriptorheap_aux_destroy)
      return NULL;
   return com->aux;
}

static D3D12_CPU_DESCRIPTOR_HANDLE * NPT_STDMETHODCALLTYPE
descriptorheap12_GetCPUDescriptorHandleForHeapStart_override(
   void *self, D3D12_CPU_DESCRIPTOR_HANDLE *_ret_out)
{
   struct npt_d3d12_descriptorheap_aux *aux = descriptorheap12_aux(self);
   if (!aux) {
      return npt_id3d12descriptorheap_default_GetCPUDescriptorHandleForHeapStart(
         self, _ret_out);
   }
   if (atomic_load_explicit(&aux->cpu_valid, memory_order_acquire)) {
      *_ret_out = aux->cpu_start;
      return _ret_out;
   }
   npt_id3d12descriptorheap_default_GetCPUDescriptorHandleForHeapStart(
      self, _ret_out);
   /* Racing first calls store the same host answer. */
   aux->cpu_start = *_ret_out;
   atomic_store_explicit(&aux->cpu_valid, true, memory_order_release);
   return _ret_out;
}

static D3D12_GPU_DESCRIPTOR_HANDLE * NPT_STDMETHODCALLTYPE
descriptorheap12_GetGPUDescriptorHandleForHeapStart_override(
   void *self, D3D12_GPU_DESCRIPTOR_HANDLE *_ret_out)
{
   struct npt_d3d12_descriptorheap_aux *aux = descriptorheap12_aux(self);
   if (!aux) {
      return npt_id3d12descriptorheap_default_GetGPUDescriptorHandleForHeapStart(
         self, _ret_out);
   }
   if (atomic_load_explicit(&aux->gpu_valid, memory_order_acquire)) {
      *_ret_out = aux->gpu_start;
      return _ret_out;
   }
   npt_id3d12descriptorheap_default_GetGPUDescriptorHandleForHeapStart(
      self, _ret_out);
   aux->gpu_start = *_ret_out;
   atomic_store_explicit(&aux->gpu_valid, true, memory_order_release);
   return _ret_out;
}

static HRESULT
dev12_check_feature_levels(void *self,
                           D3D12_FEATURE_DATA_FEATURE_LEVELS *fl)
{
   struct npt_device *dev = npt_com_self_device(self);
   const UINT n = fl->NumFeatureLevels;
   if (n && !fl->pFeatureLevelsRequested)
      return NPT_E_INVALIDARG;

   /* Wire layout: [struct][requested levels array].  The struct's
    * pointer field travels as NULL; the host override rebuilds it from
    * the appended array before calling the host. */
   const size_t total = sizeof(*fl) + (size_t)n * sizeof(D3D_FEATURE_LEVEL);
   uint8_t *tmp = malloc(total);
   if (!tmp)
      return NPT_E_OUTOFMEMORY;

   memcpy(tmp, fl, sizeof(*fl));
   ((D3D12_FEATURE_DATA_FEATURE_LEVELS *)tmp)->pFeatureLevelsRequested = NULL;
   if (n)
      memcpy(tmp + sizeof(*fl), fl->pFeatureLevelsRequested,
             (size_t)n * sizeof(D3D_FEATURE_LEVEL));

   HRESULT hr = npt_call_ID3D12Device_CheckFeatureSupport(
      npt_device_method_ring(dev), npt_com_self_id(self),
      D3D12_FEATURE_FEATURE_LEVELS, tmp, (UINT)total);

   if (NPT_SUCCEEDED(hr))
      fl->MaxSupportedFeatureLevel =
         ((D3D12_FEATURE_DATA_FEATURE_LEVELS *)tmp)->MaxSupportedFeatureLevel;

   free(tmp);
   return hr;
}

/* What the host has but Neptune cannot carry (see the top of the file). */
static void
dev12_mask_features(D3D12_FEATURE Feature, void *data, UINT size)
{
   switch (Feature) {
   case D3D12_FEATURE_D3D12_OPTIONS5:
      if (size == sizeof(D3D12_FEATURE_DATA_D3D12_OPTIONS5))
         ((D3D12_FEATURE_DATA_D3D12_OPTIONS5 *)data)->RaytracingTier =
            D3D12_RAYTRACING_TIER_NOT_SUPPORTED;
      break;
   case D3D12_FEATURE_D3D12_OPTIONS21:
      if (size == sizeof(D3D12_FEATURE_DATA_D3D12_OPTIONS21))
         ((D3D12_FEATURE_DATA_D3D12_OPTIONS21 *)data)->WorkGraphsTier =
            D3D12_WORK_GRAPHS_TIER_NOT_SUPPORTED;
      break;
   case D3D12_FEATURE_EXISTING_HEAPS:
      if (size == sizeof(D3D12_FEATURE_DATA_EXISTING_HEAPS))
         ((D3D12_FEATURE_DATA_EXISTING_HEAPS *)data)->Supported = 0;
      break;
   default:
      break;
   }
}

static HRESULT NPT_STDMETHODCALLTYPE
dev12_CheckFeatureSupport_override(void *self,
                                   D3D12_FEATURE Feature,
                                   void *pFeatureSupportData,
                                   UINT FeatureSupportDataSize)
{
   if (!pFeatureSupportData || !FeatureSupportDataSize)
      return NPT_E_INVALIDARG;

   switch (Feature) {
   case D3D12_FEATURE_FEATURE_LEVELS:
      if (FeatureSupportDataSize !=
          sizeof(D3D12_FEATURE_DATA_FEATURE_LEVELS))
         return NPT_E_INVALIDARG;
      return dev12_check_feature_levels(self, pFeatureSupportData);

   case D3D12_FEATURE_PROTECTED_RESOURCE_SESSION_TYPES:
   case D3D12_FEATURE_QUERY_META_COMMAND:
      /* Both embed guest pointers; no consumer yet.  Reject rather
       * than ship garbage pointers to the host. */
      npt_log("CheckFeatureSupport: unsupported pointer-bearing feature "
              "%u rejected", (unsigned)Feature);
      return NPT_E_INVALIDARG;

   default: {
      /* Pointer-free feature structs are a plain in/out byte blob. */
      struct npt_device *dev = npt_com_self_device(self);
      HRESULT hr = npt_call_ID3D12Device_CheckFeatureSupport(
         npt_device_method_ring(dev), npt_com_self_id(self),
         Feature, pFeatureSupportData, FeatureSupportDataSize);
      if (NPT_SUCCEEDED(hr))
         dev12_mask_features(Feature, pFeatureSupportData,
                             FeatureSupportDataSize);
      return hr;
   }
   }
}

/* ==========================================================================
 * GetCopyableFootprints: outputs the app left NULL
 * ========================================================================== */

/* Outputs of one footprint query: the three per-subresource arrays and the
 * total, with scratch space standing in for those the app did not pass. */
struct dev12_footprint_outputs {
   D3D12_PLACED_SUBRESOURCE_FOOTPRINT *layouts;
   UINT *rows;
   UINT64 *row_sizes;
   UINT64 *total;
   void *scratch;
   UINT64 scratch_total;
};

static bool
dev12_footprint_outputs_init(struct dev12_footprint_outputs *out,
                             UINT NumSubresources,
                             D3D12_PLACED_SUBRESOURCE_FOOTPRINT *pLayouts,
                             UINT *pNumRows,
                             UINT64 *pRowSizeInBytes,
                             UINT64 *pTotalBytes)
{
   out->layouts = pLayouts;
   out->rows = pNumRows;
   out->row_sizes = pRowSizeInBytes;
   out->total = pTotalBytes ? pTotalBytes : &out->scratch_total;
   out->scratch = NULL;

   const size_t n = NumSubresources;
   const size_t layouts_bytes = pLayouts ? 0 : n * sizeof(*pLayouts);
   const size_t sizes_bytes = pRowSizeInBytes ? 0 : n * sizeof(*pRowSizeInBytes);
   const size_t rows_bytes = pNumRows ? 0 : n * sizeof(*pNumRows);
   if (!layouts_bytes && !sizes_bytes && !rows_bytes)
      return true;

   /* Widest alignment first, so each array stays naturally aligned. */
   uint8_t *at = malloc(layouts_bytes + sizes_bytes + rows_bytes);
   if (!at)
      return false;
   out->scratch = at;
   if (layouts_bytes) {
      out->layouts = (D3D12_PLACED_SUBRESOURCE_FOOTPRINT *)at;
      at += layouts_bytes;
   }
   if (sizes_bytes) {
      out->row_sizes = (UINT64 *)at;
      at += sizes_bytes;
   }
   if (rows_bytes)
      out->rows = (UINT *)at;
   return true;
}

/* No room for the outputs the host always sends: the call cannot cross,
 * so answer as D3D12 does for a description it cannot lay out. */
static void
dev12_footprint_outputs_invalid(UINT NumSubresources,
                                D3D12_PLACED_SUBRESOURCE_FOOTPRINT *pLayouts,
                                UINT *pNumRows,
                                UINT64 *pRowSizeInBytes,
                                UINT64 *pTotalBytes)
{
   const size_t n = NumSubresources;
   if (pLayouts)
      memset(pLayouts, 0xff, n * sizeof(*pLayouts));
   if (pNumRows)
      memset(pNumRows, 0xff, n * sizeof(*pNumRows));
   if (pRowSizeInBytes)
      memset(pRowSizeInBytes, 0xff, n * sizeof(*pRowSizeInBytes));
   if (pTotalBytes)
      *pTotalBytes = UINT64_MAX;
}

static void NPT_STDMETHODCALLTYPE
dev12_GetCopyableFootprints_override(void *self,
                                     const D3D12_RESOURCE_DESC *pResourceDesc,
                                     UINT FirstSubresource,
                                     UINT NumSubresources,
                                     UINT64 BaseOffset,
                                     D3D12_PLACED_SUBRESOURCE_FOOTPRINT *pLayouts,
                                     UINT *pNumRows,
                                     UINT64 *pRowSizeInBytes,
                                     UINT64 *pTotalBytes)
{
   struct dev12_footprint_outputs out;
   if (!dev12_footprint_outputs_init(&out, NumSubresources, pLayouts, pNumRows,
                                     pRowSizeInBytes, pTotalBytes)) {
      dev12_footprint_outputs_invalid(NumSubresources, pLayouts, pNumRows,
                                      pRowSizeInBytes, pTotalBytes);
      return;
   }
   npt_call_ID3D12Device_GetCopyableFootprints(
      npt_com_self_ring(self), npt_com_self_id(self), pResourceDesc,
      FirstSubresource, NumSubresources, BaseOffset, out.layouts, out.rows,
      out.row_sizes, out.total);
   free(out.scratch);
}

static void NPT_STDMETHODCALLTYPE
dev12_GetCopyableFootprints1_override(void *self,
                                      const D3D12_RESOURCE_DESC1 *pResourceDesc,
                                      UINT FirstSubresource,
                                      UINT NumSubresources,
                                      UINT64 BaseOffset,
                                      D3D12_PLACED_SUBRESOURCE_FOOTPRINT *pLayouts,
                                      UINT *pNumRows,
                                      UINT64 *pRowSizeInBytes,
                                      UINT64 *pTotalBytes)
{
   struct dev12_footprint_outputs out;
   if (!dev12_footprint_outputs_init(&out, NumSubresources, pLayouts, pNumRows,
                                     pRowSizeInBytes, pTotalBytes)) {
      dev12_footprint_outputs_invalid(NumSubresources, pLayouts, pNumRows,
                                      pRowSizeInBytes, pTotalBytes);
      return;
   }
   npt_call_ID3D12Device8_GetCopyableFootprints1(
      npt_com_self_ring(self), npt_com_self_id(self), pResourceDesc,
      FirstSubresource, NumSubresources, BaseOffset, out.layouts, out.rows,
      out.row_sizes, out.total);
   free(out.scratch);
}

/* ==========================================================================
 * Shmem-backed CPU-visible heaps (persistent-map path)
 * ========================================================================== */

/* Heap sizes are 64 KiB multiples (also page-aligned by construction:
 * 64 KiB is a multiple of every guest page size we run on). */
#define NPT_D3D12_HEAP_ALIGN(v) (((uint64_t)(v) + 0xFFFFull) & ~0xFFFFull)

static bool
dev12_iid_equal(const GUID *a, const GUID *b)
{
   return a && b && memcmp(a, b, sizeof(GUID)) == 0;
}

/* CPU-visible per Windows semantics: UPLOAD, READBACK, and CUSTOM
 * with a write-back / write-combine CPU page property. */
static bool
dev12_heap_props_cpu_visible(const D3D12_HEAP_PROPERTIES *p)
{
   if (!p)
      return false;
   switch (p->Type) {
   case D3D12_HEAP_TYPE_UPLOAD:
   case D3D12_HEAP_TYPE_READBACK:
      return true;
   case D3D12_HEAP_TYPE_CUSTOM:
      return p->CPUPageProperty == D3D12_CPU_PAGE_PROPERTY_WRITE_BACK ||
             p->CPUPageProperty == D3D12_CPU_PAGE_PROPERTY_WRITE_COMBINE;
   default:
      return false;
   }
}

static bool
dev12_riid_is_resource(const IID *riid)
{
   return dev12_iid_equal(riid, &NPT_IID_ID3D12Resource) ||
          dev12_iid_equal(riid, &NPT_IID_ID3D12Resource1) ||
          dev12_iid_equal(riid, &NPT_IID_ID3D12Resource2);
}

static bool
dev12_committed_shmem_candidate(const D3D12_HEAP_PROPERTIES *props,
                                D3D12_HEAP_FLAGS heap_flags,
                                D3D12_RESOURCE_DIMENSION dimension,
                                uint64_t width,
                                const IID *riid, void **ppv)
{
   return ppv && width > 0 &&
          dimension == D3D12_RESOURCE_DIMENSION_BUFFER &&
          dev12_heap_props_cpu_visible(props) &&
          !(heap_flags & D3D12_HEAP_FLAG_ALLOW_WRITE_WATCH) &&
          dev12_riid_is_resource(riid);
}

/* Committed CPU-visible buffer -> hidden shmem heap + placed resource
 * at offset 0.  `desc` is D3D12_RESOURCE_DESC or (is_desc1)
 * D3D12_RESOURCE_DESC1; is_desc1 callers are >= ID3D12Device8 so the
 * CreatePlacedResource1 thunk is live host-side. */
static HRESULT
dev12_committed_shmem_create(void *self,
                             const D3D12_HEAP_PROPERTIES *props,
                             D3D12_HEAP_FLAGS heap_flags,
                             const void *desc, bool is_desc1,
                             uint64_t width,
                             D3D12_RESOURCE_STATES initial_state,
                             const D3D12_CLEAR_VALUE *clear,
                             const IID *riid, void **ppv)
{
   const uint64_t size = NPT_D3D12_HEAP_ALIGN(width);

   D3D12_HEAP_DESC app_desc;
   memset(&app_desc, 0, sizeof(app_desc));
   app_desc.SizeInBytes = size;
   app_desc.Properties = *props;
   app_desc.Alignment = 0;
   app_desc.Flags = heap_flags;

   void *heap = npt_d3d12_heap_create_shmem_backed(self, &NPT_IID_ID3D12Heap,
                                                   size, &app_desc);
   if (!heap)
      return NPT_E_FAIL;

   HRESULT hr;
   if (is_desc1) {
      hr = npt_id3d12device8_default_CreatePlacedResource1(
         self, (ID3D12Heap *)heap, 0, (const D3D12_RESOURCE_DESC1 *)desc,
         initial_state, clear, riid, ppv);
   } else {
      hr = npt_id3d12device_default_CreatePlacedResource(
         self, (ID3D12Heap *)heap, 0, (const D3D12_RESOURCE_DESC *)desc,
         initial_state, clear, riid, ppv);
   }

   if (NPT_FAILED(hr) || !*ppv) {
      npt_log("committed_shmem: CreatePlacedResource failed 0x%08x",
              (unsigned)hr);
      npt_com_default_release(heap);
      return NPT_FAILED(hr) ? hr : NPT_E_FAIL;
   }

   if (!npt_d3d12_resource_bind_shmem_heap(*ppv, heap, /*heap_offset=*/0,
                                           width, props, heap_flags)) {
      /* No resource aux to carry the fast path -- undo and let the
       * caller fall back to the plain committed create. */
      npt_log("committed_shmem: resource aux bind failed; undoing");
      npt_com_default_release(*ppv);
      *ppv = NULL;
      npt_com_default_release(heap);
      return NPT_E_FAIL;
   }

   /* bind took its own ref on the hidden heap; drop ours. */
   npt_com_default_release(heap);
   return hr;
}

static HRESULT NPT_STDMETHODCALLTYPE
dev12_CreateCommittedResource_override(
   void *self, const D3D12_HEAP_PROPERTIES *pHeapProperties,
   D3D12_HEAP_FLAGS HeapFlags, const D3D12_RESOURCE_DESC *pDesc,
   D3D12_RESOURCE_STATES InitialResourceState,
   const D3D12_CLEAR_VALUE *pOptimizedClearValue, const IID *riidResource,
   void **ppvResource)
{
   if (pDesc &&
       dev12_committed_shmem_candidate(pHeapProperties, HeapFlags,
                                       pDesc->Dimension, pDesc->Width,
                                       riidResource, ppvResource)) {
      HRESULT hr = dev12_committed_shmem_create(
         self, pHeapProperties, HeapFlags, pDesc, /*is_desc1=*/false,
         pDesc->Width, InitialResourceState, pOptimizedClearValue,
         riidResource, ppvResource);
      if (NPT_SUCCEEDED(hr))
         return hr;
      npt_log("CreateCommittedResource: shmem heap path failed 0x%08x; "
              "using sync-map fallback", (unsigned)hr);
   }
   return npt_id3d12device_default_CreateCommittedResource(
      self, pHeapProperties, HeapFlags, pDesc, InitialResourceState,
      pOptimizedClearValue, riidResource, ppvResource);
}

static HRESULT NPT_STDMETHODCALLTYPE
dev12_CreateCommittedResource1_override(
   void *self, const D3D12_HEAP_PROPERTIES *pHeapProperties,
   D3D12_HEAP_FLAGS HeapFlags, const D3D12_RESOURCE_DESC *pDesc,
   D3D12_RESOURCE_STATES InitialResourceState,
   const D3D12_CLEAR_VALUE *pOptimizedClearValue,
   ID3D12ProtectedResourceSession *pProtectedSession,
   const IID *riidResource, void **ppvResource)
{
   if (pDesc && !pProtectedSession &&
       dev12_committed_shmem_candidate(pHeapProperties, HeapFlags,
                                       pDesc->Dimension, pDesc->Width,
                                       riidResource, ppvResource)) {
      HRESULT hr = dev12_committed_shmem_create(
         self, pHeapProperties, HeapFlags, pDesc, /*is_desc1=*/false,
         pDesc->Width, InitialResourceState, pOptimizedClearValue,
         riidResource, ppvResource);
      if (NPT_SUCCEEDED(hr))
         return hr;
      npt_log("CreateCommittedResource1: shmem heap path failed 0x%08x; "
              "using sync-map fallback", (unsigned)hr);
   }
   return npt_id3d12device4_default_CreateCommittedResource1(
      self, pHeapProperties, HeapFlags, pDesc, InitialResourceState,
      pOptimizedClearValue, pProtectedSession, riidResource, ppvResource);
}

static HRESULT NPT_STDMETHODCALLTYPE
dev12_CreateCommittedResource2_override(
   void *self, const D3D12_HEAP_PROPERTIES *pHeapProperties,
   D3D12_HEAP_FLAGS HeapFlags, const D3D12_RESOURCE_DESC1 *pDesc,
   D3D12_RESOURCE_STATES InitialResourceState,
   const D3D12_CLEAR_VALUE *pOptimizedClearValue,
   ID3D12ProtectedResourceSession *pProtectedSession,
   const IID *riidResource, void **ppvResource)
{
   if (pDesc && !pProtectedSession &&
       dev12_committed_shmem_candidate(pHeapProperties, HeapFlags,
                                       pDesc->Dimension, pDesc->Width,
                                       riidResource, ppvResource)) {
      HRESULT hr = dev12_committed_shmem_create(
         self, pHeapProperties, HeapFlags, pDesc, /*is_desc1=*/true,
         pDesc->Width, InitialResourceState, pOptimizedClearValue,
         riidResource, ppvResource);
      if (NPT_SUCCEEDED(hr))
         return hr;
      npt_log("CreateCommittedResource2: shmem heap path failed 0x%08x; "
              "using sync-map fallback", (unsigned)hr);
   }
   return npt_id3d12device8_default_CreateCommittedResource2(
      self, pHeapProperties, HeapFlags, pDesc, InitialResourceState,
      pOptimizedClearValue, pProtectedSession, riidResource, ppvResource);
}

/* Buffer-capable CPU heap without WRITE_WATCH, asked for by a heap
 * IID we can wrap.  DENY_BUFFERS heaps can't take our placed buffers
 * -- textures-only CPU heaps stay on the default path.  Alignment >
 * 64 KiB (MSAA) is also rejected. */
static bool
dev12_heap_shmem_candidate(const D3D12_HEAP_DESC *desc, const IID *riid,
                           void **ppv)
{
   return desc && ppv && desc->SizeInBytes > 0 &&
          dev12_heap_props_cpu_visible(&desc->Properties) &&
          !(desc->Flags & D3D12_HEAP_FLAG_ALLOW_WRITE_WATCH) &&
          !(desc->Flags & D3D12_HEAP_FLAG_DENY_BUFFERS) &&
          (desc->Alignment == 0 || desc->Alignment == 0x10000ull) &&
          (dev12_iid_equal(riid, &NPT_IID_ID3D12Heap) ||
           dev12_iid_equal(riid, &NPT_IID_ID3D12Heap1));
}

static void *
dev12_heap_shmem_create(void *self, const D3D12_HEAP_DESC *pDesc,
                        const IID *riid)
{
   D3D12_HEAP_DESC app_desc = *pDesc;
   app_desc.SizeInBytes = NPT_D3D12_HEAP_ALIGN(pDesc->SizeInBytes);
   return npt_d3d12_heap_create_shmem_backed(self, riid,
                                             app_desc.SizeInBytes,
                                             &app_desc);
}

static HRESULT NPT_STDMETHODCALLTYPE
dev12_CreateHeap_override(void *self, const D3D12_HEAP_DESC *pDesc,
                          const IID *riid, void **ppvHeap)
{
   if (dev12_heap_shmem_candidate(pDesc, riid, ppvHeap)) {
      void *heap = dev12_heap_shmem_create(self, pDesc, riid);
      if (heap) {
         *ppvHeap = heap;
         return NPT_S_OK;
      }
      npt_log("CreateHeap: shmem heap path failed; using host heap");
   }
   return npt_id3d12device_default_CreateHeap(self, pDesc, riid, ppvHeap);
}

static HRESULT NPT_STDMETHODCALLTYPE
dev12_CreateHeap1_override(void *self, const D3D12_HEAP_DESC *pDesc,
                           ID3D12ProtectedResourceSession *pProtectedSession,
                           const IID *riid, void **ppvHeap)
{
   if (!pProtectedSession && dev12_heap_shmem_candidate(pDesc, riid,
                                                        ppvHeap)) {
      void *heap = dev12_heap_shmem_create(self, pDesc, riid);
      if (heap) {
         *ppvHeap = heap;
         return NPT_S_OK;
      }
      npt_log("CreateHeap1: shmem heap path failed; using host heap");
   }
   return npt_id3d12device4_default_CreateHeap1(self, pDesc,
                                                pProtectedSession, riid,
                                                ppvHeap);
}

/* After a successful placed create on an app-visible shmem heap,
 * record the backing on the resource: pins the heap wrapper for
 * lifetime and (buffers only) enables the zero-wire Map fast path. */
static void
dev12_record_placed_on_shmem_heap(void *heap, uint64_t heap_offset,
                                  uint64_t buffer_width, HRESULT hr,
                                  void **ppv)
{
   if (NPT_FAILED(hr) || !ppv || !*ppv || !heap)
      return;
   struct npt_d3d12_heap_aux *haux = npt_d3d12_heap_aux_cast(heap);
   if (!haux || !haux->shmem_heap)
      return;
   if (!npt_d3d12_resource_bind_shmem_heap(*ppv, heap, heap_offset,
                                           buffer_width,
                                           &haux->app_desc.Properties,
                                           haux->app_desc.Flags)) {
      npt_log("CreatePlacedResource: resource on shmem heap has no aux; "
              "heap lifetime NOT pinned by this resource");
   }
}

static HRESULT NPT_STDMETHODCALLTYPE
dev12_CreatePlacedResource_override(
   void *self, ID3D12Heap *pHeap, UINT64 HeapOffset,
   const D3D12_RESOURCE_DESC *pDesc, D3D12_RESOURCE_STATES InitialState,
   const D3D12_CLEAR_VALUE *pOptimizedClearValue, const IID *riid,
   void **ppvResource)
{
   HRESULT hr = npt_id3d12device_default_CreatePlacedResource(
      self, pHeap, HeapOffset, pDesc, InitialState, pOptimizedClearValue,
      riid, ppvResource);
   dev12_record_placed_on_shmem_heap(
      pHeap, HeapOffset,
      (pDesc && pDesc->Dimension == D3D12_RESOURCE_DIMENSION_BUFFER)
         ? pDesc->Width : 0,
      hr, ppvResource);
   return hr;
}

static HRESULT NPT_STDMETHODCALLTYPE
dev12_CreatePlacedResource1_override(
   void *self, ID3D12Heap *pHeap, UINT64 HeapOffset,
   const D3D12_RESOURCE_DESC1 *pDesc, D3D12_RESOURCE_STATES InitialState,
   const D3D12_CLEAR_VALUE *pOptimizedClearValue, const IID *riid,
   void **ppvResource)
{
   HRESULT hr = npt_id3d12device8_default_CreatePlacedResource1(
      self, pHeap, HeapOffset, pDesc, InitialState, pOptimizedClearValue,
      riid, ppvResource);
   dev12_record_placed_on_shmem_heap(
      pHeap, HeapOffset,
      (pDesc && pDesc->Dimension == D3D12_RESOURCE_DIMENSION_BUFFER)
         ? pDesc->Width : 0,
      hr, ppvResource);
   return hr;
}

static HRESULT NPT_STDMETHODCALLTYPE
dev12_CreateCommandQueue_override(void *self,
                                  const D3D12_COMMAND_QUEUE_DESC *pDesc,
                                  const IID *riid, void **ppCommandQueue)
{
   HRESULT hr = npt_id3d12device_default_CreateCommandQueue(
      self, pDesc, riid, ppCommandQueue);
   if (NPT_SUCCEEDED(hr) && pDesc && ppCommandQueue && *ppCommandQueue)
      npt_com_pin_queue_ring(*ppCommandQueue,
                             pDesc->Type == D3D12_COMMAND_LIST_TYPE_DIRECT);
   return hr;
}

/* ------------------------------------------------------------------ */
/* pipeline state streams                                              */
/* ------------------------------------------------------------------ */

/* Whether the encoder can send this stream (npt_pso_stream_next walks
 * it): every subobject of a known type, inside SizeInBytes, and no type
 * twice, which D3D12 rejects too. */
static bool
pso_stream_valid(const D3D12_PIPELINE_STATE_STREAM_DESC *desc)
{
   if (!desc || !desc->pPipelineStateSubobjectStream || !desc->SizeInBytes)
      return false;
   uint32_t seen = 0;
   size_t offset = 0;
   UINT type;
   const void *subobject;
   int step;
   while ((step = npt_pso_stream_next(desc, &offset, &type, &subobject)) > 0) {
      if (seen & (1u << type))
         return false;
      seen |= 1u << type;
   }
   return step == 0;
}

static HRESULT NPT_STDMETHODCALLTYPE
dev12_CreatePipelineState_override(void *self,
                                   const D3D12_PIPELINE_STATE_STREAM_DESC *pDesc,
                                   const IID *riid, void **ppPipelineState)
{
   if (!pso_stream_valid(pDesc)) {
      npt_log("CreatePipelineState: invalid pipeline state stream");
      if (ppPipelineState)
         *ppPipelineState = NULL;
      return NPT_E_INVALIDARG;
   }
   /* A compilation holds its ring on the host for up to a second: on the
    * ring the thread's other calls share (primary for D3D12), the render
    * thread's calls waited behind every pipeline the engine precompiled
    * (THE FINALS: about a frame a second while it preloaded shaders).
    * The call is synchronous under multi-ring, so the pipeline exists on
    * the host before any other ring can use it. */
   struct npt_device *dev = npt_com_self_device(self);
   struct npt_ring *ring = npt_tls_side_ring(dev);
   if (!ring)
      return npt_id3d12device2_default_CreatePipelineState(self, pDesc, riid,
                                                           ppPipelineState);
   void *raw = NULL;
   const HRESULT hr = npt_call_ID3D12Device2_CreatePipelineState(
      ring, npt_com_self_id(self), pDesc, riid,
      ppPipelineState ? &raw : NULL);
   if (ppPipelineState) {
      *ppPipelineState =
         NPT_SUCCEEDED(hr) && raw
            ? (void *)npt_com_get_or_wrap(dev, riid, (uint64_t)(uintptr_t)raw,
                                          (struct npt_com_base *)self)
            : NULL;
   }
   return hr;
}

static HRESULT NPT_STDMETHODCALLTYPE
pipelinelibrary12_LoadPipeline_override(
   void *self, const WCHAR *pName,
   const D3D12_PIPELINE_STATE_STREAM_DESC *pDesc, const IID *riid,
   void **ppPipelineState)
{
   if (!pso_stream_valid(pDesc)) {
      npt_log("LoadPipeline: invalid pipeline state stream");
      if (ppPipelineState)
         *ppPipelineState = NULL;
      return NPT_E_INVALIDARG;
   }
   return npt_id3d12pipelinelibrary1_default_LoadPipeline(self, pName, pDesc,
                                                          riid,
                                                          ppPipelineState);
}

static HRESULT NPT_STDMETHODCALLTYPE
dev12_GetDeviceRemovedReason_override(void *self)
{
   const struct npt_device *dev = npt_com_self_device(self);
   if (dev && dev->renderer && npt_renderer_is_lost(dev->renderer))
      return NPT_DXGI_ERROR_DRIVER_INTERNAL_ERROR;
   return npt_id3d12device_default_GetDeviceRemovedReason(self);
}

static HRESULT NPT_STDMETHODCALLTYPE
dev12_CreateCommandQueue1_override(void *self,
                                   const D3D12_COMMAND_QUEUE_DESC *pDesc,
                                   const IID *CreatorID, const IID *riid,
                                   void **ppCommandQueue)
{
   HRESULT hr = npt_id3d12device9_default_CreateCommandQueue1(
      self, pDesc, CreatorID, riid, ppCommandQueue);
   if (NPT_SUCCEEDED(hr) && pDesc && ppCommandQueue && *ppCommandQueue)
      npt_com_pin_queue_ring(*ppCommandQueue,
                             pDesc->Type == D3D12_COMMAND_LIST_TYPE_DIRECT);
   return hr;
}

/* ------------------------------------------------------------------ */
/* methods answered here (see the top of the file)                     */
/* ------------------------------------------------------------------ */

static HRESULT NPT_STDMETHODCALLTYPE
dev12_OpenExistingHeapFromAddress_override(void *self, const void *pAddress,
                                           const IID *riid, void **ppvHeap)
{
   (void)self; (void)pAddress; (void)riid;
   if (ppvHeap)
      *ppvHeap = NULL;
   return NPT_E_NOTIMPL;
}

static HRESULT NPT_STDMETHODCALLTYPE
dev12_OpenExistingHeapFromAddress1_override(void *self, const void *pAddress,
                                            SIZE_T size, const IID *riid,
                                            void **ppvHeap)
{
   (void)size;
   return dev12_OpenExistingHeapFromAddress_override(self, pAddress, riid,
                                                     ppvHeap);
}

static HRESULT NPT_STDMETHODCALLTYPE
dev12_OpenExistingHeapFromFileMapping_override(void *self,
                                               HANDLE hFileMapping,
                                               const IID *riid, void **ppvHeap)
{
   (void)self; (void)hFileMapping; (void)riid;
   if (ppvHeap)
      *ppvHeap = NULL;
   return NPT_E_NOTIMPL;
}

static HRESULT NPT_STDMETHODCALLTYPE
dev12_EnqueueMakeResident_override(void *self, D3D12_RESIDENCY_FLAGS Flags,
                                   UINT NumObjects, ID3D12Pageable **ppObjects,
                                   ID3D12Fence *pFenceToSignal,
                                   UINT64 FenceValueToSignal)
{
   (void)Flags;
   if (!pFenceToSignal)
      return NPT_E_INVALIDARG;
   HRESULT hr = NPT_S_OK;
   if (NumObjects)
      hr = npt_id3d12device_default_MakeResident(self, NumObjects,
                                                 ppObjects);
   if (NPT_FAILED(hr))
      return hr;
   const struct npt_id3d12fence_client_vtbl *fv =
      (const void *)((struct npt_com_base *)pFenceToSignal)->lpVtbl;
   return fv->Signal(pFenceToSignal, FenceValueToSignal);
}

static HRESULT NPT_STDMETHODCALLTYPE
dev12_CreateLifetimeTracker_override(void *self, ID3D12LifetimeOwner *pOwner,
                                     const IID *riid, void **ppvTracker)
{
   (void)self; (void)pOwner; (void)riid;
   if (ppvTracker)
      *ppvTracker = NULL;
   return NPT_E_NOTIMPL;
}

static HRESULT NPT_STDMETHODCALLTYPE
dev12_EnumerateMetaCommands_override(void *self, UINT *pNumMetaCommands,
                                     D3D12_META_COMMAND_DESC *pDescs)
{
   (void)self; (void)pDescs;
   if (!pNumMetaCommands)
      return NPT_E_INVALIDARG;
   *pNumMetaCommands = 0;
   return NPT_S_OK;
}

static HRESULT NPT_STDMETHODCALLTYPE
dev12_EnumerateMetaCommandParameters_override(
   void *self, const GUID *CommandId, D3D12_META_COMMAND_PARAMETER_STAGE Stage,
   UINT *pTotalStructureSizeInBytes, UINT *pParameterCount,
   D3D12_META_COMMAND_PARAMETER_DESC *pParameterDescs)
{
   (void)self; (void)CommandId; (void)Stage; (void)pParameterDescs;
   if (pTotalStructureSizeInBytes)
      *pTotalStructureSizeInBytes = 0;
   if (pParameterCount)
      *pParameterCount = 0;
   return NPT_E_INVALIDARG;
}

static HRESULT NPT_STDMETHODCALLTYPE
dev12_CreateMetaCommand_override(void *self, const GUID *CommandId,
                                 UINT NodeMask,
                                 const void *pCreationParametersData,
                                 SIZE_T CreationParametersDataSizeInBytes,
                                 const IID *riid, void **ppMetaCommand)
{
   (void)self; (void)CommandId; (void)NodeMask;
   (void)pCreationParametersData; (void)CreationParametersDataSizeInBytes;
   (void)riid;
   if (ppMetaCommand)
      *ppMetaCommand = NULL;
   return NPT_E_INVALIDARG;
}

static HRESULT NPT_STDMETHODCALLTYPE
dev12_CreateStateObject_override(void *self,
                                 const D3D12_STATE_OBJECT_DESC *pDesc,
                                 const IID *riid, void **ppStateObject)
{
   (void)self; (void)pDesc; (void)riid;
   if (ppStateObject)
      *ppStateObject = NULL;
   return NPT_E_NOTIMPL;
}

static HRESULT NPT_STDMETHODCALLTYPE
dev12_AddToStateObject_override(void *self,
                                const D3D12_STATE_OBJECT_DESC *pAddition,
                                ID3D12StateObject *pStateObjectToGrowFrom,
                                const IID *riid, void **ppNewStateObject)
{
   (void)self; (void)pAddition; (void)pStateObjectToGrowFrom; (void)riid;
   if (ppNewStateObject)
      *ppNewStateObject = NULL;
   return NPT_E_NOTIMPL;
}

static HRESULT NPT_STDMETHODCALLTYPE
dev12_SetBackgroundProcessingMode_override(
   void *self, D3D12_BACKGROUND_PROCESSING_MODE Mode,
   D3D12_MEASUREMENTS_ACTION MeasurementsAction,
   HANDLE hEventToSignalUponCompletion, BOOL *pbFurtherMeasurementsDesired)
{
   (void)self; (void)Mode; (void)MeasurementsAction;
   if (pbFurtherMeasurementsDesired)
      *pbFurtherMeasurementsDesired = 0;
   if (hEventToSignalUponCompletion)
      SetEvent(hEventToSignalUponCompletion);
   return NPT_S_OK;
}

static const GUID *const device12_tiers[] = {
   &NPT_IID_ID3D12Device,   &NPT_IID_ID3D12Device1,  &NPT_IID_ID3D12Device2,
   &NPT_IID_ID3D12Device3,  &NPT_IID_ID3D12Device4,  &NPT_IID_ID3D12Device5,
   &NPT_IID_ID3D12Device6,  &NPT_IID_ID3D12Device7,  &NPT_IID_ID3D12Device8,
   &NPT_IID_ID3D12Device9,  &NPT_IID_ID3D12Device10, &NPT_IID_ID3D12Device11,
   &NPT_IID_ID3D12Device12, &NPT_IID_ID3D12Device13, &NPT_IID_ID3D12Device14,
   NULL,
};

static const GUID *const pipelinelibrary12_tiers[] = {
   &NPT_IID_ID3D12PipelineLibrary, &NPT_IID_ID3D12PipelineLibrary1, NULL,
};

void
npt_overrides_d3d12_device_init(void)
{
   NPT_REGISTER_OVERRIDE_D3D12_DEVICE(CheckFeatureSupport,
                                      dev12_CheckFeatureSupport_override);
   NPT_REGISTER_OVERRIDE_D3D12_DEVICE(GetCopyableFootprints,
                                      dev12_GetCopyableFootprints_override);
   NPT_REGISTER_OVERRIDE_D3D12_DEVICE8(GetCopyableFootprints1,
                                       dev12_GetCopyableFootprints1_override);
   NPT_REGISTER_OVERRIDE_D3D12_DEVICE(CreateCommandQueue,
                                      dev12_CreateCommandQueue_override);
   NPT_REGISTER_OVERRIDE_D3D12_DEVICE(GetDeviceRemovedReason,
                                      dev12_GetDeviceRemovedReason_override);
   NPT_REGISTER_OVERRIDE_D3D12_DEVICE2(CreatePipelineState,
                                       dev12_CreatePipelineState_override);
   NPT_REGISTER_OVERRIDE(id3d12pipelinelibrary1, LoadPipeline,
                         pipelinelibrary12_LoadPipeline_override);

   NPT_REGISTER_OVERRIDE_D3D12_DEVICE(CreateCommittedResource,
                                      dev12_CreateCommittedResource_override);
   NPT_REGISTER_OVERRIDE_D3D12_DEVICE(CreateHeap,
                                      dev12_CreateHeap_override);
   NPT_REGISTER_OVERRIDE_D3D12_DEVICE(CreatePlacedResource,
                                      dev12_CreatePlacedResource_override);
   NPT_REGISTER_OVERRIDE_D3D12_DEVICE4(
      CreateCommittedResource1, dev12_CreateCommittedResource1_override);
   NPT_REGISTER_OVERRIDE_D3D12_DEVICE4(CreateHeap1,
                                       dev12_CreateHeap1_override);
   NPT_REGISTER_OVERRIDE_D3D12_DEVICE8(
      CreateCommittedResource2, dev12_CreateCommittedResource2_override);
   NPT_REGISTER_OVERRIDE_D3D12_DEVICE8(
      CreatePlacedResource1, dev12_CreatePlacedResource1_override);

   NPT_REGISTER_OVERRIDE_D3D12_DEVICE(
      GetDescriptorHandleIncrementSize,
      dev12_GetDescriptorHandleIncrementSize_override);
   NPT_REGISTER_OVERRIDE(id3d12descriptorheap,
                         GetCPUDescriptorHandleForHeapStart,
                         descriptorheap12_GetCPUDescriptorHandleForHeapStart_override);
   NPT_REGISTER_OVERRIDE(id3d12descriptorheap,
                         GetGPUDescriptorHandleForHeapStart,
                         descriptorheap12_GetGPUDescriptorHandleForHeapStart_override);
   npt_com_register_family(descriptorheap12_tiers,
                           sizeof(struct npt_d3d12_descriptorheap_aux),
                           npt_d3d12_descriptorheap_aux_init);

   NPT_REGISTER_OVERRIDE_D3D12_DEVICE3(
      OpenExistingHeapFromAddress, dev12_OpenExistingHeapFromAddress_override);
   NPT_REGISTER_OVERRIDE_D3D12_DEVICE3(
      OpenExistingHeapFromFileMapping,
      dev12_OpenExistingHeapFromFileMapping_override);
   NPT_REGISTER_OVERRIDE_D3D12_DEVICE13(
      OpenExistingHeapFromAddress1,
      dev12_OpenExistingHeapFromAddress1_override);
   NPT_REGISTER_OVERRIDE_D3D12_DEVICE3(EnqueueMakeResident,
                                       dev12_EnqueueMakeResident_override);
   NPT_REGISTER_OVERRIDE_D3D12_DEVICE5(CreateLifetimeTracker,
                                       dev12_CreateLifetimeTracker_override);
   NPT_REGISTER_OVERRIDE_D3D12_DEVICE5(EnumerateMetaCommands,
                                       dev12_EnumerateMetaCommands_override);
   NPT_REGISTER_OVERRIDE_D3D12_DEVICE5(
      EnumerateMetaCommandParameters,
      dev12_EnumerateMetaCommandParameters_override);
   NPT_REGISTER_OVERRIDE_D3D12_DEVICE5(CreateMetaCommand,
                                       dev12_CreateMetaCommand_override);
   NPT_REGISTER_OVERRIDE_D3D12_DEVICE5(CreateStateObject,
                                       dev12_CreateStateObject_override);
   NPT_REGISTER_OVERRIDE_D3D12_DEVICE7(AddToStateObject,
                                       dev12_AddToStateObject_override);
   NPT_REGISTER_OVERRIDE_D3D12_DEVICE6(
      SetBackgroundProcessingMode,
      dev12_SetBackgroundProcessingMode_override);
   NPT_REGISTER_OVERRIDE_D3D12_DEVICE9(CreateCommandQueue1,
                                       dev12_CreateCommandQueue1_override);

   static const struct npt_com_chain device12_chain = { device12_tiers, 14 };
   npt_com_register_chain(&device12_chain);
   static const struct npt_com_chain pipelinelibrary12_chain = {
      pipelinelibrary12_tiers, 1,
   };
   npt_com_register_chain(&pipelinelibrary12_chain);
}
