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
 *   - a light tap clicks too ("Light touch tap should click"), except while
 *     the pointer is hidden: then the tap only shows where it is;
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
 * ⓘ WHICH PADS. Any the TV holds through SDL, all moving the one pointer, and
 * a bridged one in the overlay: the PC stops seeing its touchpad while the
 * overlay is up (the core's blank), and the core reads where its finger is
 * from the pad's own report (the bridge core's held_touch.inl), measured on a
 * DualSense Edge through its DS5Dongle, 2026-10-20. ⚠️ A bridged DualSense on
 * Bluetooth or a bridged DS4 does not move it yet: their touch bytes are not
 * captured. */

#ifndef TOUCHPAD_POINTER_H
#define TOUCHPAD_POINTER_H

#include <stdbool.h>

typedef struct app_t app_t;
struct app_input_t;

/* At the app's start, before the interface has a display: begins the log. */
void touchpad_pointer_init(app_t *app);

/* The interface's display has just been made (root.c, app_ui_open): the input
 * device and its cursor. */
void touchpad_pointer_ui_opened(app_t *app);

/* The interface's display is about to go (root.c, app_ui_close): both go
 * first. */
void touchpad_pointer_ui_closing(void);

/* At the app's end. */
void touchpad_pointer_deinit(void);

/* Once per pass of the app's loop. `ui_has_input`: Aurora's own screens have
 * the input, not a stream. */
void touchpad_pointer_tick(struct app_input_t *input, bool ui_has_input);

/* ⭐ THE TOUCHPAD AS A CONNECTED MOUSE, IN A STREAM (rhoquinn8217, 2026-10-20):
 * "can you make the touch pad to mouse work the same way as a connected mouse?
 * The intent is to to have the TV cursor come up when I press shift+ctrl+alt+M
 * for the touch pad mouse movement." With the TV cursor on (upstream's
 * absolute mouse mode, input/tv_cursor.h) a connected mouse moves the TV's own
 * pointer and the host is told the point, so the two cursors meet. The
 * touchpad as the host's mouse (Virtual Mouse, or Input's "Send as mouse")
 * told the host only how far it moved. Now, with the TV cursor on, one
 * finger's movement moves the cursor above, the TV cursor's own picture, at
 * this file's speed, and the host is told the point; the touchpad's taps,
 * presses, drags and two-finger scrolls stay upstream's and land there. The
 * cursor goes 4 seconds after the last movement, and the next starts where a
 * connected mouse last left the pointer, if one moved since. With the TV
 * cursor off nothing changes: the host is told the movement only, for games.
 * ⓘ Not for DS5-USBIP's touchpad_to_mouse: there the PC makes the mouse, and
 * the TV never learns where its cursor went.
 * `dx`, `dy`: the finger's movement, in widths of the touchpad. True when it
 * was handled here; false, and upstream sends the movement as before. */
typedef struct stream_input_t stream_input_t;

bool touchpad_pointer_stream_move(stream_input_t *input, float dx, float dy);

#endif
