/*
 * Obelisk: OpenGL presents without a readback (macOS, --use-cgl). See vtest_present.h.
 *
 * At a marker the frame is blitted, in the guest's GL context, into the IOSurface of the buffer the marker names, and a
 * fence follows it. A worker thread with a GL context of the same share group waits for the fence, copies the IOSurface
 * into the buffer with Metal and writes the trailer, so neither the guest nor the server's other clients wait for the
 * copy. Each buffer has an IOSurface of its own: the guest fills a buffer again only once the window server released it,
 * after the trailer.
 *
 * Manual reference counting (the project builds Objective-C without ARC).
 */
#define GL_SILENCE_DEPRECATION /* the GL the server serves its guests with */
#include "vtest_present.h"

#import <Foundation/Foundation.h>
#import <IOSurface/IOSurface.h>
#import <Metal/Metal.h>
#include <OpenGL/CGLIOSurface.h>
#include <OpenGL/OpenGL.h>
#include <OpenGL/gl3.h>

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define MAX_CHAINS 16
#define MAX_BUFFERS 8
#define MAX_JOBS (MAX_CHAINS * MAX_BUFFERS)
#define PRESENT_MIN(a, b) ((a) < (b) ? (a) : (b))

struct present_buffer {
   bool valid;
   void *map;
   size_t map_len;
   id<MTLBuffer> mtl;
   uint32_t width, height, stride, format;
   /* the frame goes from the guest's texture into this IOSurface with GL, and from it into the buffer with Metal */
   IOSurfaceRef surface;
   GLuint surface_tex;
   id<MTLTexture> surface_mtl;
};

struct present_chain {
   bool used;
   const void *owner;
   uint64_t token;
   struct present_buffer buf[MAX_BUFFERS];
   uint64_t presents, failures;
   uint32_t last_texture;
};

/* a frame for the worker; it holds references, so a chain can go (its client disconnects) while its frames are copied */
struct present_job {
   GLsync sync;            /* the GL copy's fence; NULL: the frame failed, only the trailer is written */
   IOSurfaceRef surface;
   id<MTLTexture> texture;
   id<MTLBuffer> buffer;   /* keeps the mapping */
   uint32_t width, height, stride;
   uint64_t serial;
};

static struct present_chain chains[MAX_CHAINS];
static id<MTLDevice> device;
static id<MTLCommandQueue> queue;

static pthread_mutex_t job_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t job_cond = PTHREAD_COND_INITIALIZER; /* job_lock held: a job was queued or finished */
static unsigned job_head, job_count; /* the job at job_head stays queued until it is done */
static enum { WORKER_NONE, WORKER_RUNNING, WORKER_FAILED } worker_state;
static CGLContextObj worker_context;

static bool metal_ready(void)
{
   if (!device) {
      device = MTLCreateSystemDefaultDevice();
      if (device)
         queue = [device newCommandQueue];
   }
   return device && queue;
}

static void surface_release(struct present_buffer *b)
{
   if (b->surface_tex) {
      /* a name of the share group of every vrend context; deleting it needs one of them current. GL keeps the
       * texture while a queued copy still uses it. */
      if (CGLGetCurrentContext())
         glDeleteTextures(1, &b->surface_tex);
      b->surface_tex = 0;
   }
   if (b->surface_mtl) {
      [b->surface_mtl release];
      b->surface_mtl = nil;
   }
   if (b->surface) {
      CFRelease(b->surface);
      b->surface = NULL;
   }
}

static void buffer_release(struct present_buffer *b)
{
   if (!b->valid)
      return;
   surface_release(b);
   [b->mtl release]; /* its deallocator unmaps, once no job holds it */
   memset(b, 0, sizeof(*b));
}

static void chain_release(struct present_chain *c)
{
   for (int i = 0; i < MAX_BUFFERS; i++)
      buffer_release(&c->buf[i]);
   memset(c, 0, sizeof(*c));
}

static struct present_chain *chain_find(uint64_t token)
{
   for (int i = 0; i < MAX_CHAINS; i++)
      if (chains[i].used && chains[i].token == token)
         return &chains[i];
   return NULL;
}

int vtest_present_attach(const void *owner, uint64_t token, uint32_t index, uint32_t width, uint32_t height,
                         uint32_t stride, uint32_t format, uint64_t size, int fd)
{
   const size_t page = (size_t)getpagesize();
   struct present_chain *c;
   struct stat st;

   if (!metal_ready() || index >= MAX_BUFFERS || !width || !height || width > 16384 || height > 16384 ||
       format != 1 /* BGRA8, as wsd's WSD_FMT_BGRA8 */ || stride < width * 4 || stride % 4 ||
       size < (uint64_t)stride * height + VTEST_PRESENT_TRAILER_SIZE || size % page ||
       fstat(fd, &st) || (uint64_t)st.st_size < size) {
      fprintf(stderr, "vtest present: attach of buffer %u (%ux%u stride %u format %u size %llu) refused\n", index,
              width, height, stride, format, (unsigned long long)size);
      close(fd);
      return -EINVAL;
   }

   if (!(c = chain_find(token))) {
      for (int i = 0; i < MAX_CHAINS && !c; i++)
         if (!chains[i].used)
            c = &chains[i];
      if (!c) {
         fprintf(stderr, "vtest present: no room for another present chain\n");
         close(fd);
         return -ENOSPC;
      }
      c->used = true;
      c->owner = owner;
      c->token = token;
   } else if (c->owner != owner) {
      close(fd);
      return -EPERM;
   }

   void *map = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
   close(fd);
   if (map == MAP_FAILED) {
      fprintf(stderr, "vtest present: mmap of buffer %u failed: %s\n", index, strerror(errno));
      return -ENOMEM;
   }
   size_t len = size;
   id<MTLBuffer> mtl = [device newBufferWithBytesNoCopy:map
                                                 length:len
                                                options:MTLResourceStorageModeShared
                                            deallocator:^(void *pointer, NSUInteger length) {
                                               munmap(pointer, length);
                                            }];
   if (!mtl) {
      munmap(map, len);
      fprintf(stderr, "vtest present: Metal refused buffer %u\n", index);
      return -ENOMEM;
   }

   struct present_buffer *b = &c->buf[index];
   buffer_release(b);
   b->valid = true;
   b->map = map;
   b->map_len = len;
   b->mtl = mtl;
   b->width = width;
   b->height = height;
   b->stride = stride;
   b->format = format;
   fprintf(stderr, "vtest present: chain %llx buffer %u attached, %ux%u stride %u\n", (unsigned long long)token,
           index, width, height, stride);
   return 0;
}

int vtest_present_detach(const void *owner, uint64_t token)
{
   struct present_chain *c = chain_find(token);
   if (!c || c->owner != owner)
      return -ENOENT;
   fprintf(stderr, "vtest present: chain %llx detached after %llu frames\n", (unsigned long long)token,
           (unsigned long long)c->presents);
   chain_release(c);
   return 0;
}

void vtest_present_forget_owner(const void *owner)
{
   for (int i = 0; i < MAX_CHAINS; i++)
      if (chains[i].used && chains[i].owner == owner)
         chain_release(&chains[i]);
}

static bool surface_ready(struct present_buffer *b)
{
   if (b->surface)
      return true;

   NSDictionary *props = @{
      (id)kIOSurfaceWidth : @(b->width),
      (id)kIOSurfaceHeight : @(b->height),
      (id)kIOSurfaceBytesPerElement : @4,
      (id)kIOSurfacePixelFormat : @((uint32_t)'BGRA'),
   };
   b->surface = IOSurfaceCreate((CFDictionaryRef)props);
   if (!b->surface) {
      fprintf(stderr, "vtest present: no IOSurface of %ux%u\n", b->width, b->height);
      return false;
   }

   glGenTextures(1, &b->surface_tex);
   GLint prev = 0;
   glGetIntegerv(GL_TEXTURE_BINDING_RECTANGLE, &prev);
   glBindTexture(GL_TEXTURE_RECTANGLE, b->surface_tex);
   CGLError err = CGLTexImageIOSurface2D(CGLGetCurrentContext(), GL_TEXTURE_RECTANGLE, GL_RGBA8, b->width, b->height,
                                         GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, b->surface, 0);
   glBindTexture(GL_TEXTURE_RECTANGLE, prev);
   if (err != kCGLNoError) {
      fprintf(stderr, "vtest present: CGLTexImageIOSurface2D failed (%d)\n", err);
      surface_release(b);
      return false;
   }

   MTLTextureDescriptor *desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                    width:b->width
                                                                                   height:b->height
                                                                                mipmapped:NO];
   desc.usage = MTLTextureUsageShaderRead;
   b->surface_mtl = [device newTextureWithDescriptor:desc iosurface:b->surface plane:0];
   if (!b->surface_mtl) {
      fprintf(stderr, "vtest present: no Metal texture for the IOSurface\n");
      surface_release(b);
      return false;
   }
   return true;
}

/* GL: the guest's texture into the buffer's IOSurface, top row first. The GL state touched is put back as it was:
 * vrend keeps its own bindings and expects them unchanged. */
static bool copy_into_surface(struct present_buffer *b, const struct virgl_renderer_present_source *src,
                              uint32_t width, uint32_t height)
{
   GLint read_fb = 0, draw_fb = 0;
   GLboolean mask[4];
   GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);
   GLboolean srgb = glIsEnabled(GL_FRAMEBUFFER_SRGB);
   GLboolean discard = glIsEnabled(GL_RASTERIZER_DISCARD);
   GLuint fbo[2];
   bool ok;

   glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read_fb);
   glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &draw_fb);
   glGetBooleanv(GL_COLOR_WRITEMASK, mask);

   glGenFramebuffers(2, fbo);
   glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo[0]);
   glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, src->gl_target, src->gl_texture, 0);
   glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo[1]);
   glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_RECTANGLE, b->surface_tex, 0);
   GLenum read_status = glCheckFramebufferStatus(GL_READ_FRAMEBUFFER);
   GLenum draw_status = glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER);
   ok = read_status == GL_FRAMEBUFFER_COMPLETE && draw_status == GL_FRAMEBUFFER_COMPLETE;
   if (!ok) {
      static unsigned reported;
      if (reported++ < 5)
         fprintf(stderr, "vtest present: framebuffers incomplete (frame texture %u target %#x %ux%u format %u: %#x, "
                 "IOSurface texture %u: %#x, GL error %#x, GL context %p)\n", src->gl_texture, src->gl_target,
                 src->width, src->height, src->format, read_status, b->surface_tex, draw_status, glGetError(),
                 (void *)CGLGetCurrentContext());
   }
   if (ok) {
      if (scissor) glDisable(GL_SCISSOR_TEST);
      if (srgb) glDisable(GL_FRAMEBUFFER_SRGB);
      if (discard) glDisable(GL_RASTERIZER_DISCARD);
      glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
      /* GL's rows go bottom up; the IOSurface's, as the window server reads them, top down */
      if (src->y_0_top)
         glBlitFramebuffer(0, 0, width, height, 0, 0, width, height, GL_COLOR_BUFFER_BIT, GL_NEAREST);
      else
         glBlitFramebuffer(0, 0, width, height, 0, height, width, 0, GL_COLOR_BUFFER_BIT, GL_NEAREST);
      glColorMask(mask[0], mask[1], mask[2], mask[3]);
      if (discard) glEnable(GL_RASTERIZER_DISCARD);
      if (srgb) glEnable(GL_FRAMEBUFFER_SRGB);
      if (scissor) glEnable(GL_SCISSOR_TEST);
   }
   glBindFramebuffer(GL_READ_FRAMEBUFFER, read_fb);
   glBindFramebuffer(GL_DRAW_FRAMEBUFFER, draw_fb);
   glDeleteFramebuffers(2, fbo);
   return ok;
}

static bool copy_into_buffer(id<MTLTexture> texture, id<MTLBuffer> buffer, uint32_t width, uint32_t height,
                             uint32_t stride)
{
   bool ok;
   @autoreleasepool {
      id<MTLCommandBuffer> cmd = [queue commandBuffer];
      id<MTLBlitCommandEncoder> blit = [cmd blitCommandEncoder];
      [blit copyFromTexture:texture
                     sourceSlice:0
                     sourceLevel:0
                    sourceOrigin:MTLOriginMake(0, 0, 0)
                      sourceSize:MTLSizeMake(width, height, 1)
                        toBuffer:buffer
               destinationOffset:0
          destinationBytesPerRow:stride
        destinationBytesPerImage:(NSUInteger)stride * height];
      [blit endEncoding];
      [cmd commit];
      [cmd waitUntilCompleted];
      ok = cmd.status == MTLCommandBufferStatusCompleted;
   }
   return ok;
}

/* the trailer follows the buffer's stride * height bytes (the buffer's own height, not the frame's) */
static void write_trailer(void *map, size_t offset, uint32_t status, uint64_t serial)
{
   uint8_t *t = (uint8_t *)map + offset;
   ((volatile uint32_t *)t)[0] = VTEST_PRESENT_TRAILER_MAGIC;
   ((volatile uint32_t *)t)[1] = status;
   __atomic_store_n((uint64_t *)(t + 8), serial, __ATOMIC_RELEASE);
}

/* waits for the GL copy, then copies with Metal; frees the job's references */
static bool run_job(struct present_job *job)
{
   bool ok = false;
   if (job->sync) {
      GLenum r = glClientWaitSync(job->sync, 0, 1000000000ull);
      glDeleteSync(job->sync);
      ok = (r == GL_ALREADY_SIGNALED || r == GL_CONDITION_SATISFIED) &&
           copy_into_buffer(job->texture, job->buffer, job->width, job->height, job->stride);
   }
   return ok;
}

static void job_done(struct present_job *job, bool ok, size_t trailer)
{
   write_trailer(job->buffer.contents, trailer, ok ? 0 : 1, job->serial);
   [job->texture release];
   [job->buffer release];
   if (job->surface)
      CFRelease(job->surface);
}

struct queued_job {
   struct present_job job;
   size_t trailer;
};
static struct queued_job queued[MAX_JOBS];

static void *worker_main(__attribute__((unused)) void *arg)
{
   pthread_setname_np("vtest present");
   CGLSetCurrentContext(worker_context);
   for (;;) {
      pthread_mutex_lock(&job_lock);
      while (!job_count)
         pthread_cond_wait(&job_cond, &job_lock);
      struct queued_job q = queued[job_head];
      pthread_mutex_unlock(&job_lock);

      bool copied = q.job.sync != NULL; /* a frame whose GL copy failed has been reported */
      bool ok = run_job(&q.job);
      job_done(&q.job, ok, q.trailer);
      if (copied && !ok)
         fprintf(stderr, "vtest present: frame serial %llu not copied (GL fence or Metal copy failed)\n",
                 (unsigned long long)q.job.serial);

      pthread_mutex_lock(&job_lock);
      job_head = (job_head + 1) % MAX_JOBS;
      job_count--;
      pthread_cond_broadcast(&job_cond);
      pthread_mutex_unlock(&job_lock);
   }
   return NULL;
}

/* a GL context in the share group of the one current now (a vrend context), for the worker */
static bool worker_ready(void)
{
   if (worker_state == WORKER_NONE) {
      CGLContextObj current = CGLGetCurrentContext();
      pthread_t thread;
      worker_state = WORKER_FAILED;
      if (current && CGLCreateContext(CGLGetPixelFormat(current), current, &worker_context) == kCGLNoError) {
         if (!pthread_create(&thread, NULL, worker_main, NULL)) {
            pthread_detach(thread);
            worker_state = WORKER_RUNNING;
         } else {
            CGLDestroyContext(worker_context);
            worker_context = NULL;
         }
      }
      fprintf(stderr, "vtest present: %s\n",
              worker_state == WORKER_RUNNING ? "frames are copied on a worker thread"
                                             : "no worker thread; frames are copied as their markers come");
   }
   return worker_state == WORKER_RUNNING;
}

static void queue_job(const struct present_job *job, size_t trailer)
{
   pthread_mutex_lock(&job_lock);
   while (job_count == MAX_JOBS)
      pthread_cond_wait(&job_cond, &job_lock);
   struct queued_job *q = &queued[(job_head + job_count) % MAX_JOBS];
   q->job = *job;
   q->trailer = trailer;
   job_count++;
   pthread_cond_broadcast(&job_cond);
   pthread_mutex_unlock(&job_lock);
}

void vtest_present_marker(uint32_t ctx_id, const char *marker, uint32_t length,
                          const struct virgl_renderer_present_source *src)
{
   char text[128];
   unsigned long long token = 0, serial = 0;
   unsigned index = 0;
   struct present_chain *c;
   struct present_buffer *b;

   if (length >= sizeof(text))
      return;
   memcpy(text, marker, length);
   text[length] = 0;
   if (sscanf(text, "obelisk-present token=%llx buf=%u serial=%llu", &token, &index, &serial) != 3 ||
       !(c = chain_find(token)) || index >= MAX_BUFFERS || !(b = &c->buf[index])->valid)
      return;

   uint32_t width = src ? PRESENT_MIN(src->width, b->width) : 0;
   uint32_t height = src ? PRESENT_MIN(src->height, b->height) : 0;
   bool ok = src && width && height && surface_ready(b) && copy_into_surface(b, src, width, height);
   struct present_job job = {
      .sync = ok ? glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0) : NULL,
      .surface = b->surface ? (IOSurfaceRef)CFRetain(b->surface) : NULL,
      .texture = [b->surface_mtl retain],
      .buffer = [b->mtl retain],
      .width = width,
      .height = height,
      .stride = b->stride,
      .serial = serial,
   };
   size_t trailer = (size_t)b->stride * b->height;
   ok = ok && job.sync;
   if (job.sync)
      glFlush(); /* the fence must reach the GPU before another context waits for it */

   if (worker_ready()) {
      queue_job(&job, trailer);
   } else {
      bool copied = run_job(&job);
      job_done(&job, copied, trailer);
      ok = ok && copied;
   }

   if (ok) {
      if (!c->presents++ || src->gl_texture != c->last_texture)
         fprintf(stderr, "vtest present: chain %llx (context %u) presents %ux%u frames from GL texture %u (GL context %p)\n",
                 token, ctx_id, width, height, src->gl_texture, (void *)CGLGetCurrentContext());
      c->last_texture = src->gl_texture;
   } else if (c->failures++ < 5) {
      fprintf(stderr, "vtest present: chain %llx buffer %u serial %llu not copied (%s)\n", token, index, serial,
              src ? "the GL copy failed" : "no blit destination before the marker");
   }
}
