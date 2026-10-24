/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * Host-side ID3D11DeviceContext::End hook that counts the query's
 * cycle and queues its feedback entry for the next poll.  Begin and
 * GetData need no host hook: guest and host both bump the version at
 * End, so a slot still holding an earlier End's result reads as a
 * version mismatch (S_FALSE), and the guest's local GetData reads the
 * feedback slot directly.
 */

#include "npt_context.h"
#include "npt_feedback.h"
#include "npt_overrides.h"

#include "neptune-protocol/npt_protocol_host_dispatch_types.h"
#include "neptune-protocol/npt_protocol_host_id3d11devicecontext.h"

static void
npt_override_DC_End(struct npt_dispatch_context *dctx,
                    struct npt_command_ID3D11DeviceContext_End *args,
                    PFN_ID3D11DeviceContext_End original)
{
   original(args->_self, args->pAsync);

   /* args->pAsync is the host pointer; mark_end finds its entry by
    * host pointer, resolved from the guest id at the first End. */
   struct npt_context *ctx = npt_context_from_dispatch(dctx);
   if (!ctx)
      return;
   npt_feedback_query_mark_end(ctx, args->_self, args->pAsync);
}

struct npt_dispatch_id3d11devicecontext_overrides
npt_query_dc_overrides = {
   .End = npt_override_DC_End,
};
