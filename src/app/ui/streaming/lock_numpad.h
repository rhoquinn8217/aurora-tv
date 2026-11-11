/* ⭐ WHAT A CONTROLLER DOES OVER A LOCKED WINDOWS, ON THE SCREEN
 * (stream/input/lock_keys.h). rhoquinn8217, 2026-11-01: "List all buttons including
 * the touchpad and right stick bindings", for the numpad closed, which is how a lock
 * starts; and "showing the numpad with the symbols that switch base on the controller
 * you are using like the DS5-USBIP button symbols", for it open.
 *
 * The list at the top left, the numpad to the right of Windows' login prompt, off the
 * prompt either way (their photo of the lock screen, 2026-11-01: "place it to the right
 * of the login prompt"), each button in its mark (ui/common/pad_marks.h), up for as long
 * as Windows is locked. It asks for no choice and it is not timed: the four seconds of
 * every other message do not fit something read while using it. ⓘ It takes no input, on
 * the system layer as the notices are. It goes while the overlay or the keyboard has
 * the input, and comes back as they close, as it was.
 *
 * ⭐ AND ONE LINE UNDER THE LIST, UP THE SAME WAY, while something ticked waits for the
 * unlock (rhoquinn8217, 2026-11-01: "Since we now have a word that can be read from
 * DS5-USBIP, replace "Auto Bridge is waiting for Windows to be unlocked" to "Devices will
 * be auto bridged after sign in."  Leave this up like the login controller legend and the
 * numpad until the user logs in"). Under the list and not in the notices' corner: that
 * corner is where a sign-in screen lists its other users.
 *
 * ⭐ AT A USER ACCOUNT CONTROL PROMPT (2026-11-09), the same list headed "User Account
 * Control: Controller", without the numpad's row, without the keyboard's unless the
 * prompt can ask for a password or a PIN, and without the line about Auto Bridge. */

#pragma once

#include <stdbool.h>

#include "lvgl.h"

/* Once a pass of the app's loop. */
void lock_numpad_tick(void);

/* What it shows, for the control port: "off", "open" (the numpad) or "closed" (the
 * list of what the buttons do). */
const char *lock_numpad_state(void);

/* Is the line about Auto Bridge up? For the control port. */
bool lock_numpad_after_up(void);

/* Is the Close button up (top right, with the list or the numpad)? For the control
 * port. */
bool lock_numpad_close_up(void);

/* Is this point of the screen on Close? For the touchpad's click in a stream,
 * which only Aurora's own pointer can bring to Close (input/touchpad_pointer.h). */
bool lock_numpad_close_at(lv_coord_t x, lv_coord_t y);

/* A mouse's button went down or up at this point of the screen, in the interface's
 * own frame, as the pointer's driver reads it (lvgl/input/lv_sdl_drv_pointer_input.c).
 * True: the press was on Close, so it and its release are Close's alone, and the
 * stream is not told (rhoquinn8217, 2026-11-10: a mouse's click on Close goes to Close
 * only). ⓘ The driver hands every button to the interface as a press, so every button
 * is taken on Close, not the left alone. */
bool lock_numpad_mouse_takes(int button, bool down, lv_coord_t x, lv_coord_t y);

/* Where the line about Auto Bridge is, its top, or -1 while it is not up. For the
 * control port. */
lv_coord_t lock_numpad_after_at(void);

/* What a click on Close does: these controls off until the next lock or prompt
 * (lock_keys_dismiss). The control port's `numpad close` as well. */
void lock_numpad_close_click(void);

/* Where it is, on the screen. False when nothing is up. */
bool lock_numpad_area(lv_area_t *out);
