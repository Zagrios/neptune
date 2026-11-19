/*
 * SPDX-License-Identifier: MIT
 *
 * Pipeline state creation off the ring threads.
 *
 * D3DMetal compiles a pipeline's Metal shaders inside the create call.
 * On a cold shader cache that is tens of milliseconds to seconds per
 * pipeline, and an engine that creates its pipelines one after another
 * from one thread (Unreal Engine 5: several thousand at start-up) keeps
 * one compiler busy for many minutes while every other core idles and no
 * frame is drawn.
 *
 * Here the create call copies its description, answers S_OK at once and
 * leaves the creation to a pool of worker threads.  The guest id of the
 * new pipeline is recorded as pending; a lookup of a pending id waits for
 * its creation, and moves it to the front of the queue if it has not been
 * started.  So the guest never waits for a pipeline it does not use yet,
 * the compiler runs on several cores, and a command that needs a pipeline
 * still finds it created.
 *
 * A command list does not wait either.  When SetPipelineState, Reset or
 * ClearState names a pipeline that is still being compiled, the list is
 * marked and its draws are dropped until a finished pipeline is set: for
 * those frames the object is missing from the picture, and the game keeps
 * running while the compiler works.  This is what DXVK's asynchronous
 * pipeline compilation does on Vulkan.  Compute pipelines are waited
 * for: what a dispatch computes is read by later passes and by the
 * engine itself.  NPT_ASYNC_PSO_SKIP=0 makes lists wait for every
 * pipeline, =2 lets them leave out dispatches as well.
 *
 * The price is the HRESULT: the guest has its S_OK before the creation
 * ran.  A creation that fails registers its id as a null object, so the
 * commands that name it bind nothing instead of failing the context.
 * NPT_ASYNC_PSO=0 turns all of this off; NPT_ASYNC_PSO_THREADS
 * sets the number of workers.
 */

#include "npt_async_pso.h"

#include <inttypes.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "npt_com.h"
#include "npt_context.h"
#include "npt_profile.h"
#include "util/u_thread.h"

#include "neptune-protocol/npt_protocol_host_dispatch_types.h"
#include "neptune-protocol/npt_protocol_host_id3d12graphicscommandlist.h"

#if defined(__APPLE__)
#include <pthread.h>
#include <sys/qos.h>
#endif

enum npt_async_pso_kind {
   NPT_ASYNC_PSO_GRAPHICS,
   NPT_ASYNC_PSO_COMPUTE,
   NPT_ASYNC_PSO_STREAM,
};

struct npt_async_pso_job {
   struct list_head head;
   struct npt_context *ctx;
   uint64_t guest_id;
   enum npt_async_pso_kind kind;
   void *device;
   void *original;
   IID riid;
   union {
      D3D12_GRAPHICS_PIPELINE_STATE_DESC graphics;
      D3D12_COMPUTE_PIPELINE_STATE_DESC compute;
      D3D12_PIPELINE_STATE_STREAM_DESC stream;
   } desc;
   void *root_signature;
   /* Every copied block, freed with the job. */
   void **blocks;
   unsigned block_count;
   unsigned block_cap;
   bool failed_copy;
   /* The guest released the pipeline before its creation finished. */
   bool released;
   bool registered;
   bool compute;
};

static struct {
   mtx_t mutex;
   cnd_t work;      /* a job was queued */
   cnd_t done;      /* a job finished */
   struct list_head queue;
   struct list_head running;
   unsigned threads;
   uint64_t created;
   uint64_t total_ns;
   uint64_t max_ns;
   uint64_t last_log_ns;
   /* When a creation last finished; npt_async_pso_busy_until. */
   _Atomic uint64_t last_done_ns;
   _Atomic uint32_t outstanding;
} pso;

static _Atomic uint64_t pso_skipped_draws;
static once_flag pso_once = ONCE_FLAG_INIT;
static int pso_enabled = -1;

/* ---------- copying a description ---------- */

static void *
job_copy(struct npt_async_pso_job *job, const void *src, size_t size)
{
   if (!src || !size)
      return NULL;
   if (job->block_count == job->block_cap) {
      const unsigned cap = job->block_cap ? job->block_cap * 2 : 16;
      void **blocks = realloc(job->blocks, cap * sizeof(*blocks));
      if (!blocks) {
         job->failed_copy = true;
         return NULL;
      }
      job->blocks = blocks;
      job->block_cap = cap;
   }
   void *dst = malloc(size);
   if (!dst) {
      job->failed_copy = true;
      return NULL;
   }
   memcpy(dst, src, size);
   job->blocks[job->block_count++] = dst;
   return dst;
}

static const char *
job_copy_string(struct npt_async_pso_job *job, const char *s)
{
   return s ? job_copy(job, s, strlen(s) + 1) : NULL;
}

static void
job_copy_shader(struct npt_async_pso_job *job, D3D12_SHADER_BYTECODE *code)
{
   code->pShaderBytecode =
      job_copy(job, code->pShaderBytecode, code->BytecodeLength);
   if (!code->pShaderBytecode)
      code->BytecodeLength = 0;
}

static void
job_copy_input_layout(struct npt_async_pso_job *job,
                      D3D12_INPUT_LAYOUT_DESC *layout)
{
   D3D12_INPUT_ELEMENT_DESC *elements =
      job_copy(job, layout->pInputElementDescs,
               (size_t)layout->NumElements * sizeof(*elements));
   layout->pInputElementDescs = elements;
   if (!elements) {
      layout->NumElements = 0;
      return;
   }
   for (UINT i = 0; i < layout->NumElements; i++)
      elements[i].SemanticName = (const void *)job_copy_string(job, (const char *)elements[i].SemanticName);
}

static void
job_copy_stream_output(struct npt_async_pso_job *job,
                       D3D12_STREAM_OUTPUT_DESC *so)
{
   D3D12_SO_DECLARATION_ENTRY *entries =
      job_copy(job, so->pSODeclaration,
               (size_t)so->NumEntries * sizeof(*entries));
   so->pSODeclaration = entries;
   if (!entries)
      so->NumEntries = 0;
   for (UINT i = 0; i < so->NumEntries; i++)
      entries[i].SemanticName = (const void *)job_copy_string(job, (const char *)entries[i].SemanticName);
   so->pBufferStrides = job_copy(job, so->pBufferStrides,
                                 (size_t)so->NumStrides * sizeof(UINT));
   if (!so->pBufferStrides)
      so->NumStrides = 0;
}

static void
job_copy_cached(struct npt_async_pso_job *job, D3D12_CACHED_PIPELINE_STATE *c)
{
   c->pCachedBlob = job_copy(job, c->pCachedBlob, c->CachedBlobSizeInBytes);
   if (!c->pCachedBlob)
      c->CachedBlobSizeInBytes = 0;
}

static void
job_keep_root_signature(struct npt_async_pso_job *job, void *root_signature)
{
   if (root_signature && !job->root_signature) {
      npt_com_add_ref(root_signature);
      job->root_signature = root_signature;
   }
}

static bool
job_copy_stream(struct npt_async_pso_job *job,
                const D3D12_PIPELINE_STATE_STREAM_DESC *src)
{
   uint8_t *stream =
      job_copy(job, src->pPipelineStateSubobjectStream, src->SizeInBytes);
   if (!stream)
      return false;
   job->desc.stream.SizeInBytes = src->SizeInBytes;
   job->desc.stream.pPipelineStateSubobjectStream = stream;

   size_t offset = 0;
   UINT type;
   const void *at;
   int step;
   while ((step = npt_pso_stream_next(&job->desc.stream, &offset, &type,
                                      &at)) == 1) {
      /* The description lives in the copy; patch its pointers there. */
      void *desc = stream + ((const uint8_t *)at - stream);
      switch (type) {
      case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE: {
         void *rs;
         memcpy(&rs, desc, sizeof(rs));
         job_keep_root_signature(job, rs);
         break;
      }
      case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS:
         job->compute = true;
         job_copy_shader(job, desc);
         break;
      case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS:
      case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS:
      case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DS:
      case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_HS:
      case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_GS:
      case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS:
      case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS:
         job_copy_shader(job, desc);
         break;
      case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_STREAM_OUTPUT:
         job_copy_stream_output(job, desc);
         break;
      case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_INPUT_LAYOUT:
         job_copy_input_layout(job, desc);
         break;
      case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CACHED_PSO:
         job_copy_cached(job, desc);
         break;
      case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VIEW_INSTANCING: {
         D3D12_VIEW_INSTANCING_DESC *vi = desc;
         vi->pViewInstanceLocations =
            job_copy(job, vi->pViewInstanceLocations,
                     (size_t)vi->ViewInstanceCount *
                        sizeof(D3D12_VIEW_INSTANCE_LOCATION));
         if (!vi->pViewInstanceLocations)
            vi->ViewInstanceCount = 0;
         break;
      }
      default:
         break;
      }
   }
   return step == 0;
}

static void
job_free(struct npt_async_pso_job *job)
{
   for (unsigned i = 0; i < job->block_count; i++)
      free(job->blocks[i]);
   free(job->blocks);
   if (job->root_signature)
      npt_com_release(job->root_signature);
   if (job->device)
      npt_com_release(job->device);
   free(job);
}

/* ---------- workers ---------- */

static void
job_run(struct npt_async_pso_job *job)
{
   void *pipeline = NULL;
   HRESULT hr;
   const uint64_t t0 = npt_profile_now_ns();

   switch (job->kind) {
   case NPT_ASYNC_PSO_GRAPHICS:
      hr = ((PFN_ID3D12Device_CreateGraphicsPipelineState)job->original)(
         job->device, &job->desc.graphics, &job->riid, &pipeline);
      break;
   case NPT_ASYNC_PSO_COMPUTE:
      hr = ((PFN_ID3D12Device_CreateComputePipelineState)job->original)(
         job->device, &job->desc.compute, &job->riid, &pipeline);
      break;
   default:
      hr = ((PFN_ID3D12Device2_CreatePipelineState)job->original)(
         job->device, &job->desc.stream, &job->riid, &pipeline);
      break;
   }

   const uint64_t dt = npt_profile_now_ns() - t0;
   if (NPT_SUCCEEDED(hr) && pipeline) {
      /* Under the mutex, so that a release sees either a job it can
       * flag or an object it can look up. */
      mtx_lock(&pso.mutex);
      const bool released = job->released;
      if (!released) {
         npt_context_register_object(job->ctx, job->guest_id, pipeline,
                                     npt_object_type_from_iid(&job->riid));
         job->registered = true;
      }
      mtx_unlock(&pso.mutex);
      if (released)
         npt_com_release(pipeline);
   } else if (job->released) {
      /* Nobody is left to use the id. */
   } else {
      /* The guest was told S_OK and will use the id: let it stand for a
       * null pipeline rather than for an id the host has never seen. */
      npt_log("async pipeline 0x%016" PRIx64 ": creation failed, hr=0x%08x",
              job->guest_id, (unsigned)hr);
      npt_context_register_null_object(job->ctx, job->guest_id);
   }

   const uint64_t now = npt_profile_now_ns();
   atomic_store(&pso.last_done_ns, now);
   mtx_lock(&pso.mutex);
   pso.created++;
   pso.total_ns += dt;
   if (dt > pso.max_ns)
      pso.max_ns = dt;
   /* Progress, at most every five seconds while there is work. */
   if (now - pso.last_log_ns >= 5000000000ull) {
      npt_log("async pipeline creation: %" PRIu64 " created, %u waiting or"
              " running, %" PRIu64 " ms of compilation, slowest %" PRIu64
              " ms",
              pso.created, atomic_load(&pso.outstanding),
              pso.total_ns / 1000000ull, pso.max_ns / 1000000ull);
      pso.last_log_ns = now;
      pso.max_ns = 0;
   }
   mtx_unlock(&pso.mutex);
}

static int
npt_async_pso_worker(UNUSED void *arg)
{
   u_thread_setname("npt-pso");
#if defined(__APPLE__)
   /* A waiting ring thread is on the frame's critical path, and nothing
    * else tells the scheduler that this thread's work is what it waits
    * for. */
   pthread_set_qos_class_self_np(QOS_CLASS_USER_INITIATED, 0);
#endif

   mtx_lock(&pso.mutex);
   while (true) {
      while (list_is_empty(&pso.queue))
         cnd_wait(&pso.work, &pso.mutex);

      struct npt_async_pso_job *job =
         list_first_entry(&pso.queue, struct npt_async_pso_job, head);
      list_del(&job->head);
      list_addtail(&job->head, &pso.running);
      mtx_unlock(&pso.mutex);

      job_run(job);

      mtx_lock(&pso.mutex);
      list_del(&job->head);
      atomic_fetch_sub(&pso.outstanding, 1);
      cnd_broadcast(&pso.done);
      mtx_unlock(&pso.mutex);
      job_free(job);
      mtx_lock(&pso.mutex);
   }
   return 0;
}

static void
npt_async_pso_init(void)
{
   const char *e = getenv("NPT_ASYNC_PSO");
   if (e && e[0] == '0') {
      pso_enabled = 0;
      return;
   }

   mtx_init(&pso.mutex, mtx_plain);
   cnd_init(&pso.work);
   cnd_init(&pso.done);
   list_inithead(&pso.queue);
   list_inithead(&pso.running);

   /* More workers than this do not compile more: on an 18-core Apple
    * M5 Max, nine took 240 ms a pipeline where six and four took 88, and
    * the game they share the machine with dropped to a few frames a
    * second. */
   long n = sysconf(_SC_NPROCESSORS_ONLN) / 3;
   if (n > 6)
      n = 6;
   const char *t = getenv("NPT_ASYNC_PSO_THREADS");
   if (t && atoi(t) > 0)
      n = atoi(t);
   if (n < 2)
      n = 2;
   if (n > 16)
      n = 16;

   for (long i = 0; i < n; i++) {
      thrd_t thread;
      if (thrd_create(&thread, npt_async_pso_worker, NULL) == thrd_success) {
         thrd_detach(thread);
         pso.threads++;
      }
   }
   pso_enabled = pso.threads > 0;
   npt_log("async pipeline creation: %u worker threads", pso.threads);
}

static bool
npt_async_pso_on(void)
{
   call_once(&pso_once, npt_async_pso_init);
   return pso_enabled > 0;
}

/* ---------- entry points ---------- */

static struct npt_async_pso_job *
job_new(struct npt_dispatch_context *dispatch, enum npt_async_pso_kind kind,
        void *device, void *original, const IID *riid, uint64_t guest_id)
{
   if (!guest_id || !riid || !device)
      return NULL;
   struct npt_async_pso_job *job = calloc(1, sizeof(*job));
   if (!job)
      return NULL;
   job->ctx = npt_context_from_dispatch(dispatch);
   job->guest_id = guest_id;
   job->kind = kind;
   job->original = original;
   job->riid = *riid;
   npt_com_add_ref(device);
   job->device = device;
   return job;
}

static bool
job_submit(struct npt_async_pso_job *job, bool copied)
{
   if (!copied || job->failed_copy) {
      job_free(job);
      return false;
   }
   mtx_lock(&pso.mutex);
   atomic_fetch_add(&pso.outstanding, 1);
   list_addtail(&job->head, &pso.queue);
   cnd_signal(&pso.work);
   mtx_unlock(&pso.mutex);
   return true;
}

bool
npt_async_pso_create_graphics(struct npt_dispatch_context *dispatch,
                              void *device,
                              const D3D12_GRAPHICS_PIPELINE_STATE_DESC *desc,
                              const IID *riid, uint64_t guest_id,
                              void *original)
{
   if (!desc || !npt_async_pso_on())
      return false;
   struct npt_async_pso_job *job = job_new(
      dispatch, NPT_ASYNC_PSO_GRAPHICS, device, original, riid, guest_id);
   if (!job)
      return false;

   D3D12_GRAPHICS_PIPELINE_STATE_DESC *d = &job->desc.graphics;
   *d = *desc;
   job_keep_root_signature(job, d->pRootSignature);
   job_copy_shader(job, &d->VS);
   job_copy_shader(job, &d->PS);
   job_copy_shader(job, &d->DS);
   job_copy_shader(job, &d->HS);
   job_copy_shader(job, &d->GS);
   job_copy_stream_output(job, &d->StreamOutput);
   job_copy_input_layout(job, &d->InputLayout);
   job_copy_cached(job, &d->CachedPSO);
   return job_submit(job, true);
}

bool
npt_async_pso_create_compute(struct npt_dispatch_context *dispatch,
                             void *device,
                             const D3D12_COMPUTE_PIPELINE_STATE_DESC *desc,
                             const IID *riid, uint64_t guest_id,
                             void *original)
{
   if (!desc || !npt_async_pso_on())
      return false;
   struct npt_async_pso_job *job = job_new(
      dispatch, NPT_ASYNC_PSO_COMPUTE, device, original, riid, guest_id);
   if (!job)
      return false;

   D3D12_COMPUTE_PIPELINE_STATE_DESC *d = &job->desc.compute;
   *d = *desc;
   job->compute = true;
   job_keep_root_signature(job, d->pRootSignature);
   job_copy_shader(job, &d->CS);
   job_copy_cached(job, &d->CachedPSO);
   return job_submit(job, true);
}

bool
npt_async_pso_create_stream(struct npt_dispatch_context *dispatch,
                            void *device,
                            const D3D12_PIPELINE_STATE_STREAM_DESC *desc,
                            const IID *riid, uint64_t guest_id,
                            void *original)
{
   if (!desc || !desc->pPipelineStateSubobjectStream ||
       !npt_async_pso_on())
      return false;
   struct npt_async_pso_job *job = job_new(
      dispatch, NPT_ASYNC_PSO_STREAM, device, original, riid, guest_id);
   if (!job)
      return false;
   return job_submit(job, job_copy_stream(job, desc));
}

static struct npt_async_pso_job *
find_job(struct list_head *list, const struct npt_context *ctx, uint64_t id)
{
   list_for_each_entry (struct npt_async_pso_job, job, list, head) {
      if (job->guest_id == id && job->ctx == ctx)
         return job;
   }
   return NULL;
}

bool
npt_async_pso_wait(struct npt_context *ctx, uint64_t guest_id)
{
   if (pso_enabled <= 0)
      return false;

   bool waited = false;
   mtx_lock(&pso.mutex);
   while (true) {
      struct npt_async_pso_job *job = find_job(&pso.queue, ctx, guest_id);
      if (job) {
         /* Somebody needs this one now: next in line. */
         list_del(&job->head);
         list_add(&job->head, &pso.queue);
      } else if (!find_job(&pso.running, ctx, guest_id)) {
         break;
      }
      waited = true;
      cnd_wait(&pso.done, &pso.mutex);
   }
   mtx_unlock(&pso.mutex);
   return waited;
}

bool
npt_async_pso_release_pending(struct npt_context *ctx, uint64_t guest_id)
{
   if (pso_enabled <= 0)
      return false;

   bool pending = false;
   mtx_lock(&pso.mutex);
   struct npt_async_pso_job *job = find_job(&pso.queue, ctx, guest_id);
   if (!job)
      job = find_job(&pso.running, ctx, guest_id);
   if (job && !job->registered) {
      job->released = true;
      pending = true;
   }
   mtx_unlock(&pso.mutex);
   return pending;
}

uint64_t
npt_async_pso_last_activity_ns(void)
{
   if (pso_enabled <= 0)
      return 0;
   if (atomic_load(&pso.outstanding))
      return npt_profile_now_ns();
   return atomic_load(&pso.last_done_ns);
}

void
npt_async_pso_drain(struct npt_context *ctx)
{
   if (pso_enabled <= 0)
      return;

   /* Jobs not started are dropped: nothing will use their pipelines. */
   struct list_head dropped;
   list_inithead(&dropped);
   mtx_lock(&pso.mutex);
   list_for_each_entry_safe (struct npt_async_pso_job, job, &pso.queue,
                             head) {
      if (job->ctx == ctx) {
         list_del(&job->head);
         list_addtail(&job->head, &dropped);
         atomic_fetch_sub(&pso.outstanding, 1);
      }
   }
   cnd_broadcast(&pso.done);
   while (true) {
      bool busy = false;
      list_for_each_entry (struct npt_async_pso_job, job, &pso.running,
                           head) {
         if (job->ctx == ctx)
            busy = true;
      }
      if (!busy)
         break;
      cnd_wait(&pso.done, &pso.mutex);
   }
   if (pso.created)
      npt_log("async pipeline creation: %" PRIu64 " pipelines, %" PRIu64
              " ms of compilation, %" PRIu64 " draws and dispatches left"
              " out while their pipeline compiled",
              pso.created, pso.total_ns / 1000000ull,
              atomic_load(&pso_skipped_draws));
   mtx_unlock(&pso.mutex);

   list_for_each_entry_safe (struct npt_async_pso_job, job, &dropped, head) {
      list_del(&job->head);
      job_free(job);
   }
}

/* ---------- command lists that do not wait ---------- */

/* Set while the ring thread decodes a command that may name a pending
 * pipeline without waiting for it; the lookup then reports the pipeline
 * here instead. */
static _Thread_local bool pso_nowait;
static _Thread_local uint64_t pso_skipped_id;

/* Lists whose current pipeline is not compiled yet.  Keys are never
 * removed, so a probe needs no lock; pso_skipping counts the marked lists
 * and keeps the table off the draw path while it is zero. */
#define NPT_PSO_LIST_SLOTS 4096
static struct {
   _Atomic(void *) list;
   _Atomic bool skip;
} pso_lists[NPT_PSO_LIST_SLOTS];
static _Atomic uint32_t pso_skipping;
static int pso_skip_enabled = -1;

static bool
npt_async_pso_skip_on(void)
{
   if (pso_skip_enabled < 0) {
      /* 0: lists wait; 1: draws are left out; 2: dispatches too. */
      const char *e = getenv("NPT_ASYNC_PSO_SKIP");
      pso_skip_enabled = e && e[0] >= '0' && e[0] <= '2' ? e[0] - '0' : 1;
   }
   return pso_skip_enabled > 0;
}

void
npt_async_pso_begin_command(uint32_t cmd_type)
{
   if (pso_enabled <= 0)
      return;
   const uint32_t list =
      NPT_CMD_TYPE(255, NPT_IFACE_ID_ID3D12GraphicsCommandList, 0);
   pso_nowait =
      (cmd_type & ~0xFFu) == list &&
      ((cmd_type & 0xFFu) == NPT_METHOD_ID3D12GraphicsCommandList_Reset ||
       (cmd_type & 0xFFu) == NPT_METHOD_ID3D12GraphicsCommandList_ClearState ||
       (cmd_type & 0xFFu) ==
          NPT_METHOD_ID3D12GraphicsCommandList_SetPipelineState) &&
      npt_async_pso_skip_on();
   pso_skipped_id = 0;
}

void
npt_async_pso_end_command(void)
{
   pso_nowait = false;
}

static int
list_slot(void *list, bool insert)
{
   uint32_t at = (uint32_t)(((uintptr_t)list >> 4) * 2654435761u) %
                 NPT_PSO_LIST_SLOTS;
   for (unsigned i = 0; i < NPT_PSO_LIST_SLOTS; i++) {
      void *key = atomic_load(&pso_lists[at].list);
      if (key == list)
         return (int)at;
      if (!key) {
         if (!insert)
            return -1;
         void *expected = NULL;
         if (atomic_compare_exchange_strong(&pso_lists[at].list, &expected,
                                            list) ||
             expected == list)
            return (int)at;
      }
      at = (at + 1) % NPT_PSO_LIST_SLOTS;
   }
   return -1;
}

static bool
list_skips(void *list)
{
   if (likely(!atomic_load_explicit(&pso_skipping, memory_order_relaxed)))
      return false;
   const int at = list_slot(list, false);
   if (at < 0 || !atomic_load(&pso_lists[at].skip))
      return false;
   atomic_fetch_add_explicit(&pso_skipped_draws, 1, memory_order_relaxed);
   return true;
}

/* Marks or unmarks the list; false if it cannot be marked. */
static bool
list_set_skip(void *list, bool skip)
{
   const int at = list_slot(list, skip);
   if (at < 0)
      return !skip;
   if (atomic_exchange(&pso_lists[at].skip, skip) != skip) {
      if (skip)
         atomic_fetch_add(&pso_skipping, 1);
      else
         atomic_fetch_sub(&pso_skipping, 1);
   }
   return true;
}

/* The pipeline a list command names, after the decode: the decoded
 * pointer, or -- when the lookup left out a pending pipeline -- nothing
 * and the list marked, or the pipeline once it has been waited for. */
static ID3D12PipelineState *
list_pipeline(struct npt_dispatch_context *dispatch, void *list,
              ID3D12PipelineState *decoded, bool *skip)
{
   const uint64_t id = pso_skipped_id;
   pso_skipped_id = 0;
   *skip = false;
   if (!id) {
      if (atomic_load_explicit(&pso_skipping, memory_order_relaxed))
         list_set_skip(list, false);
      return decoded;
   }
   if (list_set_skip(list, true)) {
      *skip = true;
      return NULL;
   }
   /* No room to mark the list: wait after all. */
   struct npt_context *ctx = npt_context_from_dispatch(dispatch);
   npt_async_pso_wait(ctx, id);
   return npt_context_lookup_object(ctx, NULL, id,
                                    NPT_OBJECT_TYPE_IUNKNOWN);
}

static void
npt_pso_list_SetPipelineState(
   struct npt_dispatch_context *ctx,
   struct npt_command_ID3D12GraphicsCommandList_SetPipelineState *args,
   PFN_ID3D12GraphicsCommandList_SetPipelineState original)
{
   bool skip;
   ID3D12PipelineState *pipeline =
      list_pipeline(ctx, args->_self, args->pPipelineState, &skip);
   if (!skip)
      original(args->_self, pipeline);
}

static HRESULT
npt_pso_list_Reset(struct npt_dispatch_context *ctx,
                   struct npt_command_ID3D12GraphicsCommandList_Reset *args,
                   PFN_ID3D12GraphicsCommandList_Reset original)
{
   bool skip;
   ID3D12PipelineState *pipeline =
      list_pipeline(ctx, args->_self, args->pInitialState, &skip);
   args->ret = original(args->_self, args->pAllocator, pipeline);
   return args->ret;
}

static void
npt_pso_list_ClearState(
   struct npt_dispatch_context *ctx,
   struct npt_command_ID3D12GraphicsCommandList_ClearState *args,
   PFN_ID3D12GraphicsCommandList_ClearState original)
{
   bool skip;
   ID3D12PipelineState *pipeline =
      list_pipeline(ctx, args->_self, args->pPipelineState, &skip);
   original(args->_self, pipeline);
}

static void
npt_pso_list_DrawInstanced(
   UNUSED struct npt_dispatch_context *ctx,
   struct npt_command_ID3D12GraphicsCommandList_DrawInstanced *args,
   PFN_ID3D12GraphicsCommandList_DrawInstanced original)
{
   if (!list_skips(args->_self))
      original(args->_self, args->VertexCountPerInstance,
               args->InstanceCount, args->StartVertexLocation,
               args->StartInstanceLocation);
}

static void
npt_pso_list_DrawIndexedInstanced(
   UNUSED struct npt_dispatch_context *ctx,
   struct npt_command_ID3D12GraphicsCommandList_DrawIndexedInstanced *args,
   PFN_ID3D12GraphicsCommandList_DrawIndexedInstanced original)
{
   if (!list_skips(args->_self))
      original(args->_self, args->IndexCountPerInstance, args->InstanceCount,
               args->StartIndexLocation, args->BaseVertexLocation,
               args->StartInstanceLocation);
}

static void
npt_pso_list_Dispatch(
   UNUSED struct npt_dispatch_context *ctx,
   struct npt_command_ID3D12GraphicsCommandList_Dispatch *args,
   PFN_ID3D12GraphicsCommandList_Dispatch original)
{
   if (!list_skips(args->_self))
      original(args->_self, args->ThreadGroupCountX, args->ThreadGroupCountY,
               args->ThreadGroupCountZ);
}

static void
npt_pso_list_ExecuteIndirect(
   UNUSED struct npt_dispatch_context *ctx,
   struct npt_command_ID3D12GraphicsCommandList_ExecuteIndirect *args,
   PFN_ID3D12GraphicsCommandList_ExecuteIndirect original)
{
   if (!list_skips(args->_self))
      original(args->_self, args->pCommandSignature, args->MaxCommandCount,
               args->pArgumentBuffer, args->ArgumentBufferOffset,
               args->pCountBuffer, args->CountBufferOffset);
}

static void
npt_pso_list_DispatchMesh(
   UNUSED struct npt_dispatch_context *ctx,
   struct npt_command_ID3D12GraphicsCommandList6_DispatchMesh *args,
   PFN_ID3D12GraphicsCommandList6_DispatchMesh original)
{
   if (!list_skips(args->_self))
      original(args->_self, args->ThreadGroupCountX, args->ThreadGroupCountY,
               args->ThreadGroupCountZ);
}

struct npt_dispatch_id3d12graphicscommandlist_overrides
   npt_async_pso_list_overrides = {
   .Reset = npt_pso_list_Reset,
   .ClearState = npt_pso_list_ClearState,
   .DrawInstanced = npt_pso_list_DrawInstanced,
   .DrawIndexedInstanced = npt_pso_list_DrawIndexedInstanced,
   .Dispatch = npt_pso_list_Dispatch,
   .SetPipelineState = npt_pso_list_SetPipelineState,
   .ExecuteIndirect = npt_pso_list_ExecuteIndirect,
};

struct npt_dispatch_id3d12graphicscommandlist6_overrides
   npt_async_pso_list6_overrides = {
   .DispatchMesh = npt_pso_list_DispatchMesh,
};

bool
npt_async_pso_lookup_pending(struct npt_context *ctx, uint64_t guest_id)
{
   if (pso_enabled <= 0)
      return false;
   if (!pso_nowait)
      return npt_async_pso_wait(ctx, guest_id);

   /* Needed now, even if nobody waits: next in line. */
   bool pending = false;
   bool compute = false;
   mtx_lock(&pso.mutex);
   struct npt_async_pso_job *job = find_job(&pso.queue, ctx, guest_id);
   if (job) {
      list_del(&job->head);
      list_add(&job->head, &pso.queue);
      pending = true;
   } else if ((job = find_job(&pso.running, ctx, guest_id))) {
      pending = true;
   }
   if (job)
      compute = job->compute;
   mtx_unlock(&pso.mutex);
   /* A draw left out is a hole in one frame's picture.  A dispatch left
    * out is data that was never computed, and an engine reads such data
    * back and indexes with it: compute pipelines are waited for. */
   if (pending && compute && pso_skip_enabled < 2)
      return npt_async_pso_wait(ctx, guest_id);
   if (pending)
      pso_skipped_id = guest_id;
   return false;
}

bool
npt_async_pso_lookup_skipped(void)
{
   return pso_skipped_id != 0;
}
