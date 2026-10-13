/*
 * Writes what the interface has just drawn to a file, so the overlay, the
 * USB Bridge panel and a notice can be photographed without a camera.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

/*
 * Reads the interface back and writes it as a BMP.
 *
 * `name` may be NULL for a timestamped name, or a bare file name. It is always
 * written inside the app's own logs directory; a name carrying a path
 * separator is refused, so a command from the control port cannot write
 * anywhere it likes.
 *
 * `out` receives the path written, or the reason it was not. Returns whether a
 * file was written.
 *
 * MUST be called on the interface thread: it touches the SDL renderer, which
 * belongs to that thread. The control port's commands already run there.
 */
bool ctm_screenshot_write(const char *name, char *out, size_t out_len);
