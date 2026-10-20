/* The touchpad as the TV's mouse, in Aurora's own screens and its overlay.
 *
 * ⭐⭐ WHY (rhoquinn8217, 2026-10-20): "All touchpads on ds5/ds5e/ds4 control
 * the mouse in the dashboard and when the streaming overlay is up. The
 * controllers can be bridge or released." and, for Settings > USB Bridge,
 * "Enable touchpad as mouse in dashboard and streaming overlay". Default is
 * checked. The dashboard is Aurora's home screen, the one that shows which
 * computer the TV is on.
 *
 * ➡️ So, whenever Aurora's interface has the input -- no stream, or the
 * streaming overlay or a bridge's question up -- a DualSense's, an Edge's or a
 * DualShock 4's touchpad moves a pointer of Aurora's:
 *   - one finger slides it, as on a laptop: a stroke across the whole pad
 *     crosses the screen about one and a quarter times;
 *   - pressing the touchpad in clicks, and held in, it drags (a list scrolls);
 *   - with two fingers down it neither moves nor clicks: two fingers and a
 *     press are the bridge gesture.
 * With the overlay closed in a stream the touchpad goes to the host, as it
 * always has.
 *
 * ⓘ THE POINTER IS AURORA'S, NOT webOS's. webOS draws its pointer for the
 * remote and for mice, and a pad's touchpad never reaches it (upstream grabs
 * the touchpad's event node: gamepad_touchpad_grab). So this is an input
 * device of LVGL's with a cursor LVGL draws: the TV cursor's own picture
 * (input/tv_cursor.h), its tip on the point. It shows while the touchpad is
 * used and goes after a few seconds untouched.
 *
 * ⓘ WHICH PADS. Any the TV holds through SDL, all moving the one pointer. A
 * bridged pad exists only in a stream and is left alone here: the PC already
 * stops seeing its touchpad while the overlay is up (the core's blank), and
 * reading where its finger is needs the core, from a capture of those bytes,
 * which the project takes from its own observation. */

#ifndef TOUCHPAD_POINTER_H
#define TOUCHPAD_POINTER_H

#include <stdbool.h>

typedef struct app_t app_t;
struct app_input_t;

/* Once the UI is up: the input device and its cursor. */
void touchpad_pointer_init(app_t *app);

/* Before the UI goes. */
void touchpad_pointer_deinit(void);

/* Once per pass of the app's loop. `ui_has_input`: Aurora's own screens have
 * the input, not a stream. */
void touchpad_pointer_tick(struct app_input_t *input, bool ui_has_input);

#endif
