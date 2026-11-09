/*
 * Obelisk: OpenGL presents without a readback (macOS, --use-cgl).
 *
 * A guest's display side attaches shared-memory buffers to a present chain over its own vtest connection
 * (VCMD_OBELISK_PRESENT_ATTACH, the buffer's fd with SCM_RIGHTS) and gives the same buffers to the window server.
 * To present, the guest's GL context blits the frame into a texture and sends the string marker (a debug marker)
 * "obelisk-present token=<hex> buf=<n> serial=<n>"; at that marker the server copies the texture into the buffer on
 * the GPU (GL into an IOSurface, Metal from it into the buffer) and writes the serial into the buffer's trailer, the
 * VTEST_PRESENT_TRAILER_SIZE bytes after stride * height, which the window server does not show. The GL copy is
 * queued in command order at the marker; the rest happens on a worker thread, and the guest watches the trailer.
 */
#ifndef VTEST_PRESENT_H
#define VTEST_PRESENT_H

#include <stdbool.h>
#include <stdint.h>

#include "virglrenderer.h"

#define VTEST_PRESENT_TRAILER_MAGIC 0x50424c4fu /* "OLBP" */
#define VTEST_PRESENT_TRAILER_SIZE 16          /* uint32 magic, uint32 status (0: shown frame), uint64 serial */

/* Takes the fd (closes it on failure too). owner: the connection, whose chains go with it. */
int vtest_present_attach(const void *owner, uint64_t token, uint32_t index, uint32_t width, uint32_t height,
                         uint32_t stride, uint32_t format, uint64_t size, int fd);
/* the chain's buffers go (the guest detaches them from the window server too); 0 or -ENOENT */
int vtest_present_detach(const void *owner, uint64_t token);
void vtest_present_forget_owner(const void *owner);
void vtest_present_marker(uint32_t ctx_id, const char *marker, uint32_t length,
                          const struct virgl_renderer_present_source *src);

#endif
