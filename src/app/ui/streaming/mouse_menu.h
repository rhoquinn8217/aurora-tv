/* The streaming overlay's Mouse button: a menu with the TV cursor on or off,
 * aligning it, and Virtual Mouse.
 *
 * ⭐ WHY (rhoquinn8217, 2026-10-11): "create a mouse button in the overlay that
 * opens a menu that has a button to toggle on/off the tv cursor, calibrate the
 * cursor or turn off the virtual mouse. I don't want to remove the virtual
 * mouse button in the overlay yet." The layout was shown first and approved
 * as it is here.
 *
 * - **The button** is the third in the overlay's row, right after Virtual
 *   Mouse, so it takes that button's place when that one goes. Teal, and not
 *   hidden with bridging off, as the bridge's two buttons are: nothing in it
 *   is the bridge's. bridge_overlay.c makes it with the row's other buttons of
 *   this fork's.
 * - **The menu** opens above the button and looks like the USB Bridge panel. A
 *   switch leaves it open, so the new state can be seen; Esc, B or a click
 *   outside it closes it, as the panel does.
 * - **TV cursor**, On or Off: input/tv_cursor.h says what each means, and why
 *   Off is what a game that turns with the mouse needs. It is saved, so the
 *   next stream starts the way this one was left.
 * - **Align TV cursor**: the overlay closes and a bar at the top of the screen
 *   takes the keys. The host's cursor first goes to the TV cursor's tip, so
 *   the two start together, and the mouse still moves both while clicks go
 *   nowhere. Each arrow or d-pad press moves the TV cursor one of the TV's own
 *   pixels; Enter or A keeps it on this TV, Esc or B puts back what was there.
 *   Greyed out, but still selectable, while the TV cursor is off.
 * - **Virtual Mouse**: the overlay's own switch, which also stays in the row.
 */

#ifndef MOUSE_MENU_H
#define MOUSE_MENU_H

#include <stdbool.h>

#include "lvgl.h"

#include "streaming.controller.h"

/* Gives the overlay's Mouse button its menu. The menu and the bar go with the
 * button, when the stream's screen goes. */
void mouse_menu_attach(lv_obj_t *button, streaming_controller_t *controller);

/* Is the bar that aligns the TV cursor up? While it is, it takes the keys as
 * the overlay does (ui/root.c) and a bridged device is held (bridge_app.c). */
bool mouse_menu_aligning(void);

#endif /* MOUSE_MENU_H */
