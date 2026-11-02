/* ⭐ WHAT A CONTROLLER DOES OVER A LOCKED WINDOWS, ON THE SCREEN
 * (stream/input/lock_keys.h). rhoquinn8217, 2026-11-01: "List all buttons including
 * the touchpad and right stick bindings", for the numpad closed, which is how a lock
 * starts; and "showing the numpad with the symbols that switch base on the controller
 * you are using like the DS5-USBIP button symbols", for it open.
 *
 * Top left, off the middle of the picture where Windows' box is, each button in its
 * mark (ui/common/pad_marks.h), up for as long as Windows is locked. It asks for no
 * choice and it is not timed: the four seconds of every other message do not fit
 * something read while using it. ⓘ It takes no input, on the system layer as the
 * notices are. It goes while the overlay or the keyboard has the input, and comes
 * back as they close, as it was. */

#pragma once

#include <stdbool.h>

#include "lvgl.h"

/* Once a pass of the app's loop. */
void lock_numpad_tick(void);

/* What it shows, for the control port: "off", "open" (the numpad) or "closed" (the
 * list of what the buttons do). */
const char *lock_numpad_state(void);

/* Where it is, on the screen. False when nothing is up. */
bool lock_numpad_area(lv_area_t *out);
