/*
 * GL contexts for virgl (vrend) on macOS, through CGL (--use-cgl).
 *
 * macOS has neither EGL nor GLX, so vrend gets its contexts from the caller's callbacks, as under QEMU: the core
 * profile of OpenGL 4.1, the newest macOS offers, whatever version vrend asks for. vrend renders into its own
 * framebuffer objects, so the contexts need no drawable. The first context, vrend's ctx0, is the one every shared
 * context joins.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#ifdef __APPLE__

#define GL_SILENCE_DEPRECATION 1

#include <OpenGL/OpenGL.h>
#include <stdio.h>

#include "vtest_cgl.h"

static CGLContextObj root_context;

virgl_renderer_gl_context
vtest_cgl_create_context(UNUSED void *cookie, UNUSED int scanout_idx,
                         struct virgl_renderer_gl_ctx_param *param)
{
   const CGLPixelFormatAttribute attributes[] = {
      kCGLPFAOpenGLProfile, (CGLPixelFormatAttribute)kCGLOGLPVersion_GL4_Core,
      kCGLPFAAccelerated,
      kCGLPFAAllowOfflineRenderers,
      0
   };
   CGLPixelFormatObj format = NULL;
   CGLContextObj context = NULL;
   GLint count = 0;
   CGLError error;

   error = CGLChoosePixelFormat(attributes, &format, &count);
   if (error != kCGLNoError || !format) {
      fprintf(stderr, "vtest: CGLChoosePixelFormat failed: %s\n", CGLErrorString(error));
      return NULL;
   }
   error = CGLCreateContext(format, param->shared ? root_context : NULL, &context);
   CGLReleasePixelFormat(format);
   if (error != kCGLNoError) {
      fprintf(stderr, "vtest: CGLCreateContext (GL %d.%d asked, shared %d) failed: %s\n",
              param->major_ver, param->minor_ver, param->shared, CGLErrorString(error));
      return NULL;
   }
   if (!root_context)
      root_context = context;
   return context;
}

void
vtest_cgl_destroy_context(UNUSED void *cookie, virgl_renderer_gl_context context)
{
   if (CGLGetCurrentContext() == context)
      CGLSetCurrentContext(NULL);
   if (context == root_context)
      root_context = NULL;
   CGLDestroyContext(context);
}

int
vtest_cgl_make_current(UNUSED void *cookie, UNUSED int scanout_idx, virgl_renderer_gl_context context)
{
   return CGLSetCurrentContext(context) == kCGLNoError ? 0 : -1;
}

#endif /* __APPLE__ */
