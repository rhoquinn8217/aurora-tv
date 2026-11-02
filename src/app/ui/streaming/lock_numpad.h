/* ⭐ THE NUMPAD ON THE SCREEN WHILE THE BUTTONS TYPE (stream/input/lock_keys.h).
 * rhoquinn8217, 2026-11-01, on the picture they said yes to: "showing the numpad
 * with the symbols that switch base on the controller you are using", and "have
 * the numpad stay up. Have the numpad start with the numbad on lock and circle
 * will hide it but a short message should stay up saying that circle brings it
 * back".
 *
 * So: a three by four numpad at the top left, off the middle of the picture where
 * Windows' box is, each key its digit and the mark of the button that types it
 * (ui/common/pad_marks.h), up for as long as Windows is locked. It asks for no
 * choice and it is not timed: the four seconds of every other message do not fit
 * something read while typing. ⓘ It takes no input, on the system layer as the
 * notices are. It goes while the overlay or the keyboard has the input, and comes
 * back as they close, with what was up before: the numpad or the message. */

#pragma once

#include <stdbool.h>

#include "lvgl.h"

/* Once a pass of the app's loop. */
void lock_numpad_tick(void);

/* What it shows, for the control port: "off", "shown" (the numpad) or "hidden"
 * (the message in its place). */
const char *lock_numpad_state(void);

/* Where it is, on the screen. False when nothing is up. */
bool lock_numpad_area(lv_area_t *out);
