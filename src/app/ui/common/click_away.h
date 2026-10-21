/* A window closes when the screen around it is clicked.
 *
 * ⭐ WHY (rhoquinn8217, 2026-10-20): "after opening the server menu or the
 * streaming action menu in the aurora-tv dashboard, clicking away should close
 * both those menus. same with new server and help". Each is a message box,
 * which LVGL puts on a backdrop over the whole screen; a click that lands on
 * the backdrop itself, and not on the box, closes the box, as its own Close
 * does. A click inside the box is the box's.
 *
 * ⓘ With any pointer: the TV's remote, a mouse, or a pad's touchpad
 * (input/touchpad_pointer.h). */

#ifndef CLICK_AWAY_H
#define CLICK_AWAY_H

#include "lvgl.h"

/* For a message box made with no parent of its own (on LVGL's backdrop). */
void click_away_closes(lv_obj_t *msgbox);

#endif /* CLICK_AWAY_H */
