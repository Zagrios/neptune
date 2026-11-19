/*
 * SPDX-License-Identifier: MIT
 *
 * Pipeline state creation off the ring threads (npt_async_pso.c).
 */

#ifndef NPT_ASYNC_PSO_H
#define NPT_ASYNC_PSO_H

#include "npt_common.h"

#include "neptune-protocol/npt_protocol_host_dispatch_types.h"

struct npt_context;
struct npt_dispatch_context;

/* Each queues the creation and returns true; false means the caller
 * creates the pipeline itself, as before. */
bool
npt_async_pso_create_graphics(struct npt_dispatch_context *dispatch,
                              void *device,
                              const D3D12_GRAPHICS_PIPELINE_STATE_DESC *desc,
                              const IID *riid, uint64_t guest_id,
                              void *original);

bool
npt_async_pso_create_compute(struct npt_dispatch_context *dispatch,
                             void *device,
                             const D3D12_COMPUTE_PIPELINE_STATE_DESC *desc,
                             const IID *riid, uint64_t guest_id,
                             void *original);

bool
npt_async_pso_create_stream(struct npt_dispatch_context *dispatch,
                            void *device,
                            const D3D12_PIPELINE_STATE_STREAM_DESC *desc,
                            const IID *riid, uint64_t guest_id,
                            void *original);

/* Waits for the creation of guest_id if one is queued or running; true if
 * it had to wait, and the caller's lookup is worth repeating. */
bool
npt_async_pso_wait(struct npt_context *ctx, uint64_t guest_id);

/* For the object lookup, on an id the table does not hold.  Waits for a
 * pending creation and returns true when the lookup is worth repeating.
 * Inside a command list call that can do without the pipeline it does
 * not wait: it returns false and npt_async_pso_lookup_skipped() tells the
 * lookup that the id is a pipeline left out on purpose, not a miss. */
bool
npt_async_pso_lookup_pending(struct npt_context *ctx, uint64_t guest_id);

bool
npt_async_pso_lookup_skipped(void);

/* Around the dispatch of one command. */
void
npt_async_pso_begin_command(uint32_t cmd_type);

void
npt_async_pso_end_command(void);

extern struct npt_dispatch_id3d12graphicscommandlist_overrides
   npt_async_pso_list_overrides;
extern struct npt_dispatch_id3d12graphicscommandlist6_overrides
   npt_async_pso_list6_overrides;

/* A release of guest_id: true if its creation is still pending, in which
 * case the pipeline is released when the creation finishes and the caller
 * has nothing left to do.  The compilation still runs to its end -- an
 * engine that precompiles a pipeline and drops it wants the shader cache
 * filled. */
bool
npt_async_pso_release_pending(struct npt_context *ctx, uint64_t guest_id);

/* The time of the last sign that creations are making progress: now while
 * any is queued or running, else when the last one finished; 0 for never.
 * Work that waits behind a creation is late, not lost. */
uint64_t
npt_async_pso_last_activity_ns(void);

/* Drops the context's queued creations and waits for its running ones. */
void
npt_async_pso_drain(struct npt_context *ctx);

#endif /* NPT_ASYNC_PSO_H */
