#include <stdlib.h>
/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * ID3D11DeviceContext{,1..4}: Map/Unmap shadow staging,
 * UpdateSubresource (registry-skipped because pSrcData is unsized),
 * and Begin/End/GetData with shared-memory query feedback.
 */

#include "npt_com.h"
#include "npt_overrides_d3d11_feedback.h"
#include "npt_device.h"
#include "npt_dispatch.h"
#include "npt_overrides.h"
#include "npt_profile.h"
#include "npt_resource.h"
#include "npt_ring.h"

#include "neptune-protocol/npt_protocol_client_id3d11devicecontext.h"

#define NPT_REGISTER_OVERRIDE_D3D11_DEVICE_CONTEXT4(m, f) \
   NPT_REGISTER_OVERRIDE(id3d11devicecontext4, m, f)
#define NPT_REGISTER_OVERRIDE_D3D11_DEVICE_CONTEXT3(m, f) \
   NPT_REGISTER_OVERRIDE(id3d11devicecontext3, m, f); \
   NPT_REGISTER_OVERRIDE_D3D11_DEVICE_CONTEXT4(m, f)
#define NPT_REGISTER_OVERRIDE_D3D11_DEVICE_CONTEXT2(m, f) \
   NPT_REGISTER_OVERRIDE(id3d11devicecontext2, m, f); \
   NPT_REGISTER_OVERRIDE_D3D11_DEVICE_CONTEXT3(m, f)
#define NPT_REGISTER_OVERRIDE_D3D11_DEVICE_CONTEXT1(m, f) \
   NPT_REGISTER_OVERRIDE(id3d11devicecontext1, m, f); \
   NPT_REGISTER_OVERRIDE_D3D11_DEVICE_CONTEXT2(m, f)
#define NPT_REGISTER_OVERRIDE_D3D11_DEVICE_CONTEXT(m, f) \
   NPT_REGISTER_OVERRIDE(id3d11devicecontext, m, f); \
   NPT_REGISTER_OVERRIDE_D3D11_DEVICE_CONTEXT1(m, f)

static const GUID *const context_tiers[] = {
   &NPT_IID_ID3D11DeviceContext,  &NPT_IID_ID3D11DeviceContext1,
   &NPT_IID_ID3D11DeviceContext2, &NPT_IID_ID3D11DeviceContext3,
   &NPT_IID_ID3D11DeviceContext4, NULL,
};

/* Obelisk test trace (NPT_TRACE_STAGING=<max lines>): every context call that touches a STAGING or mapped texture,
 * to follow the Scaleform glyph cache (staging R8 textures). Off by default. */
static int
npt_trace_staging_take(void)
{
   static int budget = -1;
   if (budget < 0) {
      char buf[16];
      DWORD n = GetEnvironmentVariableA("NPT_TRACE_STAGING", buf, sizeof(buf));
      budget = (n > 0 && n < sizeof(buf)) ? atoi(buf) : 0;
   }
   if (budget <= 0)
      return 0;
   budget--;
   return 1;
}

static bool
npt_trace_staging_interesting(void *res)
{
   struct npt_d3d11_texture *t = npt_d3d11_texture_cast(res);
   return t && (npt_d3d11_texture_get_usage(t) == D3D11_USAGE_STAGING || npt_d3d11_texture_get_is_mapped(t));
}

static const char *
npt_trace_res(void *res, char *buf, size_t len)
{
   struct npt_d3d11_texture *t = npt_d3d11_texture_cast(res);
   if (t)
      snprintf(buf, len, "tex %p(u%u m%d)", res, npt_d3d11_texture_get_usage(t), npt_d3d11_texture_get_is_mapped(t));
   else if (npt_d3d11_buffer_cast(res))
      snprintf(buf, len, "buf %p", res);
   else
      snprintf(buf, len, "res %p", res);
   return buf;
}

static uint32_t
npt_d3d11_map_to_access_flags(D3D11_MAP map_type)
{
   switch (map_type) {
   case D3D11_MAP_READ:
      return NPT_MAP_ACCESS_READ;
   case D3D11_MAP_WRITE:
      return NPT_MAP_ACCESS_WRITE;
   case D3D11_MAP_READ_WRITE:
      return NPT_MAP_ACCESS_READ | NPT_MAP_ACCESS_WRITE;
   case D3D11_MAP_WRITE_DISCARD:
      return NPT_MAP_ACCESS_WRITE | NPT_MAP_ACCESS_DISCARD;
   case D3D11_MAP_WRITE_NO_OVERWRITE:
      return NPT_MAP_ACCESS_WRITE | NPT_MAP_ACCESS_NO_OVERWRITE;
   default:
      return NPT_MAP_ACCESS_WRITE;
   }
}


/*
 * Obelisk: deferred, ranged sync of dynamic vertex and index buffers.
 *
 * The rename-ring path ships the whole buffer at every Unmap. Rocket League (UE3) streams its immediate-mode geometry
 * through a 4 MiB dynamic vertex buffer and index buffer with Map(WRITE_NO_OVERWRITE) appends: about 7000 Unmaps per
 * second, each copying 4 MiB on the host (about 30 GB/s of memcpy, half the renderer's ring thread). A dynamic buffer
 * whose only bind flags are VERTEX_BUFFER/INDEX_BUFFER can only be read by input-assembler draws (and copies), so its
 * Unmap is deferred and each draw ships just the bytes it reads: the vertex range of its vertex buffers and the index
 * range of its index buffer (the vertex range of an indexed draw comes from scanning the indices, which the guest has
 * in the same slot memory). Draws whose reads cannot be bounded (instancing, indirect, DrawAuto, a static index buffer)
 * ship whole buffers, as before. Copies flush whole buffers first. NPT_DEFER_DYNAMIC=0 turns this off.
 */
#define NPT_DEFER_MAX_CTX 8
#define NPT_DEFER_VB_SLOTS 32

struct npt_defer_ctx {
   void *self;
   bool immediate;
   struct npt_d3d11_buffer *vb[NPT_DEFER_VB_SLOTS];
   UINT vb_stride[NPT_DEFER_VB_SLOTS], vb_offset[NPT_DEFER_VB_SLOTS];
   struct npt_d3d11_buffer *ib;
   DXGI_FORMAT ib_format;
   UINT ib_offset;
};

static struct npt_defer_ctx npt_defer_ctxs[NPT_DEFER_MAX_CTX];
static struct { uint64_t ships, bytes, full, skipped, t0; } npt_defer_stats;

static bool
npt_defer_enabled(void)
{
   static int on = -1;
   if (on < 0) {
      char buf[8];
      DWORD n = GetEnvironmentVariableA("NPT_DEFER_DYNAMIC", buf, sizeof(buf));
      on = !(n > 0 && n < sizeof(buf) && buf[0] == '0');
      npt_log("deferred dynamic VB/IB sync %s", on ? "on" : "off");
   }
   return on;
}

static struct npt_defer_ctx *
npt_defer_ctx_get(void *self, bool create)
{
   for (unsigned i = 0; i < NPT_DEFER_MAX_CTX; i++)
      if (npt_defer_ctxs[i].self == self)
         return &npt_defer_ctxs[i];
   if (!create)
      return NULL;
   for (unsigned i = 0; i < NPT_DEFER_MAX_CTX; i++)
      if (!npt_defer_ctxs[i].self) {
         struct npt_defer_ctx *c = &npt_defer_ctxs[i];
         memset(c, 0, sizeof(*c));
         c->self = self;
         c->immediate = npt_id3d11devicecontext_default_GetType(self) == D3D11_DEVICE_CONTEXT_IMMEDIATE;
         return c;
      }
   return NULL;
}

/* Whether this buffer's DISCARD/NO_OVERWRITE Unmaps on this context are deferred. */
static bool
npt_defer_buffer(void *self, struct npt_d3d11_buffer *b)
{
   struct npt_d3d11_buffer_aux *aux = npt_d3d11_buffer_get_aux(b);
   if (!aux)
      return false;
   if (aux->defer < 0) {
      D3D11_BUFFER_DESC d;
      const UINT ia = D3D11_BIND_VERTEX_BUFFER | D3D11_BIND_INDEX_BUFFER;
      aux->defer = npt_defer_enabled() && npt_d3d11_buffer_fill_desc(b, &d) && d.Usage == D3D11_USAGE_DYNAMIC &&
                   d.BindFlags && !(d.BindFlags & ~ia) && !d.MiscFlags;
   }
   if (!aux->defer)
      return false;
   struct npt_defer_ctx *c = npt_defer_ctx_get(self, true);
   return c && c->immediate;
}

static void
npt_defer_ship(void *self, struct npt_d3d11_buffer *b, uint64_t lo, uint64_t hi)
{
   struct npt_d3d11_buffer_aux *aux = npt_d3d11_buffer_get_aux(b);
   if (!aux || !aux->pend_dirty)
      return;
   const uint32_t width = npt_d3d11_buffer_get_byte_width(b);
   if (hi > width) hi = width;
   if (lo >= hi)
      return;
   if (lo >= aux->synced_lo && hi <= aux->synced_hi && !aux->pend_discard) {
      npt_defer_stats.skipped++;
      return;
   }
   struct npt_ring *ring = npt_com_self_ring(self);
   const uint32_t slot = npt_d3d11_buffer_get_current_slot(b);
   const uint32_t flags = NPT_MAP_ACCESS_WRITE |
      (aux->pend_discard ? NPT_MAP_ACCESS_DISCARD : NPT_MAP_ACCESS_NO_OVERWRITE);
   uint32_t seqno = 0;
   npt_dispatch_resource_unmap_range_seqno(ring, ((struct npt_com_base *)self)->base.id,
                                           ((struct npt_com_base *)b)->base.id, 0,
                                           npt_d3d11_buffer_get_slot_shmem_res_id(b, slot), width,
                                           npt_d3d11_buffer_slot_offset(b, slot), flags, lo, hi, &seqno);
   npt_d3d11_buffer_mark_slot_submitted(b, slot, seqno, ring);
   aux->pend_discard = false;
   if (aux->synced_hi > aux->synced_lo && lo <= aux->synced_hi && hi >= aux->synced_lo) {
      if (lo < aux->synced_lo) aux->synced_lo = (uint32_t)lo;
      if (hi > aux->synced_hi) aux->synced_hi = (uint32_t)hi;
   } else {
      aux->synced_lo = (uint32_t)lo;
      aux->synced_hi = (uint32_t)hi;
   }
   npt_defer_stats.ships++;
   npt_defer_stats.bytes += hi - lo;
   if (lo == 0 && hi == width)
      npt_defer_stats.full++;
   if (npt_defer_stats.ships % 20000 == 0)
      npt_log("deferred VB/IB sync: %llu ships, %.1f MiB shipped, %llu whole-buffer, %llu skipped",
              (unsigned long long)npt_defer_stats.ships, npt_defer_stats.bytes / 1048576.0,
              (unsigned long long)npt_defer_stats.full, (unsigned long long)npt_defer_stats.skipped);
}

static void
npt_defer_ship_all(void *self, struct npt_d3d11_buffer *b)
{
   if (b)
      npt_defer_ship(self, b, 0, npt_d3d11_buffer_get_byte_width(b));
}

/* Bindings hold no reference (that would change the game's visible refcounts); a destroyed buffer is dropped from
 * every binding by the destroy hook instead. */
static void
npt_defer_track(struct npt_d3d11_buffer **slot, struct npt_d3d11_buffer *b)
{
   *slot = b;
}

static void
npt_defer_buffer_destroyed(void *buffer)
{
   for (unsigned i = 0; i < NPT_DEFER_MAX_CTX; i++) {
      struct npt_defer_ctx *c = &npt_defer_ctxs[i];
      if (!c->self)
         continue;
      for (unsigned k = 0; k < NPT_DEFER_VB_SLOTS; k++)
         if ((void *)c->vb[k] == buffer)
            c->vb[k] = NULL;
      if ((void *)c->ib == buffer)
         c->ib = NULL;
   }
}

static struct npt_d3d11_buffer *
npt_defer_pending(struct npt_d3d11_buffer *b)
{
   struct npt_d3d11_buffer_aux *aux = b ? npt_d3d11_buffer_get_aux(b) : NULL;
   return aux && aux->defer == 1 && aux->pend_dirty ? b : NULL;
}

/* Ships whole every pending buffer bound to the input assembler. */
static void
npt_defer_ship_bound_all(void *self, struct npt_defer_ctx *c)
{
   for (unsigned i = 0; i < NPT_DEFER_VB_SLOTS; i++)
      if (npt_defer_pending(c->vb[i]))
         npt_defer_ship_all(self, c->vb[i]);
   if (npt_defer_pending(c->ib))
      npt_defer_ship_all(self, c->ib);
}

static void
npt_defer_ship_vertices(void *self, struct npt_defer_ctx *c, uint64_t first, uint64_t count)
{
   for (unsigned i = 0; i < NPT_DEFER_VB_SLOTS; i++) {
      struct npt_d3d11_buffer *b = npt_defer_pending(c->vb[i]);
      if (!b)
         continue;
      const uint64_t stride = c->vb_stride[i], off = c->vb_offset[i];
      if (!stride) {
         npt_defer_ship_all(self, b);
         continue;
      }
      /* element 0 too: a per-instance stream of a non-instanced draw reads instance 0 */
      npt_defer_ship(self, b, off, off + stride);
      if (count)
         npt_defer_ship(self, b, off + first * stride, off + (first + count) * stride);
   }
}

/* Ships the index range of an indexed draw and returns the vertex range it references (false: unknown). */
static bool
npt_defer_index_range(void *self, struct npt_defer_ctx *c, UINT count, UINT start, uint32_t *vmin, uint32_t *vmax)
{
   if (!c->ib || !count)
      return false;
   const uint32_t isz = c->ib_format == DXGI_FORMAT_R32_UINT ? 4 : c->ib_format == DXGI_FORMAT_R16_UINT ? 2 : 0;
   if (!isz)
      return false;
   struct npt_d3d11_buffer_aux *aux = npt_d3d11_buffer_get_aux(c->ib);
   if (!aux || aux->defer != 1)
      return false; /* not a deferred dynamic buffer: its contents are not in guest memory */
   const uint64_t lo = (uint64_t)c->ib_offset + (uint64_t)start * isz, hi = lo + (uint64_t)count * isz;
   if (hi > npt_d3d11_buffer_get_byte_width(c->ib))
      return false;
   npt_defer_ship(self, c->ib, lo, hi);
   const uint8_t *p = npt_d3d11_buffer_slot_ptr(c->ib, npt_d3d11_buffer_get_current_slot(c->ib));
   if (!p)
      return false;
   uint32_t mn = UINT32_MAX, mx = 0;
   if (isz == 2) {
      const uint16_t *ix = (const uint16_t *)(p + lo);
      for (UINT k = 0; k < count; k++) { if (ix[k] < mn) mn = ix[k]; if (ix[k] > mx) mx = ix[k]; }
   } else {
      const uint32_t *ix = (const uint32_t *)(p + lo);
      for (UINT k = 0; k < count; k++) { if (ix[k] < mn) mn = ix[k]; if (ix[k] > mx) mx = ix[k]; }
   }
   *vmin = mn;
   *vmax = mx;
   return true;
}

static void
npt_defer_before_draw(void *self, bool indexed, UINT count, UINT start, INT base, bool bounded)
{
   struct npt_defer_ctx *c = npt_defer_ctx_get(self, false);
   if (!c)
      return;
   if (!bounded) {
      npt_defer_ship_bound_all(self, c);
      return;
   }
   if (!indexed) {
      npt_defer_ship_vertices(self, c, start, count);
      return;
   }
   uint32_t vmin, vmax;
   if (npt_defer_index_range(self, c, count, start, &vmin, &vmax)) {
      const int64_t first = (int64_t)base + vmin;
      if (first >= 0) {
         npt_defer_ship_vertices(self, c, (uint64_t)first, (uint64_t)vmax - vmin + 1);
         return;
      }
   }
   npt_defer_ship_bound_all(self, c);
}

/*
 * WRITE_DISCARD / WRITE_NO_OVERWRITE take the rename-ring fast path
 * (no round-trip; Unmap replays Map+memcpy+Unmap on the host).
 * Other Map types take the sync MAP_RESOURCE path on slot 0.
 */
static HRESULT
ctx_Map_buffer(void *self, struct npt_d3d11_buffer *b, UINT Subresource,
               D3D11_MAP MapType, UINT MapFlags,
               D3D11_MAPPED_SUBRESOURCE *pMappedResource)
{
   if (!npt_d3d11_buffer_ensure_map_shmem(b))
      return NPT_E_OUTOFMEMORY;

   if (MapType == D3D11_MAP_WRITE_DISCARD ||
       MapType == D3D11_MAP_WRITE_NO_OVERWRITE) {
      const uint32_t flags = npt_d3d11_map_to_access_flags(MapType);
      if (npt_defer_buffer(self, b)) {
         struct npt_d3d11_buffer_aux *aux = npt_d3d11_buffer_get_aux(b);
         aux->synced_lo = aux->synced_hi = 0;
         if (MapType == D3D11_MAP_WRITE_DISCARD) aux->pend_dirty = false; /* old contents are gone */
      }
      if (MapType == D3D11_MAP_WRITE_DISCARD)
         npt_d3d11_buffer_rotate_slot(b);
      const uint32_t byte_width = npt_d3d11_buffer_get_byte_width(b);
      pMappedResource->pData =
         npt_d3d11_buffer_slot_ptr(b, npt_d3d11_buffer_get_current_slot(b));
      pMappedResource->RowPitch = byte_width;
      pMappedResource->DepthPitch = byte_width;
      npt_d3d11_buffer_set_last_map_access_flags(b, flags);
      npt_d3d11_buffer_set_is_mapped(b, true);
      return NPT_S_OK;
   }

   /* Sync round-trip, slot 0. */
   uint64_t context_id = ((struct npt_com_base *)self)->base.id;
   uint64_t buffer_id  = ((struct npt_com_base *)b)->base.id;

   uint32_t row_pitch = 0, depth_pitch = 0;
   HRESULT hr = npt_dispatch_resource_map(
      npt_com_self_ring(self), context_id, buffer_id, Subresource,
      npt_d3d11_map_to_access_flags(MapType), MapFlags,
      npt_d3d11_buffer_get_map_shmem_res_id(b),
      /*pessimistic_size=*/npt_d3d11_buffer_get_byte_width(b),
      /*mip_height=*/0, /*mip_depth=*/0,
      /*shmem_offset=*/npt_d3d11_buffer_slot_offset(b, 0),
      &row_pitch, &depth_pitch);
   if (NPT_FAILED(hr))
      return hr;

   /* access_flags=0 => Unmap reuses sync-MAP map_state. */
   npt_d3d11_buffer_set_current_slot(b, 0);
   pMappedResource->pData = npt_d3d11_buffer_slot_ptr(b, 0);
   pMappedResource->RowPitch = row_pitch;
   pMappedResource->DepthPitch = depth_pitch;
   npt_d3d11_buffer_set_last_map_access_flags(b, 0);
   npt_d3d11_buffer_set_is_mapped(b, true);
   return NPT_S_OK;
}

/*
 * 1D/2D/3D unified.  WRITE_DISCARD/NO_OVERWRITE with a cached
 * RowPitch take the rename-ring fast path (no round-trip; first Map
 * pays the sync cost).  All other paths go through the sync
 * MAP_RESOURCE on slot 0; host RowPitch/DepthPitch are stashed for
 * the next fast-path Map.
 */
static HRESULT
ctx_Map_texture(void *self, struct npt_d3d11_texture *t, UINT Subresource,
                D3D11_MAP MapType, UINT MapFlags,
                D3D11_MAPPED_SUBRESOURCE *pMappedResource)
{
   if (!npt_d3d11_texture_is_mappable(t))
      return NPT_E_NOTIMPL;
   if (!npt_d3d11_texture_ensure_map_shmem(t))
      return NPT_E_OUTOFMEMORY;

   const uint32_t per_slot    = npt_d3d11_texture_get_slot_size(t);
   const uint32_t shmem_total = npt_d3d11_texture_get_shmem_size(t);
   const uint32_t cached_rp   = npt_d3d11_texture_get_cached_row_pitch(t);
   const uint32_t cached_dp   = npt_d3d11_texture_get_cached_depth_pitch(t);
   const bool can_async = (cached_rp != 0) &&
      (MapType == D3D11_MAP_WRITE_DISCARD ||
       MapType == D3D11_MAP_WRITE_NO_OVERWRITE);

   if (can_async) {
      const uint32_t byte_size =
         npt_d3d11_texture_get_subresource_byte_size(t, Subresource, cached_rp);
      if (byte_size && byte_size <= per_slot) {
         if (MapType == D3D11_MAP_WRITE_DISCARD)
            npt_d3d11_texture_rotate_slot(t);
         const uint32_t slot = npt_d3d11_texture_get_current_slot(t);
         pMappedResource->pData = npt_d3d11_texture_slot_ptr(t, slot);
         pMappedResource->RowPitch = cached_rp;
         pMappedResource->DepthPitch = cached_dp;
         npt_d3d11_texture_set_mapped_state(t, Subresource, cached_rp,
                                            byte_size,
                                            npt_d3d11_map_to_access_flags(MapType));
         return NPT_S_OK;
      }
      /* Cached pitch implies a byte_size that overruns one slot
       * (defensive: shouldn't happen for 2D); fall through to sync. */
   }

   /* Sync MAP_RESOURCE on slot 0; host MAP dispatcher reads from
    * offset 0.  Cache the returned pitches for the next fast-path. */
   uint64_t context_id  = ((struct npt_com_base *)self)->base.id;
   uint64_t resource_id = ((struct npt_com_base *)t)->base.id;
   npt_d3d11_texture_set_current_slot(t, 0);

   uint32_t mip_h = 0, mip_d = 0;
   npt_d3d11_texture_get_mip_dimensions(t, Subresource, &mip_h, &mip_d);

   uint32_t row_pitch = 0, depth_pitch = 0;
   HRESULT hr = npt_dispatch_resource_map(
      npt_com_self_ring(self), context_id, resource_id, Subresource,
      npt_d3d11_map_to_access_flags(MapType), MapFlags,
      npt_d3d11_texture_get_map_shmem_res_id(t),
      /*byte_size=*/per_slot,
      mip_h, mip_d,
      /*shmem_offset=*/npt_d3d11_texture_slot_offset(t, 0),
      &row_pitch, &depth_pitch);
   if (NPT_FAILED(hr))
      return hr;

   /* Bound Unmap memcpy at host_row_pitch * h * d.  Refuse if the
    * host's RowPitch implies a region larger than per_slot, or one
    * that cannot be sized at all -- silent overrun is worse than a
    * clean failure. */
   const uint32_t byte_size =
      npt_d3d11_texture_get_subresource_byte_size(t, Subresource, row_pitch);
   if (!byte_size || byte_size > per_slot) {
      npt_log("ctx_Map_texture: row_pitch=%u implies %u-byte mapped "
              "region, exceeds per-slot %u (shmem %u) -- refusing",
              row_pitch, byte_size, per_slot, shmem_total);
      memset(pMappedResource, 0, sizeof(*pMappedResource));
      return NPT_E_FAIL;
   }

   /* RowPitch is stable per (resource, subresource); pitch drift
    * would trip the byte_size > per_slot guard above. */
   npt_d3d11_texture_set_cached_pitches(t, row_pitch, depth_pitch);

   pMappedResource->pData = npt_d3d11_texture_shmem_ptr(t);
   pMappedResource->RowPitch = row_pitch;
   pMappedResource->DepthPitch = depth_pitch;
   /* access_flags=0 => Unmap reuses sync-MAP map_state. */
   npt_d3d11_texture_set_mapped_state(t, Subresource, row_pitch,
                                      byte_size, /*access_flags=*/0);
   return NPT_S_OK;
}

static void
ctx_Unmap_buffer(void *self, struct npt_d3d11_buffer *b, UINT Subresource)
{
   struct npt_ring *ring = npt_com_self_ring(self);
   const uint32_t slot = npt_d3d11_buffer_get_current_slot(b);
   const uint32_t access_flags =
      npt_d3d11_buffer_get_last_map_access_flags(b);

   if (access_flags && npt_defer_buffer(self, b)) {
      struct npt_d3d11_buffer_aux *aux = npt_d3d11_buffer_get_aux(b);
      aux->pend_dirty = true;
      if (access_flags & NPT_MAP_ACCESS_DISCARD)
         aux->pend_discard = true;
      npt_d3d11_buffer_set_is_mapped(b, false);
      npt_d3d11_buffer_set_last_map_access_flags(b, 0);
      return;
   }

   uint32_t seqno = 0;
   npt_dispatch_resource_unmap_seqno(ring,
      ((struct npt_com_base *)self)->base.id,
      ((struct npt_com_base *)b)->base.id,
      Subresource,
      npt_d3d11_buffer_get_slot_shmem_res_id(b, slot),
      npt_d3d11_buffer_get_byte_width(b),
      npt_d3d11_buffer_slot_offset(b, slot),
      access_flags,
      &seqno);

   /* Sync Maps land on slot 0 and don't need rename-ring tracking. */
   if (access_flags)
      npt_d3d11_buffer_mark_slot_submitted(b, slot, seqno, ring);

   npt_d3d11_buffer_set_is_mapped(b, false);
   npt_d3d11_buffer_set_last_map_access_flags(b, 0);
}

static void
ctx_Unmap_texture(void *self, struct npt_d3d11_texture *t)
{
   struct npt_ring *ring = npt_com_self_ring(self);
   const uint32_t access_flags = npt_d3d11_texture_get_last_map_access_flags(t);
   const uint32_t slot = npt_d3d11_texture_get_current_slot(t);

   uint32_t seqno = 0;
   if (access_flags) {
      npt_dispatch_resource_unmap_seqno(ring,
         ((struct npt_com_base *)self)->base.id,
         ((struct npt_com_base *)t)->base.id,
         npt_d3d11_texture_get_last_map_subresource(t),
         npt_d3d11_texture_get_slot_shmem_res_id(t, slot),
         npt_d3d11_texture_get_last_map_byte_size(t),
         /*shmem_offset=*/npt_d3d11_texture_slot_offset(t, slot),
         access_flags,
         &seqno);
      npt_d3d11_texture_mark_slot_submitted(t, slot, seqno, ring);
   } else {
      npt_dispatch_resource_unmap(ring,
         ((struct npt_com_base *)self)->base.id,
         ((struct npt_com_base *)t)->base.id,
         npt_d3d11_texture_get_last_map_subresource(t),
         npt_d3d11_texture_get_slot_shmem_res_id(t, slot),
         npt_d3d11_texture_get_last_map_byte_size(t),
         /*shmem_offset=*/npt_d3d11_texture_slot_offset(t, slot),
         /*access_flags=*/0);
   }

   npt_d3d11_texture_clear_mapped_state(t);
}

/*
 * Obelisk: a copy whose source texture is still mapped. Rocket League (Scaleform glyph cache) maps a staging texture
 * with D3D11_MAP_WRITE, writes glyphs, copies it to the glyph texture and maps it again, without an Unmap in between.
 * On Windows the copy reads the staging memory the game just wrote. Here the written bytes only live in the guest
 * map slot until an Unmap ships them, so the copy read stale host contents and glyphs went missing. Ship the slot to
 * the host before the copy, and keep the game's mapping valid: later writes go out with the next flush or Unmap
 * (the rename-replay path: host Map + memcpy + Unmap).
 */
static void
ctx_flush_mapped_source(void *self, ID3D11Resource *src)
{
   /* NPT_FLUSH_MAPPED_COPY=0 turns this off (A/B tests). The main menu freeze first blamed on it (runs
    * 20261003T014348 to 022545) was a lost unlock in nptunix.so, fixed in LKD (mm/watch.c retry bound). */
   static int enabled = -1;
   if (enabled < 0) {
      const char *e = getenv("NPT_FLUSH_MAPPED_COPY");
      enabled = !(e && e[0] == '0');
   }
   if (!enabled)
      return;
   npt_defer_ship_all(self, npt_defer_pending(npt_d3d11_buffer_cast(src)));
   struct npt_d3d11_texture *t = npt_d3d11_texture_cast(src);
   if (!t || !npt_d3d11_texture_get_is_mapped(t))
      return;
   const uint32_t sub = npt_d3d11_texture_get_last_map_subresource(t);
   const uint32_t bytes = npt_d3d11_texture_get_last_map_byte_size(t);
   const uint32_t flags = npt_d3d11_texture_get_last_map_access_flags(t);
   const uint32_t pitch = npt_d3d11_texture_get_cached_row_pitch(t);
   static unsigned n;
   if (n++ < 5)
      npt_log("copy from mapped texture %p (sub %u, %u bytes, flags 0x%x): flushing the map slot first", src, sub,
              bytes, flags);
   ctx_Unmap_texture(self, t);
   npt_d3d11_texture_set_mapped_state(t, sub, pitch, bytes, flags ? flags : NPT_MAP_ACCESS_WRITE);
}

static void NPT_STDMETHODCALLTYPE
ctx_CopySubresourceRegion_override(void *self, ID3D11Resource *pDstResource, UINT DstSubresource, UINT DstX,
                                   UINT DstY, UINT DstZ, ID3D11Resource *pSrcResource, UINT SrcSubresource,
                                   const D3D11_BOX *pSrcBox)
{
   { /* Obelisk diag */
      static unsigned nd;
      struct npt_d3d11_texture *td = npt_d3d11_texture_cast(pDstResource), *ts = npt_d3d11_texture_cast(pSrcResource);
      if ((td || ts) && nd++ < 120)
         npt_log("diag: CSR dst %p(tex %d mapped %d) sub %u at %u,%u src %p(tex %d mapped %d) sub %u box %s %u,%u-%u,%u",
                 pDstResource, !!td, td ? npt_d3d11_texture_get_is_mapped(td) : 0, DstSubresource, DstX, DstY,
                 pSrcResource, !!ts, ts ? npt_d3d11_texture_get_is_mapped(ts) : 0, SrcSubresource,
                 pSrcBox ? "yes" : "no", pSrcBox ? pSrcBox->left : 0, pSrcBox ? pSrcBox->top : 0,
                 pSrcBox ? pSrcBox->right : 0, pSrcBox ? pSrcBox->bottom : 0);
   }
   if ((npt_trace_staging_interesting(pSrcResource) || npt_trace_staging_interesting(pDstResource)) &&
       npt_trace_staging_take()) {
      char a[64], b[64];
      npt_log("stg: ctx %p CSR dst %s sub %u at %u,%u <- src %s sub %u box %s %u,%u-%u,%u", self,
              npt_trace_res(pDstResource, a, sizeof a), DstSubresource, DstX, DstY, npt_trace_res(pSrcResource, b, sizeof b),
              SrcSubresource, pSrcBox ? "yes" : "no", pSrcBox ? pSrcBox->left : 0, pSrcBox ? pSrcBox->top : 0,
              pSrcBox ? pSrcBox->right : 0, pSrcBox ? pSrcBox->bottom : 0);
   }
   ctx_flush_mapped_source(self, pSrcResource);
   ctx_flush_mapped_source(self, pDstResource);
   npt_id3d11devicecontext_default_CopySubresourceRegion(self, pDstResource, DstSubresource, DstX, DstY, DstZ,
                                                         pSrcResource, SrcSubresource, pSrcBox);
}

static void NPT_STDMETHODCALLTYPE
ctx_CopySubresourceRegion1_override(void *self, ID3D11Resource *pDstResource, UINT DstSubresource, UINT DstX,
                                    UINT DstY, UINT DstZ, ID3D11Resource *pSrcResource, UINT SrcSubresource,
                                    const D3D11_BOX *pSrcBox, UINT CopyFlags)
{
   ctx_flush_mapped_source(self, pSrcResource);
   ctx_flush_mapped_source(self, pDstResource);
   npt_id3d11devicecontext1_default_CopySubresourceRegion1(self, pDstResource, DstSubresource, DstX, DstY, DstZ,
                                                           pSrcResource, SrcSubresource, pSrcBox, CopyFlags);
}

static void NPT_STDMETHODCALLTYPE
ctx_CopyResource_override(void *self, ID3D11Resource *pDstResource, ID3D11Resource *pSrcResource)
{
   { /* Obelisk diag */
      static unsigned nd;
      struct npt_d3d11_texture *td = npt_d3d11_texture_cast(pDstResource), *ts = npt_d3d11_texture_cast(pSrcResource);
      if ((td || ts) && nd++ < 120)
         npt_log("diag: CR dst %p(tex %d mapped %d) src %p(tex %d mapped %d)", pDstResource, !!td,
                 td ? npt_d3d11_texture_get_is_mapped(td) : 0, pSrcResource, !!ts,
                 ts ? npt_d3d11_texture_get_is_mapped(ts) : 0);
   }
   if ((npt_trace_staging_interesting(pSrcResource) || npt_trace_staging_interesting(pDstResource)) &&
       npt_trace_staging_take()) {
      char a[64], b[64];
      npt_log("stg: ctx %p CR dst %s <- src %s", self, npt_trace_res(pDstResource, a, sizeof a),
              npt_trace_res(pSrcResource, b, sizeof b));
   }
   ctx_flush_mapped_source(self, pSrcResource);
   ctx_flush_mapped_source(self, pDstResource);
   npt_id3d11devicecontext_default_CopyResource(self, pDstResource, pSrcResource);
}

static void NPT_STDMETHODCALLTYPE
ctx_IASetVertexBuffers_override(void *self, UINT StartSlot, UINT NumBuffers, ID3D11Buffer **ppVertexBuffers,
                                const UINT *pStrides, const UINT *pOffsets)
{
   struct npt_defer_ctx *c = npt_defer_enabled() ? npt_defer_ctx_get(self, true) : NULL;
   if (c)
      for (UINT i = 0; i < NumBuffers && StartSlot + i < NPT_DEFER_VB_SLOTS; i++) {
         struct npt_d3d11_buffer *b = ppVertexBuffers ? npt_d3d11_buffer_cast(ppVertexBuffers[i]) : NULL;
         npt_defer_track(&c->vb[StartSlot + i], b);
         c->vb_stride[StartSlot + i] = pStrides ? pStrides[i] : 0;
         c->vb_offset[StartSlot + i] = pOffsets ? pOffsets[i] : 0;
      }
   npt_id3d11devicecontext_default_IASetVertexBuffers(self, StartSlot, NumBuffers, ppVertexBuffers,
                                                      pStrides, pOffsets);
}

static void NPT_STDMETHODCALLTYPE
ctx_IASetIndexBuffer_override(void *self, ID3D11Buffer *pIndexBuffer, DXGI_FORMAT Format, UINT Offset)
{
   struct npt_defer_ctx *c = npt_defer_enabled() ? npt_defer_ctx_get(self, true) : NULL;
   if (c) {
      npt_defer_track(&c->ib, npt_d3d11_buffer_cast(pIndexBuffer));
      c->ib_format = Format;
      c->ib_offset = Offset;
   }
   npt_id3d11devicecontext_default_IASetIndexBuffer(self, pIndexBuffer, Format, Offset);
}

static void NPT_STDMETHODCALLTYPE
ctx_ClearState_override(void *self)
{
   struct npt_defer_ctx *c = npt_defer_ctx_get(self, false);
   if (c) {
      for (unsigned i = 0; i < NPT_DEFER_VB_SLOTS; i++)
         npt_defer_track(&c->vb[i], NULL);
      npt_defer_track(&c->ib, NULL);
   }
   npt_id3d11devicecontext_default_ClearState(self);
}

static void NPT_STDMETHODCALLTYPE
ctx_Draw_override(void *self, UINT VertexCount, UINT StartVertexLocation)
{
   npt_defer_before_draw(self, false, VertexCount, StartVertexLocation, 0, true);
   npt_id3d11devicecontext_default_Draw(self, VertexCount, StartVertexLocation);
}

static void NPT_STDMETHODCALLTYPE
ctx_DrawIndexed_override(void *self, UINT IndexCount, UINT StartIndexLocation, INT BaseVertexLocation)
{
   npt_defer_before_draw(self, true, IndexCount, StartIndexLocation, BaseVertexLocation, true);
   npt_id3d11devicecontext_default_DrawIndexed(self, IndexCount, StartIndexLocation, BaseVertexLocation);
}

static void NPT_STDMETHODCALLTYPE
ctx_DrawInstanced_override(void *self, UINT VertexCountPerInstance, UINT InstanceCount, UINT StartVertexLocation,
                           UINT StartInstanceLocation)
{
   npt_defer_before_draw(self, false, VertexCountPerInstance, StartVertexLocation, 0,
                         InstanceCount <= 1 && !StartInstanceLocation);
   npt_id3d11devicecontext_default_DrawInstanced(self, VertexCountPerInstance, InstanceCount, StartVertexLocation,
                                                 StartInstanceLocation);
}

static void NPT_STDMETHODCALLTYPE
ctx_DrawIndexedInstanced_override(void *self, UINT IndexCountPerInstance, UINT InstanceCount,
                                  UINT StartIndexLocation, INT BaseVertexLocation, UINT StartInstanceLocation)
{
   npt_defer_before_draw(self, true, IndexCountPerInstance, StartIndexLocation, BaseVertexLocation,
                         InstanceCount <= 1 && !StartInstanceLocation);
   npt_id3d11devicecontext_default_DrawIndexedInstanced(self, IndexCountPerInstance, InstanceCount,
                                                        StartIndexLocation, BaseVertexLocation,
                                                        StartInstanceLocation);
}

static void NPT_STDMETHODCALLTYPE
ctx_DrawAuto_override(void *self)
{
   npt_defer_before_draw(self, false, 0, 0, 0, false);
   npt_id3d11devicecontext_default_DrawAuto(self);
}

static void NPT_STDMETHODCALLTYPE
ctx_DrawIndexedInstancedIndirect_override(void *self, ID3D11Buffer *pBufferForArgs, UINT AlignedByteOffsetForArgs)
{
   npt_defer_before_draw(self, true, 0, 0, 0, false);
   npt_id3d11devicecontext_default_DrawIndexedInstancedIndirect(self, pBufferForArgs, AlignedByteOffsetForArgs);
}

static void NPT_STDMETHODCALLTYPE
ctx_DrawInstancedIndirect_override(void *self, ID3D11Buffer *pBufferForArgs, UINT AlignedByteOffsetForArgs)
{
   npt_defer_before_draw(self, false, 0, 0, 0, false);
   npt_id3d11devicecontext_default_DrawInstancedIndirect(self, pBufferForArgs, AlignedByteOffsetForArgs);
}

/* Resources without a wrapper fall through to E_NOTIMPL / no-op. */
static HRESULT NPT_STDMETHODCALLTYPE
ctx_Map_override(void *self, ID3D11Resource *pResource, UINT Subresource,
                 D3D11_MAP MapType, UINT MapFlags,
                 D3D11_MAPPED_SUBRESOURCE *pMappedResource)
{
   if (!pResource || !pMappedResource)
      return NPT_E_INVALIDARG;

   struct npt_d3d11_buffer *b = npt_d3d11_buffer_cast(pResource);
   if (b)
      return ctx_Map_buffer(self, b, Subresource, MapType, MapFlags,
                            pMappedResource);

   struct npt_d3d11_texture *t = npt_d3d11_texture_cast(pResource);
   if (t) {
      const int was_mapped = npt_d3d11_texture_get_is_mapped(t);
      HRESULT hr = ctx_Map_texture(self, t, Subresource, MapType, MapFlags, pMappedResource);
      if (npt_trace_staging_interesting(pResource) && npt_trace_staging_take())
         npt_log("stg: ctx %p Map tex %p(u%u) sub %u type %u flags 0x%x was_mapped %d -> 0x%x data %p pitch %u", self,
                 pResource, npt_d3d11_texture_get_usage(t), Subresource, (unsigned)MapType, MapFlags, was_mapped,
                 (unsigned)hr, pMappedResource->pData, pMappedResource->RowPitch);
      static int n; /* Obelisk diag */
      if (n < 60 || !(n % 500))
         npt_log("diag: Map res %p sub %u type %u flags 0x%x -> 0x%x data %p pitch %u", pResource, Subresource,
                 (unsigned)MapType, MapFlags, (unsigned)hr, pMappedResource->pData, pMappedResource->RowPitch);
      n++;
      return hr;
   }

   return NPT_E_NOTIMPL;
}

static void NPT_STDMETHODCALLTYPE
ctx_Unmap_override(void *self, ID3D11Resource *pResource, UINT Subresource)
{
   if (!pResource) return;

   struct npt_d3d11_buffer *b = npt_d3d11_buffer_cast(pResource);
   if (b) {
      if (npt_d3d11_buffer_get_is_mapped(b))
         ctx_Unmap_buffer(self, b, Subresource);
      return;
   }

   struct npt_d3d11_texture *t = npt_d3d11_texture_cast(pResource);
   if (t && npt_trace_staging_take())
      npt_log("stg: ctx %p Unmap tex %p(u%u m%d) sub %u", self, pResource, npt_d3d11_texture_get_usage(t),
              npt_d3d11_texture_get_is_mapped(t), Subresource);
   { /* Obelisk diag: texture unmaps */
      static int n;
      if (n < 60 || !(n % 500))
         npt_log("diag: Unmap res %p tex %p sub %u mapped %d flags 0x%x", pResource, t, Subresource,
                 t ? npt_d3d11_texture_get_is_mapped(t) : -1, t ? npt_d3d11_texture_get_last_map_access_flags(t) : 0);
      n++;
   }
   if (t && npt_d3d11_texture_get_is_mapped(t))
      ctx_Unmap_texture(self, t);
}

/*
 * The generator skips UpdateSubresource (pSrcData is unsized);
 * route through RESOURCE_UPDATE.  Box sizing per resource:
 *   buffer:    pDstBox ? (right-left) : ByteWidth
 *   tex1d:     pDstBox ? (right-left)*bpp : width*bpp
 *   tex2d:     SrcRowPitch * rows_of_memory(height_box)
 *   tex3d:     SrcDepthPitch * depth_box
 */
static void NPT_STDMETHODCALLTYPE
ctx_UpdateSubresource_override(void *self, ID3D11Resource *pDstResource,
                               UINT DstSubresource, const D3D11_BOX *pDstBox,
                               const void *pSrcData, UINT SrcRowPitch,
                               UINT SrcDepthPitch)
{
   if (!pDstResource || !pSrcData)
      return;
   {
      struct npt_d3d11_texture *tt = npt_d3d11_texture_cast(pDstResource);
      const struct npt_d3d11_texture_aux *ax = tt ? npt_d3d11_texture_get_aux_const(tt) : NULL;
      if (ax && (pDstBox || ax->bytes_per_pixel == 1) && npt_trace_staging_take()) {
         /* rows x row bytes actually covered, and how many of those source bytes are non-zero */
         const uint32_t w = pDstBox ? pDstBox->right - pDstBox->left : ax->width;
         const uint32_t h = pDstBox ? pDstBox->bottom - pDstBox->top : ax->height;
         uint64_t nz = 0;
         for (uint32_t y = 0; y < h && y < 4096; y++)
            for (uint32_t x = 0; x < w * ax->bytes_per_pixel && x < 16384; x++)
               nz += ((const uint8_t *)pSrcData)[(size_t)y * SrcRowPitch + x] != 0;
         npt_log("stg: ctx %p UpdateSubresource dst tex %p %ux%u fmt %u u%u sub %u box %s %u,%u-%u,%u pitch %u src %p "
                 "nonzero %llu", self, pDstResource, ax->width, ax->height, (unsigned)ax->format, ax->usage,
                 DstSubresource, pDstBox ? "yes" : "no", pDstBox ? pDstBox->left : 0, pDstBox ? pDstBox->top : 0,
                 pDstBox ? pDstBox->right : 0, pDstBox ? pDstBox->bottom : 0, SrcRowPitch, pSrcData,
                 (unsigned long long)nz);
      }
   }

   /* MSDN: an empty box -- top >= bottom, left >= right, or
    * front >= back -- is a no-op.  Return before the unsigned box
    * extents below wrap. */
   if (pDstBox && (pDstBox->left >= pDstBox->right ||
                   pDstBox->top >= pDstBox->bottom ||
                   pDstBox->front >= pDstBox->back))
      return;

   struct npt_device *dev = npt_com_self_device(self);
   if (!dev)
      return;

   uint64_t resource_id = ((struct npt_com_base *)pDstResource)->base.id;
   uint64_t byte_size = 0;   /* ring reservation; what the host may consume */
   uint64_t copy_size = 0;   /* what D3D guarantees readable at pSrcData */

   struct npt_d3d11_buffer *b = npt_d3d11_buffer_cast(pDstResource);
   if (b) {
      byte_size = pDstBox ? (pDstBox->right - pDstBox->left)
                          : npt_d3d11_buffer_get_byte_width(b);
      copy_size = byte_size;
   } else {
      struct npt_d3d11_texture *t = npt_d3d11_texture_cast(pDstResource);
      if (t) {
         if (pDstBox) {
            /* Dimension from aux: depth>1 => 3D, height>1 => 2D, else 1D. */
            uint32_t tex_h = 0, tex_d = 0;
            npt_d3d11_texture_get_mip_dimensions(t, DstSubresource,
                                                 &tex_h, &tex_d);
            const uint32_t box_w = pDstBox->right  - pDstBox->left;
            const uint32_t box_h = pDstBox->bottom - pDstBox->top;
            const uint32_t box_d = pDstBox->back   - pDstBox->front;
            /* Box bounds are texels; SrcRowPitch spans a row of memory,
             * which under block compression is four texel rows.  The
             * reservation charges the full pitch for every row, but the
             * copy stops at the D3D-guaranteed source extent,
             *   (rows-1)*RowPitch + RowSizeInBytes
             * per slice (dwm's section bitmaps end exactly at a page
             * edge -> AV in the ring copy past that extent). */
            const DXGI_FORMAT fmt = npt_d3d11_texture_get_format(t);
            uint32_t rows = npt_dxgi_format_block_rows(fmt, box_h);
            bool planar = false;
            if (pDstBox->top == 0 && box_h == tex_h) {
               /* A full-height box covers the chroma plane(s) that the
                * planar formats store after the luma rows; a partial
                * box has no documented planar footprint and stays
                * luma-only. */
               const uint32_t sub_rows =
                  npt_dxgi_format_subresource_rows(fmt, box_h);
               planar = sub_rows != rows;
               rows = sub_rows;
            }
            const uint32_t last_row = npt_dxgi_format_row_bytes(fmt, box_w);
            /* MSDN sizes planar transfers by the full pitch of every
             * row, chroma included; no final-row discount there. */
            const bool tight = last_row && !planar;
            if (tex_d > 1) {
               const uint64_t strides =
                  (uint64_t)SrcDepthPitch * (box_d - 1u);
               byte_size = strides + (uint64_t)SrcRowPitch * rows;
               copy_size = tight
                  ? strides + (uint64_t)SrcRowPitch * (rows - 1u) + last_row
                  : byte_size;
            } else if (tex_h > 1) {
               byte_size = (uint64_t)SrcRowPitch * rows;
               copy_size = tight
                  ? (uint64_t)SrcRowPitch * (rows - 1u) + last_row
                  : byte_size;
            } else {
               /* 1D: SrcRowPitch is meaningless; one tight row. */
               byte_size = last_row;
               copy_size = last_row;
            }
            if (copy_size > byte_size)
               copy_size = byte_size;
         } else {
            uint32_t copy32 = 0;
            byte_size = npt_d3d11_texture_get_subresource_update_sizes(
               t, DstSubresource, SrcRowPitch, SrcDepthPitch, &copy32);
            copy_size = copy32;
         }
      }
   }

   if (!byte_size || !copy_size || byte_size > 0xffffffffu) {
      npt_log("ctx_UpdateSubresource: unsupported resource type "
              "(resource=%p sub=%u row=%u depth=%u) -- dropping update",
              pDstResource, DstSubresource, SrcRowPitch, SrcDepthPitch);
      return;
   }

   (void)npt_dispatch_resource_update(npt_device_method_ring(dev), resource_id,
                                      DstSubresource, SrcRowPitch,
                                      SrcDepthPitch, pDstBox,
                                      pSrcData, (uint32_t)byte_size,
                                      (uint32_t)copy_size);
}

/*
 * Query feedback: each query has a 128-B shmem slot.  Begin clears
 * the flag and bumps a version; the host's poll writes [result,
 * version, flag=1] when GetData is ready; guest GetData reads
 * locally.
 */

static struct npt_query_feedback_slot *
ctx_query_slot(struct npt_d3d11_query_aux *aux)
{
   if (!aux || !aux->base.fb_shmem)
      return NULL;
   return (struct npt_query_feedback_slot *)
      ((uint8_t *)aux->base.fb_shmem->mmap_ptr + aux->base.fb_offset);
}

/* A query whose result has not reached its slot this long after End asks
 * the host instead: an End on a deferred context, which the host does not
 * poll, or a registration the host refused. */
#define NPT_QUERY_LOCAL_WAIT_NS (50ull * 1000 * 1000)

static void NPT_STDMETHODCALLTYPE
ctx_End_override(void *self, ID3D11Asynchronous *pAsync)
{
   struct npt_d3d11_query_aux *aux = npt_d3d11_query_aux_cast(pAsync);
   if (aux && aux->base.registered) {
      /* Bump version + clear flag BEFORE the wire End so a racing
       * guest GetData sees flag=0 or a version mismatch (S_FALSE).
       * "New version + old result" is impossible: host writes flag=1
       * only after the result with release ordering. */
      uint32_t v = atomic_fetch_add_explicit(&aux->local_version, 1,
                                             memory_order_relaxed) + 1;
      struct npt_query_feedback_slot *slot = ctx_query_slot(aux);
      if (slot) {
         atomic_store_explicit(&slot->state, ((uint64_t)v) << 32,
                               memory_order_release);
      }
      aux->end_ns = npt_profile_now_ns();
   }
   npt_id3d11devicecontext_default_End(self, pAsync);
}

/* Obelisk: GetData reads the result the host poll left in the query's
 * slot, instead of a wire round trip that waits for the host to drain
 * the ring each time (Rocket League polls queries every frame). */
static HRESULT NPT_STDMETHODCALLTYPE
ctx_GetData_override(void *self, ID3D11Asynchronous *pAsync, void *pData,
                     UINT DataSize, UINT GetDataFlags)
{
   struct npt_d3d11_query_aux *aux = npt_d3d11_query_aux_cast(pAsync);
   struct npt_query_feedback_slot *slot = ctx_query_slot(aux);
   if (!slot || !aux->base.registered)
      /* Counter, OOM at Create, or QI-routed wrapper. */
      return npt_id3d11devicecontext_default_GetData(self, pAsync, pData,
                                                     DataSize, GetDataFlags);

   const uint64_t s = atomic_load_explicit(&slot->state,
                                           memory_order_acquire);
   const uint32_t flag = (uint32_t)(s & 0xFFFFFFFFu);
   const uint32_t version = (uint32_t)(s >> 32);
   const uint32_t expected =
      atomic_load_explicit(&aux->local_version, memory_order_relaxed);

   if (flag == NPT_QUERY_FEEDBACK_FLAG_READY && version == expected) {
      if (pData && DataSize) {
         const UINT copy = DataSize <= aux->query_data_size ?
                           DataSize : aux->query_data_size;
         memcpy(pData, slot->result, copy);
      }
      return NPT_S_OK;
   }

   if (npt_profile_now_ns() - aux->end_ns > NPT_QUERY_LOCAL_WAIT_NS)
      return npt_id3d11devicecontext_default_GetData(self, pAsync, pData,
                                                     DataSize, GetDataFlags);

   /* Without DONOTFLUSH, D3D11 flushes so the query can complete: once
    * per cycle is enough, the poll publishes the result. */
   if (!(GetDataFlags & 0x1u /* D3D11_ASYNC_GETDATA_DONOTFLUSH */) &&
       aux->flushed_version != expected) {
      aux->flushed_version = expected;
      npt_id3d11devicecontext_default_Flush(self);
   }
   return NPT_S_FALSE;
}

/* CopyFlags hints (DISCARD, NO_OVERWRITE) are advisory in the host
 * D3D library and lost on the wire either way. */
static void NPT_STDMETHODCALLTYPE
ctx_UpdateSubresource1_override(void *self, ID3D11Resource *pDstResource,
                                UINT DstSubresource, const D3D11_BOX *pDstBox,
                                const void *pSrcData, UINT SrcRowPitch,
                                UINT SrcDepthPitch, UINT CopyFlags)
{
   (void)CopyFlags;
   ctx_UpdateSubresource_override(self, pDstResource, DstSubresource,
                                  pDstBox, pSrcData, SrcRowPitch,
                                  SrcDepthPitch);
}

/* The protocol reserves output ids before a state query runs, and its
 * reply carries no null mask.  A reserved id therefore does not establish
 * that the host returned a view.  Query the actual host object before
 * exposing it to an application; QI reports E_NOINTERFACE for an id which
 * the host did not register, without making the decoder fatal. */
static void
ctx_confirm_host_view(void **view, const GUID *iid)
{
   if (!view || !*view)
      return;
   void *confirmed = NULL;
   HRESULT hr = npt_com_query_interface_host(*view, iid, &confirmed);
   if (NPT_SUCCEEDED(hr) && confirmed) {
      npt_com_default_release(confirmed);
   } else {
      npt_com_default_release(*view);
      *view = NULL;
   }
}

static void NPT_STDMETHODCALLTYPE
ctx_OMGetRenderTargets_override(void *self, UINT NumViews,
                               ID3D11RenderTargetView **views,
                               ID3D11DepthStencilView **depth)
{
   npt_id3d11devicecontext_default_OMGetRenderTargets(self, NumViews,
                                                   views, depth);
   if (views) {
      for (UINT i = 0; i < NumViews; i++)
         ctx_confirm_host_view((void **)&views[i],
                               &NPT_IID_ID3D11RenderTargetView);
   }
   ctx_confirm_host_view((void **)depth, &NPT_IID_ID3D11DepthStencilView);
}

void
npt_overrides_d3d11_context_init(void)
{
   NPT_REGISTER_OVERRIDE_D3D11_DEVICE_CONTEXT(Map,   ctx_Map_override);
   NPT_REGISTER_OVERRIDE_D3D11_DEVICE_CONTEXT(Unmap, ctx_Unmap_override);
   NPT_REGISTER_OVERRIDE_D3D11_DEVICE_CONTEXT(CopySubresourceRegion, ctx_CopySubresourceRegion_override);
   NPT_REGISTER_OVERRIDE_D3D11_DEVICE_CONTEXT1(CopySubresourceRegion1, ctx_CopySubresourceRegion1_override);
   NPT_REGISTER_OVERRIDE_D3D11_DEVICE_CONTEXT(CopyResource, ctx_CopyResource_override);
   npt_d3d11_buffer_destroy_hook = npt_defer_buffer_destroyed;
   NPT_REGISTER_OVERRIDE_D3D11_DEVICE_CONTEXT(IASetVertexBuffers, ctx_IASetVertexBuffers_override);
   NPT_REGISTER_OVERRIDE_D3D11_DEVICE_CONTEXT(IASetIndexBuffer, ctx_IASetIndexBuffer_override);
   NPT_REGISTER_OVERRIDE_D3D11_DEVICE_CONTEXT(ClearState, ctx_ClearState_override);
   NPT_REGISTER_OVERRIDE_D3D11_DEVICE_CONTEXT(Draw, ctx_Draw_override);
   NPT_REGISTER_OVERRIDE_D3D11_DEVICE_CONTEXT(DrawIndexed, ctx_DrawIndexed_override);
   NPT_REGISTER_OVERRIDE_D3D11_DEVICE_CONTEXT(DrawInstanced, ctx_DrawInstanced_override);
   NPT_REGISTER_OVERRIDE_D3D11_DEVICE_CONTEXT(DrawIndexedInstanced, ctx_DrawIndexedInstanced_override);
   NPT_REGISTER_OVERRIDE_D3D11_DEVICE_CONTEXT(DrawAuto, ctx_DrawAuto_override);
   NPT_REGISTER_OVERRIDE_D3D11_DEVICE_CONTEXT(DrawIndexedInstancedIndirect, ctx_DrawIndexedInstancedIndirect_override);
   NPT_REGISTER_OVERRIDE_D3D11_DEVICE_CONTEXT(DrawInstancedIndirect, ctx_DrawInstancedIndirect_override);
   NPT_REGISTER_OVERRIDE_D3D11_DEVICE_CONTEXT (UpdateSubresource,
                                               ctx_UpdateSubresource_override);
   NPT_REGISTER_OVERRIDE_D3D11_DEVICE_CONTEXT1(UpdateSubresource1,
                                               ctx_UpdateSubresource1_override);
   NPT_REGISTER_OVERRIDE_D3D11_DEVICE_CONTEXT(End,     ctx_End_override);
   NPT_REGISTER_OVERRIDE_D3D11_DEVICE_CONTEXT(GetData, ctx_GetData_override);
   NPT_REGISTER_OVERRIDE_D3D11_DEVICE_CONTEXT(OMGetRenderTargets,
                                             ctx_OMGetRenderTargets_override);

   npt_com_register_family(context_tiers, 0, NULL);
}
