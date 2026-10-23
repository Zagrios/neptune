/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * D3D11 resource manipulation for the RESOURCE_{UPDATE, MAP, UNMAP}
 * transport commands.  Resolves host resources via the object table,
 * walks the COM vtable for D3D11 Map/Unmap/Update, and (for MAP)
 * stashes sync-map state per (resource, subresource) so the matching
 * UNMAP can write back.
 */

#include "npt_resource.h"

#include <stdlib.h>
#include <string.h>

#include "npt_com.h"
#include "npt_context.h"
#include "npt_transport_defs.h"

#include "neptune-protocol/npt_protocol_host_dispatch_types.h"
#include "neptune-protocol/npt_protocol_host_id3d12resource.h"

/* Obelisk test trace (NPT_TRACE_TEXUPD=<max lines>): every texture update path with the texture's description, to
 * find which path the game's partial texture updates take. Off by default. */
#include <stdio.h>
#include <stdarg.h>
static int npt_texupd_budget = -1;
/* Obelisk test stats (NPT_TRACE_UNMAP=1): bytes copied by UNMAP per 5 s, split by map kind. */
#include <time.h>
static struct { int on; uint64_t t0, n[3], bytes[3], maxb[3]; } npt_unmap_stats = { -1 };

static void
npt_unmap_account(uint32_t access_flags, uint64_t bytes)
{
   if (npt_unmap_stats.on < 0)
      npt_unmap_stats.on = getenv("NPT_TRACE_UNMAP") != NULL;
   if (!npt_unmap_stats.on)
      return;
   const int k = !access_flags ? 0 : (access_flags & NPT_MAP_ACCESS_DISCARD) ? 1 : 2;
   npt_unmap_stats.n[k]++;
   npt_unmap_stats.bytes[k] += bytes;
   if (bytes > npt_unmap_stats.maxb[k])
      npt_unmap_stats.maxb[k] = bytes;
   const uint64_t now = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
   if (!npt_unmap_stats.t0)
      npt_unmap_stats.t0 = now;
   if (now - npt_unmap_stats.t0 >= 5000000000ull) {
      static const char *const names[3] = { "sync", "discard", "no_overwrite" };
      for (int i = 0; i < 3; i++)
         fprintf(stderr, "unmap stats 5 s: %s %llu unmaps, %.1f MiB copied, max %.1f KiB\n", names[i],
                 (unsigned long long)npt_unmap_stats.n[i], npt_unmap_stats.bytes[i] / 1048576.0,
                 npt_unmap_stats.maxb[i] / 1024.0);
      memset(npt_unmap_stats.n, 0, sizeof(npt_unmap_stats.n));
      memset(npt_unmap_stats.bytes, 0, sizeof(npt_unmap_stats.bytes));
      memset(npt_unmap_stats.maxb, 0, sizeof(npt_unmap_stats.maxb));
      npt_unmap_stats.t0 = now;
   }
}


static void
npt_trace_tex(void *resource, const char *fmt, ...)
{
   if (npt_texupd_budget < 0) {
      const char *e = getenv("NPT_TRACE_TEXUPD");
      npt_texupd_budget = e ? atoi(e) : 0;
   }
   if (!npt_texupd_budget || !resource)
      return;
   PFN_ID3D11Resource_GetType get_type =
      NPT_COM_VTBL_FUNC(PFN_ID3D11Resource_GetType, npt_com_vtable(resource), NPT_VTBL_ID3D11Resource_GetType);
   D3D11_RESOURCE_DIMENSION dim = D3D11_RESOURCE_DIMENSION_UNKNOWN;
   get_type(resource, &dim);
   if (dim != D3D11_RESOURCE_DIMENSION_TEXTURE2D)
      return;
   D3D11_TEXTURE2D_DESC d;
   memset(&d, 0, sizeof(d));
   PFN_ID3D11Texture2D_GetDesc get_desc =
      NPT_COM_VTBL_FUNC(PFN_ID3D11Texture2D_GetDesc, npt_com_vtable(resource), NPT_VTBL_ID3D11Texture2D_GetDesc);
   get_desc(resource, &d);
   char msg[256];
   va_list ap;
   va_start(ap, fmt);
   vsnprintf(msg, sizeof(msg), fmt, ap);
   va_end(ap);
   npt_texupd_budget--;
   fprintf(stderr, "texupd: tex %p %ux%u fmt %u mips %u arr %u usage %u bind 0x%x cpu 0x%x: %s\n", resource,
           d.Width, d.Height, (unsigned)d.Format, d.MipLevels, d.ArraySize, (unsigned)d.Usage, d.BindFlags,
           d.CPUAccessFlags, msg);
}

void
npt_resource_update(struct npt_context *ctx,
                    uint64_t resource_id, uint32_t subresource,
                    uint32_t row_pitch, uint32_t depth_pitch,
                    UNUSED uint32_t byte_size,
                    bool has_box,
                    uint32_t box_left, uint32_t box_top, uint32_t box_front,
                    uint32_t box_right, uint32_t box_bottom, uint32_t box_back,
                    const void *payload)
{
   /* Recover the device and immediate context from the resource.
    * Both calls add refs that we release after UpdateSubresource. */
   void *resource = npt_context_lookup_object(ctx, NULL, resource_id,
                                              NPT_OBJECT_TYPE_IUNKNOWN);
   if (!resource) {
      npt_log("resource_update: NULL resource");
      return;
   }

   /* A D3D12 resource is not an ID3D11DeviceChild: slot 3 of its vtable is
    * GetPrivateData, so the D3D11 path below would call the wrong method
    * entirely and the update would be lost with no error anywhere. Route it
    * through the D3D12 API, which takes the same (subresource, box, pitches)
    * shape. WriteToSubresource requires the resource to be mapped, and the
    * guest's no-pointer Map is local bookkeeping that never reaches the host,
    * so the bracket has to be applied here. */
   if (npt_context_object_is(ctx, resource_id, NPT_OBJECT_TYPE_ID3D12RESOURCE)) {
      PFN_ID3D12Resource_WriteToSubresource wts12 =
         NPT_COM_VTBL_FUNC(PFN_ID3D12Resource_WriteToSubresource,
                           npt_com_vtable(resource),
                           NPT_VTBL_ID3D12Resource_WriteToSubresource);
      if (!wts12) {
         npt_log("resource_update: no D3D12 WriteToSubresource");
         return;
      }
      D3D12_BOX box12;
      const D3D12_BOX *box12_arg = NULL;
      if (has_box) {
         box12.left   = box_left;
         box12.top    = box_top;
         box12.front  = box_front;
         box12.right  = box_right;
         box12.bottom = box_bottom;
         box12.back   = box_back;
         box12_arg = &box12;
      }
      PFN_ID3D12Resource_Map map12 =
         NPT_COM_VTBL_FUNC(PFN_ID3D12Resource_Map, npt_com_vtable(resource),
                           NPT_VTBL_ID3D12Resource_Map);
      PFN_ID3D12Resource_Unmap unmap12 =
         NPT_COM_VTBL_FUNC(PFN_ID3D12Resource_Unmap, npt_com_vtable(resource),
                           NPT_VTBL_ID3D12Resource_Unmap);
      const bool mapped = map12 && !NPT_FAILED(map12(resource, subresource,
                                                     NULL, NULL));
      HRESULT hr = wts12(resource, subresource, box12_arg, payload,
                         row_pitch, depth_pitch);
      if (mapped && unmap12)
         unmap12(resource, subresource, NULL);
      if (NPT_FAILED(hr))
         npt_log("resource_update: D3D12 WriteToSubresource sub %u failed "
                 "0x%08x (mapped=%d pitch=%u/%u)", subresource,
                 (unsigned)hr, (int)mapped, row_pitch, depth_pitch);
      return;
   }

   ID3D11Device *device = NULL;
   PFN_ID3D11DeviceChild_GetDevice get_dev =
      NPT_COM_VTBL_FUNC(PFN_ID3D11DeviceChild_GetDevice,
                        npt_com_vtable(resource),
                        NPT_VTBL_ID3D11DeviceChild_GetDevice);
   get_dev(resource, &device);
   if (!device) {
      npt_log("resource_update: GetDevice returned NULL");
      return;
   }

   ID3D11DeviceContext *imm_ctx = NULL;
   PFN_ID3D11Device_GetImmediateContext get_ctx =
      NPT_COM_VTBL_FUNC(PFN_ID3D11Device_GetImmediateContext,
                        npt_com_vtable(device),
                        NPT_VTBL_ID3D11Device_GetImmediateContext);
   get_ctx(device, &imm_ctx);
   if (!imm_ctx) {
      npt_log("resource_update: GetImmediateContext returned NULL");
      npt_com_release(device);
      return;
   }

   D3D11_BOX box;
   const D3D11_BOX *box_arg = NULL;
   if (has_box) {
      box.left   = box_left;
      box.top    = box_top;
      box.front  = box_front;
      box.right  = box_right;
      box.bottom = box_bottom;
      box.back   = box_back;
      box_arg = &box;
   }

   PFN_ID3D11DeviceContext_UpdateSubresource update =
      NPT_COM_VTBL_FUNC(PFN_ID3D11DeviceContext_UpdateSubresource,
                        npt_com_vtable(imm_ctx),
                        NPT_VTBL_ID3D11DeviceContext_UpdateSubresource);
   npt_trace_tex(resource, "update sub %u box %s %u,%u-%u,%u pitch %u bytes %u", subresource, has_box ? "yes" : "no",
                 box_left, box_top, box_right, box_bottom, row_pitch, byte_size);
   update(imm_ctx, resource, subresource, box_arg, payload,
          row_pitch, depth_pitch);

   npt_com_release(imm_ctx);
   npt_com_release(device);
}

/* MAP and UNMAP name their API family with context_id: zero means a
 * D3D12 resource the guest maps directly, non-zero the ID3D11DeviceContext
 * to map it through.  Each family keeps an unrelated method at the slots
 * the other's path calls (ID3D12Resource::Map is slot 8, SetEvictionPriority
 * on an ID3D11Resource), so a context_id disagreeing with what the resource
 * is would call the wrong method and report nothing.  The field still
 * selects -- a resource registered under no precise type answers to either
 * family -- and the object table only vetoes a definite contradiction. */
static bool
npt_resource_family_matches(struct npt_context *ctx, uint64_t resource_id,
                            uint64_t context_id, const char *what)
{
   const npt_object_type want = context_id ? NPT_OBJECT_TYPE_ID3D11RESOURCE
                                           : NPT_OBJECT_TYPE_ID3D12RESOURCE;
   if (npt_context_object_is(ctx, resource_id, want))
      return true;

   npt_log("%s: resource 0x%" PRIx64 " is not a D3D%s resource "
           "(context_id 0x%" PRIx64 ")", what, resource_id,
           context_id ? "11" : "12", context_id);
   return false;
}

/* Returns 0 on invalid flags (causes D3D11 Map to fail). */
static D3D11_MAP
npt_access_flags_to_d3d11_map(uint32_t access_flags)
{
   const bool read = access_flags & NPT_MAP_ACCESS_READ;
   const bool write = access_flags & NPT_MAP_ACCESS_WRITE;

   if (read && write)
      return D3D11_MAP_READ_WRITE;
   if (read)
      return D3D11_MAP_READ;
   if (access_flags & NPT_MAP_ACCESS_DISCARD)
      return D3D11_MAP_WRITE_DISCARD;
   if (access_flags & NPT_MAP_ACCESS_NO_OVERWRITE)
      return D3D11_MAP_WRITE_NO_OVERWRITE;
   if (write)
      return D3D11_MAP_WRITE;

   return (D3D11_MAP)0;
}

/* ---- per-context sync-map bookkeeping --------------------------------- */

static struct npt_sync_map_entry *
npt_sync_map_find(struct npt_context *ctx, uint64_t resource_id,
                  uint32_t subresource)
{
   for (uint32_t i = 0; i < ctx->sync_maps.count; ++i) {
      struct npt_sync_map_entry *e = &ctx->sync_maps.entries[i];
      if (e->resource_id == resource_id && e->subresource == subresource)
         return e;
   }
   return NULL;
}

static struct npt_sync_map_entry *
npt_sync_map_add(struct npt_context *ctx)
{
   if (ctx->sync_maps.count == ctx->sync_maps.cap) {
      uint32_t cap = ctx->sync_maps.cap ? ctx->sync_maps.cap * 2 : 8;
      struct npt_sync_map_entry *e =
         realloc(ctx->sync_maps.entries, cap * sizeof(*e));
      if (!e)
         return NULL;
      ctx->sync_maps.entries = e;
      ctx->sync_maps.cap = cap;
   }
   return &ctx->sync_maps.entries[ctx->sync_maps.count++];
}

static void
npt_sync_map_remove(struct npt_context *ctx, struct npt_sync_map_entry *e)
{
   uint32_t idx = (uint32_t)(e - ctx->sync_maps.entries);
   ctx->sync_maps.entries[idx] =
      ctx->sync_maps.entries[--ctx->sync_maps.count];
}

static HRESULT
npt_resource_map_pinned(struct npt_context *ctx,
                 uint64_t context_id, uint64_t resource_id,
                 uint32_t subresource, uint32_t access_flags,
                 uint32_t api_map_flags, uint32_t shmem_res_id,
                 uint64_t read_range_begin,
                 uint64_t read_range_end,
                 uint64_t byte_size,
                 uint32_t mip_height, uint32_t mip_depth,
                 uint32_t shmem_offset,
                 uint32_t *out_row_pitch, uint32_t *out_depth_pitch,
                 uint32_t *out_mapped_size)
{
   *out_row_pitch = 0;
   *out_depth_pitch = 0;
   *out_mapped_size = 0;

   void *resource = npt_context_lookup_object(ctx, NULL, resource_id,
                                              NPT_OBJECT_TYPE_IUNKNOWN);
   if (!resource) {
      npt_log("map_resource: NULL resource");
      return NPT_E_FAIL;
   }
   if (!npt_resource_family_matches(ctx, resource_id, context_id,
                                    "map_resource"))
      return NPT_E_INVALIDARG;

   struct npt_resource *shmem_res =
      npt_context_get_resource(ctx, shmem_res_id);
   if (!shmem_res || shmem_res->fd_type != VIRGL_RESOURCE_FD_SHM ||
       !shmem_res->u.data) {
      npt_log("map_resource: invalid SHM resource %u", shmem_res_id);
      return NPT_E_FAIL;
   }

   if ((uint64_t)shmem_offset >= shmem_res->size) {
      npt_log("map_resource: shmem_offset %u exceeds shmem size %zu",
              shmem_offset, shmem_res->size);
      return NPT_E_FAIL;
   }

   D3D11_MAPPED_SUBRESOURCE mapped;
   memset(&mapped, 0, sizeof(mapped));
   HRESULT hr;

   if (!context_id) {
      /* RowPitch/DepthPitch stay 0: this path serves buffers, and the
       * guest computes texture layouts via GetCopyableFootprints. */
      D3D12_RANGE read_range;
      const D3D12_RANGE *rr = NULL;
      if (read_range_begin != NPT_MAP_RANGE_NULL) {
         read_range.Begin = (SIZE_T)read_range_begin;
         read_range.End = (SIZE_T)read_range_end;
         rr = &read_range;
      }

      void *pData = NULL;
      PFN_ID3D12Resource_Map map12 =
         NPT_COM_VTBL_FUNC(PFN_ID3D12Resource_Map,
                           npt_com_vtable(resource),
                           NPT_VTBL_ID3D12Resource_Map);
      if (!map12) {
         npt_log("map_resource: no D3D12 Map");
         return NPT_E_FAIL;
      }
      hr = map12(resource, subresource, rr, &pData);
      if (NPT_FAILED(hr))
         return hr;
      if (!pData) {
         npt_log("map_resource: D3D12 Map returned NULL data");
         return NPT_E_FAIL;
      }
      mapped.pData = pData;
   } else {
      void *imm_ctx = npt_context_lookup_object(ctx, NULL, context_id,
                                                NPT_OBJECT_TYPE_ID3D11DEVICECONTEXT);
      if (!imm_ctx) {
         npt_log("map_resource: NULL immediate context");
         return NPT_E_FAIL;
      }

      D3D11_MAP d3d11_map_type = npt_access_flags_to_d3d11_map(access_flags);

      /* May block on GPU sync; the guest's spin on the ring head
       * propagates the stall. */
      PFN_ID3D11DeviceContext_Map map_fn =
         NPT_COM_VTBL_FUNC(PFN_ID3D11DeviceContext_Map,
                           npt_com_vtable(imm_ctx),
                           NPT_VTBL_ID3D11DeviceContext_Map);
      hr = map_fn(imm_ctx, resource, subresource,
                  d3d11_map_type, api_map_flags, &mapped);

      if (NPT_FAILED(hr))
         return hr;
   }

   /* Memcpy bound for READ (and matching WRITE on the Unmap path):
    *   1. (mip_height, mip_depth) for textures — tightest, required
    *      since the guest can't know RowPitch ahead of Map.
    *   2. byte_size for buffers.
    *   3. shmem size as fallback.
    * Clamped to shmem_res->size so an oversized blob can't over-read
    * past D3D's mapped region into unrelated host memory. */
   uint64_t bound;
   if (mip_height && mip_depth) {
      bound = (uint64_t)mapped.RowPitch *
              (uint64_t)mip_height *
              (uint64_t)mip_depth;
   } else {
      bound = byte_size ? byte_size : shmem_res->size - shmem_offset;
   }
   if (bound > shmem_res->size - shmem_offset)
      bound = shmem_res->size - shmem_offset;
   const uint32_t mapped_size = (uint32_t)bound;

   /* Obelisk: a D3D11 Map that is not WRITE_DISCARD keeps the resource contents (MAP_WRITE, READ_WRITE, and
    * WRITE_NO_OVERWRITE, whose unwritten bytes stay valid), and the Unmap ships the whole slot back. Copy the
    * current contents into the slot for those too, not only for READ: Rocket League's Scaleform glyph cache copies
    * its atlas into a CPU-write staging texture, maps it with MAP_WRITE, adds glyphs and uploads the whole mapped
    * image; with an empty slot each upload erased the older glyphs (text disappearing, run 20261003T025108). */
   const bool keep_contents = context_id && (access_flags & NPT_MAP_ACCESS_WRITE) &&
                              !(access_flags & NPT_MAP_ACCESS_DISCARD);
   if ((access_flags & NPT_MAP_ACCESS_READ) || keep_contents)
      memcpy((uint8_t *)shmem_res->u.data + shmem_offset, mapped.pData,
             mapped_size);

   struct npt_sync_map_entry *entry =
      npt_sync_map_find(ctx, resource_id, subresource);
   if (entry) {
      /* Double-map of the same subresource: D3D11 disallows it, so
       * this indicates guest state drift; replace the stale entry. */
      npt_log("map_resource: resource 0x%" PRIx64 " sub %u already "
              "mapped, replacing stale entry", resource_id, subresource);
   } else {
      entry = npt_sync_map_add(ctx);
      if (!entry) {
         npt_log("map_resource: sync map table OOM");
         return NPT_E_OUTOFMEMORY;
      }
   }
   *entry = (struct npt_sync_map_entry){
      .resource_id  = resource_id,
      .subresource  = subresource,
      .access_flags = access_flags,
      .mapped_data  = mapped.pData,
      .mapped_size  = mapped_size,
      .persistent   = !!(access_flags & NPT_MAP_ACCESS_PERSISTENT),
   };

   if (context_id)
      npt_trace_tex(resource, "sync map sub %u access 0x%x rowpitch %u size %u slot_off %u", subresource,
                    access_flags, mapped.RowPitch, mapped_size, shmem_offset);
   *out_row_pitch = mapped.RowPitch;
   *out_depth_pitch = mapped.DepthPitch;
   *out_mapped_size = mapped_size;
   return hr;
}

static HRESULT
npt_resource_unmap_pinned(struct npt_context *ctx,
                   uint64_t context_id, uint64_t resource_id,
                   uint32_t subresource, uint32_t shmem_res_id,
                   uint32_t shmem_offset, uint64_t byte_size,
                   uint32_t access_flags,
                   uint64_t written_range_begin,
                   uint64_t written_range_end)
{
   void *resource = npt_context_lookup_object(ctx, NULL, resource_id,
                                              NPT_OBJECT_TYPE_IUNKNOWN);
   if (!resource) {
      npt_log("unmap_resource: NULL resource");
      return NPT_E_FAIL;
   }
   if (!npt_resource_family_matches(ctx, resource_id, context_id,
                                    "unmap_resource"))
      return NPT_E_INVALIDARG;

   struct npt_resource *shmem_res =
      npt_context_get_resource(ctx, shmem_res_id);
   if (!shmem_res || shmem_res->fd_type != VIRGL_RESOURCE_FD_SHM ||
       !shmem_res->u.data) {
      npt_log("unmap_resource: invalid SHM resource %u", shmem_res_id);
      return NPT_E_FAIL;
   }

   /* Bounds check the slot window. */
   if ((uint64_t)shmem_offset + byte_size > shmem_res->size) {
      npt_log("unmap_resource: slot window [%u, %u+%" PRIu64 ") "
              "exceeds shmem size %zu",
              shmem_offset, shmem_offset, byte_size,
              shmem_res->size);
      return NPT_E_FAIL;
   }
   const uint8_t *slot_src =
      (const uint8_t *)shmem_res->u.data + shmem_offset;
   if (context_id)
      npt_trace_tex(resource, "unmap sub %u access 0x%x bytes %llu slot_off %u", subresource, access_flags,
                    (unsigned long long)byte_size, shmem_offset);

   if (access_flags) {
      /* Rename-ring path: no prior MAP_RESOURCE; replay the full
       * Map + memcpy + Unmap cycle here. */
      if (!context_id) {
         npt_log("unmap_resource: access_flags path requires context_id");
         return NPT_E_FAIL;
      }

      void *imm_ctx = npt_context_lookup_object(ctx, NULL, context_id,
                                                 NPT_OBJECT_TYPE_ID3D11DEVICECONTEXT);
      if (!imm_ctx) {
         npt_log("unmap_resource: NULL immediate context");
         return NPT_E_FAIL;
      }

      D3D11_MAP d3d11_map_type =
         npt_access_flags_to_d3d11_map(access_flags);

      PFN_ID3D11DeviceContext_Map map_fn =
         NPT_COM_VTBL_FUNC(PFN_ID3D11DeviceContext_Map,
                           npt_com_vtable(imm_ctx),
                           NPT_VTBL_ID3D11DeviceContext_Map);
      PFN_ID3D11DeviceContext_Unmap unmap_fn =
         NPT_COM_VTBL_FUNC(PFN_ID3D11DeviceContext_Unmap,
                           npt_com_vtable(imm_ctx),
                           NPT_VTBL_ID3D11DeviceContext_Unmap);

      D3D11_MAPPED_SUBRESOURCE mapped;
      memset(&mapped, 0, sizeof(mapped));
      HRESULT hr = map_fn(imm_ctx, resource, subresource,
                          d3d11_map_type, 0, &mapped);
      if (NPT_FAILED(hr)) {
         npt_log("unmap_resource: rename-ring Map failed 0x%08x", hr);
         return NPT_E_FAIL;
      }

      if ((access_flags & NPT_MAP_ACCESS_WRITE) && mapped.pData) {
         uint64_t bound = byte_size;
         if ((uint64_t)shmem_offset + bound > shmem_res->size)
            bound = shmem_res->size - shmem_offset;
         /* Obelisk: a D3D11 rename replay may name the byte range the guest needs shipped (deferred dynamic
          * vertex/index buffers); only that range is copied, at the same offset. 0/0 means the whole slot. */
         uint64_t from = 0;
         if (written_range_end > written_range_begin && written_range_begin != NPT_MAP_RANGE_NULL) {
            from = written_range_begin < bound ? written_range_begin : bound;
            if (written_range_end < bound)
               bound = written_range_end;
         }
         memcpy((uint8_t *)mapped.pData + from, slot_src + from, (size_t)(bound - from));
         npt_unmap_account(access_flags, bound - from);
      }

      unmap_fn(imm_ctx, resource, subresource);
      return NPT_S_OK;
   }

   /* Paired with a prior MAP_RESOURCE. */
   struct npt_sync_map_entry *entry =
      npt_sync_map_find(ctx, resource_id, subresource);
   if (!entry) {
      npt_log("unmap_resource: resource 0x%" PRIx64 " sub %u not mapped",
              resource_id, subresource);
      return NPT_E_FAIL;
   }

   if (entry->persistent) {
      npt_log("unmap_resource: resource 0x%" PRIx64 " is persistent, "
              "ignoring unmap", resource_id);
      return NPT_E_FAIL;
   }

   if (entry->access_flags & NPT_MAP_ACCESS_WRITE) {
      /* MIN(guest byte_size, recorded mapped_size) so we don't
       * overrun the D3D region with stale/padded SHM bytes.  0 means
       * "use the full mapped_size". */
      uint64_t bound = byte_size ? byte_size : entry->mapped_size;
      if (bound > entry->mapped_size)
         bound = entry->mapped_size;
      memcpy(entry->mapped_data, slot_src, (size_t)bound);
      npt_unmap_account(0, bound);
   }

   if (!context_id) {
      D3D12_RANGE written_range;
      const D3D12_RANGE *wr = NULL;
      if (written_range_begin != NPT_MAP_RANGE_NULL) {
         written_range.Begin = (SIZE_T)written_range_begin;
         written_range.End = (SIZE_T)written_range_end;
         wr = &written_range;
      }
      PFN_ID3D12Resource_Unmap unmap12 =
         NPT_COM_VTBL_FUNC(PFN_ID3D12Resource_Unmap,
                           npt_com_vtable(resource),
                           NPT_VTBL_ID3D12Resource_Unmap);
      if (!unmap12) {
         npt_log("unmap_resource: no D3D12 Unmap");
         return NPT_E_FAIL;
      }
      unmap12(resource, subresource, wr);

      npt_sync_map_remove(ctx, entry);
      return NPT_S_OK;
   }

   void *imm_ctx = npt_context_lookup_object(ctx, NULL, context_id,
                                              NPT_OBJECT_TYPE_ID3D11DEVICECONTEXT);
   if (!imm_ctx) {
      npt_log("unmap_resource: NULL immediate context");
      return NPT_E_FAIL;
   }

   PFN_ID3D11DeviceContext_Unmap unmap_fn =
      NPT_COM_VTBL_FUNC(PFN_ID3D11DeviceContext_Unmap,
                        npt_com_vtable(imm_ctx),
                        NPT_VTBL_ID3D11DeviceContext_Unmap);
   unmap_fn(imm_ctx, resource, subresource);

   npt_sync_map_remove(ctx, entry);
   return NPT_S_OK;
}

/* Map/Unmap copy to and from the guest's shmem slot. A guest destroy of that shmem arrives on another thread
 * while ring commands naming it are still queued, so pin it for the whole call. A pinned call that finds the
 * resource already destroyed fails like any other stale id. */
HRESULT
npt_resource_map(struct npt_context *ctx,
                 uint64_t context_id, uint64_t resource_id,
                 uint32_t subresource, uint32_t access_flags,
                 uint32_t api_map_flags, uint32_t shmem_res_id,
                 uint64_t read_range_begin,
                 uint64_t read_range_end,
                 uint64_t byte_size,
                 uint32_t mip_height, uint32_t mip_depth,
                 uint32_t shmem_offset,
                 uint32_t *out_row_pitch, uint32_t *out_depth_pitch,
                 uint32_t *out_mapped_size)
{
   struct npt_resource *pin = npt_context_pin_resource(ctx, shmem_res_id);
   const HRESULT hr = npt_resource_map_pinned(ctx, context_id, resource_id, subresource, access_flags,
                                              api_map_flags, shmem_res_id, read_range_begin,
                                              read_range_end, byte_size, mip_height, mip_depth,
                                              shmem_offset, out_row_pitch, out_depth_pitch,
                                              out_mapped_size);
   if (pin)
      npt_context_free_detached_resource(pin);
   return hr;
}

HRESULT
npt_resource_unmap(struct npt_context *ctx,
                   uint64_t context_id, uint64_t resource_id,
                   uint32_t subresource, uint32_t shmem_res_id,
                   uint32_t shmem_offset, uint64_t byte_size,
                   uint32_t access_flags,
                   uint64_t written_range_begin,
                   uint64_t written_range_end)
{
   struct npt_resource *pin = npt_context_pin_resource(ctx, shmem_res_id);
   const HRESULT hr = npt_resource_unmap_pinned(ctx, context_id, resource_id, subresource, shmem_res_id,
                                                shmem_offset, byte_size, access_flags,
                                                written_range_begin, written_range_end);
   if (pin)
      npt_context_free_detached_resource(pin);
   return hr;
}
