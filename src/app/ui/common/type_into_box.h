/* A window's box fills from a keyboard while the box does not have focus.
 *
 * ⭐ WHY (rhoquinn8217, 2026-10-21): "When adding a new Server IP address,
 * allow keyboard input to fill out the box". What stood in the way, read on
 * the rooted monitor 2026-10-22: the add-a-computer window opens with focus on
 * its buttons, so the box's text input is off (lv_theme_moonlight.c turns it
 * on only while a box has focus, and the box has it for a moment as the window
 * is made), and SDL turns no key into text while it is off. A key that is not
 * a move, Enter, Esc or Backspace then reached nothing
 * (lvgl/input/lv_drv_sdl_key.c, read_keyboard). With focus on the box, text
 * input is on and webOS's own on-screen keyboard comes up over the screen.
 *
 * So while such a window is open and text input is off, a key that types one
 * of the box's characters goes into the box (Shift for capitals, ':' and
 * '_'), Backspace takes one back, and the window's OK is selected, so Enter
 * adds the computer. With text input on, SDL's text reaches the box as before
 * and nothing here acts. Focus stays where it is, so webOS's keyboard stays
 * away.
 *
 * ⓘ A keyboard webOS's compositor has not opened sends the app nothing at
 * all: the lab's virtual keyboards (ctm-vdev's, or one made through uinput)
 * reach only the stream's own keyboard grab. Its keys are tested through the
 * control port's `type`, which puts them where a keyboard's go. */

#ifndef TYPE_INTO_BOX_H
#define TYPE_INTO_BOX_H

#include <stdbool.h>
#include <stdint.h>

#include <SDL.h>

#include "lvgl.h"

/* `box` takes the keys while `window` is open; `buttons` is the window's row
 * of buttons, whose button `ok` is selected once something is typed. */
void type_into_box(lv_obj_t *window, lv_obj_t *box, lv_obj_t *buttons, uint16_t ok);

/* For every key the interface reads: true when it went into a box, and the
 * interface does nothing more with it. */
bool type_into_box_key(const SDL_KeyboardEvent *event);

#endif /* TYPE_INTO_BOX_H */
