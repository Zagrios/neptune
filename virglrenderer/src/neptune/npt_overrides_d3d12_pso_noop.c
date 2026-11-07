/*
 * SPDX-License-Identifier: MIT
 *
 * Pipeline states whose Metal compile failed.
 *
 * When the Metal pipeline compile of a D3D12 graphics pipeline state fails, D3DMetal's D3DMGraphicsPipelineState
 * constructor marks the object no-op and skips InitStaticGraphicsState, but the device still hands the wrapper to
 * the application (FailOnNoOpPipelinestate is hard-coded 0). The vertex-buffer slot count of such an object is
 * whatever the heap held, and the first draw that binds it walks that count in SetRenderVertexBuffers: a crash of
 * the render server minutes into any game that compiles many pipelines. In one HELLDIVERS 2 session about 240 of
 * 9000 graphics states were born that way, and every one of 11 crashes read such a count. D3DMetal skips the draws
 * of a no-op state anyway, so zeroing the count when the state is created costs nothing and ends the crash.
 *
 * The offsets are D3DMetal internals (GPTK 2.x libd3dshared, checked against the crash dumps):
 *   wrapper + 0x58    the inner D3DMGraphicsPipelineState / D3DMComputePipelineState
 *   inner   + 0x10    kind: 0 graphics, 1 compute
 *   inner   + 0x28    no-op flag, set when the Metal pipeline compile failed
 *   inner   + 0x28c   16-bit vertex-buffer slot count (D3D12 allows at most 32)
 * Nothing is written to an object that does not look like a no-op graphics state; an impossible count on a state
 * that is not marked no-op is only logged.
 */

#include "npt_overrides.h"
#include "npt_common.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#define PSO_INNER 0x58
#define PIPELINE_KIND 0x10
#define PIPELINE_NOOP 0x28
#define PIPELINE_VB_COUNT 0x28c
#define VB_SLOTS_MAX 32
#define LOG_FIRST 20 /* lines per kind before the log goes quiet */

static _Atomic uint32_t born, noop, odd;

void
npt_d3d12_pso_born(void *pso, bool is_pipeline_state)
{
   if (!is_pipeline_state || !pso)
      return;
   char *inner = *(char **)((char *)pso + PSO_INNER);
   if (!inner || *(const uint64_t *)(inner + PIPELINE_KIND) != 0)
      return; /* no inner object yet, or a compute pipeline: no vertex slots */

   const uint32_t n = atomic_fetch_add(&born, 1) + 1;
   uint16_t *count = (uint16_t *)(inner + PIPELINE_VB_COUNT);
   const bool is_noop = *(const uint8_t *)(inner + PIPELINE_NOOP) != 0;

   if (is_noop) {
      const uint32_t k = atomic_fetch_add(&noop, 1) + 1;
      if (k <= LOG_FIRST || k % 256 == 0)
         npt_log("d3d12: pipeline state %p was born no-op (metal compile failed), vertex slot count %u zeroed "
                 "(%u of %u graphics states so far)",
                 pso, *count, k, n);
      *count = 0;
      return;
   }
   if (*count > VB_SLOTS_MAX) {
      /* Not marked no-op and still impossible: not a case this understands, so leave it alone and say so. */
      const uint32_t k = atomic_fetch_add(&odd, 1) + 1;
      if (k <= LOG_FIRST)
         npt_log("d3d12: pipeline state %p was born with vertex slot count %u and no no-op flag, left as is (%u so far)",
                 pso, *count, k);
   }
}
