/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * D3D12 fence dispatch overrides.  Both Signal entry points feed the
 * feedback substrate so the guest can read a fence's value out of a
 * shared slot instead of over the wire: ID3D12CommandQueue::Signal is
 * the GPU-timeline advance, ID3D12Fence::Signal the CPU-side one.
 * ID3D12CommandQueue::Wait first waits on the CPU for a fence another
 * queue signals (see npt_override_ID3D12CommandQueue_Wait).
 */

#include "npt_com.h"
#include "npt_context.h"
#include "npt_event.h"
#include "npt_feedback.h"
#include "npt_overrides.h"
#include "npt_profile.h"

#include <unistd.h>

#include "neptune-protocol/npt_protocol_host_dispatch_types.h"
#include "neptune-protocol/npt_protocol_host_id3d12commandqueue.h"
#include "neptune-protocol/npt_protocol_host_id3d12fence.h"

/* ---- GATE_WAIT bridge ---------------------------------------------------- */

void
npt_d3d12_gate_addref(void *fence)
{
   npt_com_add_ref(fence);
}

void
npt_d3d12_gate_release(void *fence)
{
   npt_com_release(fence);
}

bool
npt_d3d12_gate_seoc(void *fence, uint64_t value, void *signal_handle)
{
   /* signal_handle follows the same convention as the event-proxy path:
    * whatever npt_event's event_fd_signal_handle produces for this
    * platform. */
   PFN_ID3D12Fence_SetEventOnCompletion seoc = NPT_COM_VTBL_FUNC(
      PFN_ID3D12Fence_SetEventOnCompletion, npt_com_vtable(fence),
      NPT_VTBL_ID3D12Fence_SetEventOnCompletion);
   return NPT_SUCCEEDED(seoc(fence, value, (HANDLE)signal_handle));
}

bool
npt_d3d12_gate_reached(void *fence, uint64_t value)
{
   PFN_ID3D12Fence_GetCompletedValue get = NPT_COM_VTBL_FUNC(
      PFN_ID3D12Fence_GetCompletedValue, npt_com_vtable(fence),
      NPT_VTBL_ID3D12Fence_GetCompletedValue);
   return get(fence) >= value;
}

static HRESULT
npt_override_ID3D12CommandQueue_Signal(
   struct npt_dispatch_context *dctx,
   struct npt_command_ID3D12CommandQueue_Signal *args,
   PFN_ID3D12CommandQueue_Signal original)
{
   args->ret = original(args->_self, args->pFence, args->Value);

   if (NPT_SUCCEEDED(args->ret) && args->pFence) {
      npt_feedback_fence_mark_signal(npt_context_from_dispatch(dctx),
                                     args->pFence, args->Value);
   }
   return args->ret;
}

static HRESULT
npt_override_ID3D12Fence_Signal(
   struct npt_dispatch_context *dctx,
   struct npt_command_ID3D12Fence_Signal *args,
   PFN_ID3D12Fence_Signal original)
{
   args->ret = original(args->_self, args->Value);

   if (NPT_SUCCEEDED(args->ret)) {
      npt_feedback_fence_mark_signal(npt_context_from_dispatch(dctx),
                                     args->_self, args->Value);
   }
   return args->ret;
}

/* ID3D12CommandQueue::Wait.  D3DMetal turns a queue Wait into a wait on a Metal shared event, and a Wait on a fence that
 * another queue signals later stalls the Metal command queue for tens of seconds: the signalling command buffer is not
 * committed while the waiting one holds the device (HELLDIVERS 2 uploads on a COPY queue and waits for it on DIRECT every
 * frame).  The wait is therefore done here, on the CPU, until the fence has the value, and the backend only sees a Wait
 * that is already satisfied.  This blocks the ring thread, so it only works while the Signal comes through another
 * ring: every DIRECT queue shares one ring, and a guest release drains all of them.  Until a Signal of the value has
 * reached the host (the feedback entry's target_value), the wait gives up after NPT_QUEUE_WAIT_UNSIGNALLED_NS, in case
 * the Signal is queued behind it; once it has, the GPU gets there, and NPT_QUEUE_WAIT_MAX_NS only keeps a lost device
 * from hanging the thread.  Either way the backend Wait then behaves as before. */
#define NPT_QUEUE_WAIT_UNSIGNALLED_NS 100000000ll
#define NPT_QUEUE_WAIT_MAX_NS 10000000000ll

/* Whether a Signal of at least value has reached the host for this fence. */
static bool
npt_d3d12_fence_signal_dispatched(struct npt_context *ctx, void *fence, uint64_t value)
{
   bool dispatched = false;
   if (!ctx || !ctx->feedback.table)
      return false;
   npt_feedback_state_lock(ctx);
   struct npt_feedback_entry *entry = npt_feedback_lookup_by_host_obj_locked(ctx, fence, NPT_FEEDBACK_TYPE_FENCE12);
   if (entry)
      dispatched = entry->target_value >= value;
   npt_feedback_state_unlock(ctx);
   return dispatched;
}

static HRESULT
npt_override_ID3D12CommandQueue_Wait(
   struct npt_dispatch_context *dctx,
   struct npt_command_ID3D12CommandQueue_Wait *args,
   PFN_ID3D12CommandQueue_Wait original)
{
   if (args->pFence && !npt_d3d12_gate_reached(args->pFence, args->Value)) {
      struct npt_context *ctx = npt_context_from_dispatch(dctx);
      const int64_t t0 = npt_profile_now_ns();
      while (!npt_d3d12_gate_reached(args->pFence, args->Value)) {
         const int64_t waited = npt_profile_now_ns() - t0;
         if (waited >= NPT_QUEUE_WAIT_MAX_NS ||
             (waited >= NPT_QUEUE_WAIT_UNSIGNALLED_NS &&
              !npt_d3d12_fence_signal_dispatched(ctx, args->pFence, args->Value)))
            break;
         usleep(200);
      }
   }
   args->ret = original(args->_self, args->pFence, args->Value);
   return args->ret;
}

struct npt_dispatch_id3d12commandqueue_overrides
npt_id3d12commandqueue_overrides = {
   .Signal = npt_override_ID3D12CommandQueue_Signal,
   .Wait = npt_override_ID3D12CommandQueue_Wait,
};

struct npt_dispatch_id3d12fence_overrides npt_id3d12fence_overrides = {
   .Signal = npt_override_ID3D12Fence_Signal,
};
