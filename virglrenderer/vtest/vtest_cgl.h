/*
 * GL contexts for virgl (vrend) on macOS, through CGL: see vtest_cgl.c.
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

#ifndef VTEST_CGL_H
#define VTEST_CGL_H

#ifdef __APPLE__

#include "virglrenderer.h"

#ifndef UNUSED
#define UNUSED __attribute__((unused))
#endif

virgl_renderer_gl_context
vtest_cgl_create_context(void *cookie, int scanout_idx, struct virgl_renderer_gl_ctx_param *param);

void
vtest_cgl_destroy_context(void *cookie, virgl_renderer_gl_context context);

int
vtest_cgl_make_current(void *cookie, int scanout_idx, virgl_renderer_gl_context context);

#endif /* __APPLE__ */

#endif /* VTEST_CGL_H */
