/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * Shared aux types for override modules that own a host-written
 * feedback shmem slot (queries, fences).  npt_d3d11_feedback_aux is
 * the base; per-type aux structs embed it as their first member so
 * type-agnostic code can touch the slot bookkeeping.  The host poll
 * runs CPU-side since D3D11 has no GPU-injected QueryPoolResults.
 */

#ifndef NPT_OVERRIDES_D3D11_FEEDBACK_H
#define NPT_OVERRIDES_D3D11_FEEDBACK_H

#include "npt_common.h"

struct npt_com_base;
struct npt_renderer_shmem;

struct npt_d3d11_feedback_aux {
   struct npt_com_base *com;

   /* NULL shmem = registration skipped (env opt-out, pool OOM, or
    * wrapper came via QI without going through Create); the
    * type-specific path falls back to the sync wire round-trip. */
   struct npt_renderer_shmem *fb_shmem;
   uint32_t fb_offset;

   /* Cleared on aux_destroy so UNREGISTER fires exactly once. */
   bool registered;
};

struct npt_d3d11_query_aux {
   /* Must be first so generic helpers can downcast. */
   struct npt_d3d11_feedback_aux base;

   /* Bytes for the host to memcpy each successful poll. */
   uint32_t query_data_size;

   /* Bumped at each End before the slot is cleared, as the host bumps
    * its own count at End; the host stamps its count with each result,
    * so a slot still holding an earlier cycle's result reads as a
    * mismatch and GetData returns S_FALSE. */
   _Atomic uint32_t local_version;

   /* When the last End was issued (npt_profile_now_ns), and the cycle
    * GetData last asked the host to flush for. */
   uint64_t end_ns;
   uint32_t flushed_version;
};

/* Type-safe downcast from an ID3D11Asynchronous (or any tier in the
 * query family: Query, Query1, Predicate, Counter) to its aux.
 * Returns NULL if `async` is not a wrapper in the query family --
 * checked via vtbl identity against the family's tier vtbls, so
 * QI-routed pointers from other families fail cleanly. */
struct npt_d3d11_query_aux *npt_d3d11_query_aux_cast(void *async);

#endif /* NPT_OVERRIDES_D3D11_FEEDBACK_H */
