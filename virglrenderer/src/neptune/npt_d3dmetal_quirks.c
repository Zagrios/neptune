/*
 * SPDX-License-Identifier: MIT
 *
 * D3DMetal pipeline states whose Metal compile failed.
 *
 * When the Metal pipeline compile of a D3D12 graphics pipeline state fails, D3DMetal's D3DMGraphicsPipelineState
 * constructor marks the object no-op and skips InitStaticGraphicsState, but the device still hands the wrapper to
 * the application (FailOnNoOpPipelinestate is hard-coded 0). The vertex-buffer slot count of such an object is
 * whatever the heap held, and the first draw that binds it walks that count in SetRenderVertexBuffers: a crash of
 * the render server minutes into any game that compiles many pipelines. In one HELLDIVERS 2 session about 240 of
 * 9000 graphics states were born that way, and every one of 11 crashes read such a count. D3DMetal skips the draws
 * of a no-op state anyway, so zeroing the count when the state is created costs nothing and ends the crash.
 *
 * The offsets are D3DMetal internals, read from D3DMetal.framework 4.0b2 (pso is the ID3D12PipelineState pointer):
 *   pso   + 0      D3D12PipelineState_GetVTable(), stored by the D3D12PipelineState constructor
 *   pso   + 0x58   the inner pipeline state, stored by the D3D12GraphicsPipelineState constructor
 *   inner + 0      _ZTV25D3DMGraphicsPipelineState + 16, stored by the D3DMGraphicsPipelineState constructor (a
 *                  compute state carries _ZTV24D3DMComputePipelineState + 16)
 *   inner + 0x28   no-op flag, set by that constructor when the Metal pipeline compile fails
 *   inner + 0x28c  16-bit vertex-buffer slot count, zeroed by InitStaticGraphicsState, set by InitVertexState and
 *                  walked by D3D12GraphicsCommandListMPL::SetRenderVertexBuffers (D3D12 allows at most 32)
 * They hold for that build only: another D3DMetal, or another backend, turns this off with one line in the log.
 * Nothing is written to an object whose two vtables are not those; an impossible count on a state that is not
 * marked no-op is only logged.
 */

#include "npt_d3dmetal_quirks.h"
#include "npt_library.h"

#ifdef HAVE_DLFCN_H
#include <dlfcn.h>
#endif

#define PSO_INNER 0x58
#define PIPELINE_NOOP 0x28
#define PIPELINE_VB_COUNT 0x28c
#define VB_SLOTS_MAX 32
#define LOG_FIRST 20 /* lines per kind before the log goes quiet */

/* The build the offsets were read from, as the end of its what(1) string
 * "@(#)PROGRAM:D3DMetal  PROJECT:D3DMetal-4.0b2\n". */
#define D3DMETAL_PROJECT "PROJECT:D3DMetal-4.0b2\n"

static once_flag quirks_once = ONCE_FLAG_INIT;
static bool quirks_enabled;
static const void *graphics_pipeline_vtable;
static void *(*pipeline_state_get_vtable)(void);

static _Atomic uint32_t noop, odd;

/* NULL once the backend is the D3DMetal build above and its symbols resolved, otherwise why the fix stays off. */
static const char *
npt_d3dmetal_quirks_resolve(void)
{
#ifdef HAVE_DLFCN_H
   const struct npt_d3d_library *lib = npt_renderer_get_library();
   if (!lib || lib->backend != NPT_BACKEND_D3DMETAL || !lib->d3d12_module)
      return "the D3D12 backend is not D3DMetal";

   /* d3dmetal-native loads D3DMetal RTLD_LOCAL: reach it through the path it loaded. The reference is kept, since
    * the addresses resolved below have to stay valid. */
   const char *(*framework_path)(void) =
      ((union { void *p; const char *(*f)(void); }){ .p = dlsym(lib->d3d12_module, "dmn_framework_path") }).f;
   const char *path = framework_path ? framework_path() : NULL;
   void *d3dmetal = path ? dlopen(path, RTLD_NOW | RTLD_LOCAL | RTLD_NOLOAD) : NULL;
   if (!d3dmetal)
      return "D3DMetal.framework is not loaded";

   const char *version = dlsym(d3dmetal, "D3DMetalVersionString");
   if (!version || !strstr(version, D3DMETAL_PROJECT))
      return "D3DMetal is not 4.0b2, the build its offsets were read from";

   const char *vtable = dlsym(d3dmetal, "_ZTV25D3DMGraphicsPipelineState");
   pipeline_state_get_vtable =
      ((union { void *p; void *(*f)(void); }){ .p = dlsym(d3dmetal, "D3D12PipelineState_GetVTable") }).f;
   if (!vtable || !pipeline_state_get_vtable)
      return "D3DMetal does not export the pipeline-state vtables";
   /* An object points past the offset-to-top and typeinfo words of its vtable. */
   graphics_pipeline_vtable = vtable + 2 * sizeof(void *);
   return NULL;
#else
   return "dlopen is not available";
#endif
}

static void
npt_d3dmetal_quirks_init(void)
{
   const char *why = npt_d3dmetal_quirks_resolve();
   quirks_enabled = !why;
   if (why)
      npt_log("d3dmetal: the vertex-buffer count of no-op pipeline states is left as is: %s", why);
}

void
npt_d3dmetal_on_register_object(void *obj, npt_object_type type)
{
   /* No interface derives from ID3D12PipelineState. */
   if (type != NPT_OBJECT_TYPE_ID3D12PIPELINESTATE || !obj)
      return;
   /* Resolved at the first pipeline state, when the device that made it has loaded D3DMetal. */
   call_once(&quirks_once, npt_d3dmetal_quirks_init);
   if (!quirks_enabled || *(void **)obj != pipeline_state_get_vtable())
      return;
   char *inner = *(char **)((char *)obj + PSO_INNER);
   if (!inner || *(const void **)inner != graphics_pipeline_vtable)
      return; /* no inner object yet, or a compute pipeline: no vertex slots */

   uint16_t *count = (uint16_t *)(inner + PIPELINE_VB_COUNT);
   const bool is_noop = *(const uint8_t *)(inner + PIPELINE_NOOP) != 0;

   if (is_noop) {
      const uint32_t k = atomic_fetch_add(&noop, 1) + 1;
      if (k <= LOG_FIRST || k % 256 == 0)
         npt_log("d3d12: pipeline state %p was born no-op (metal compile failed), vertex slot count %u zeroed",
                 obj, *count);
      *count = 0;
      return;
   }
   if (*count > VB_SLOTS_MAX) {
      /* Not marked no-op and still impossible: not a case this understands, so leave it alone and say so. */
      const uint32_t k = atomic_fetch_add(&odd, 1) + 1;
      if (k <= LOG_FIRST)
         npt_log("d3d12: pipeline state %p was born with vertex slot count %u and no no-op flag, left as is (%u so far)",
                 obj, *count, k);
   }
}
