/* The add-a-computer window's box opens holding the start of the TV's own
 * network, and has a button that empties it.
 *
 * ⭐ WHY (rhoquinn8217, 2026-10-23): "when adding a new server IP, auto fill in
 * "192.168.1."", taken from the TV's own address, "since that will most likely
 * the case. However this will not be the case for remote TV like my c3 and c5.
 * Allow the user to clear it to add a custom IP address." So the box opens
 * with the first three numbers of the TV's address and the cursor after them
 * ("192.168.1." on every set at home), and a round clear button at its right
 * end, shown while the box holds text, empties it in one click for an address
 * elsewhere. Backspace still takes one character back.
 *
 * ⓘ libcommons' hostport_parse takes "192.168.1." for a host name (a trailing
 * dot ends a name), and an empty box for an empty one, so upstream's check
 * alone would light OK before the last number is typed, or after a clear: OK
 * stays off while the box is empty or ends with a dot.
 *
 * ⭐ A CONTROLLER TYPES ON A NUMBER PAD (rhoquinn8217, 2026-11-06: "d-pad should
 * be able to reach the (x), and when inputing the IP address, the controller
 * should be able to select keys and d-pad and the cross/A button"; their "yes" to
 * the picture, then "add a button that brings up the the aurora-tv virtual
 * keyboard rather than the webOS one"). webOS's own keyboard, which the box's
 * focus opens, answers the remote alone: the controller's d-pad or Cross moved
 * the focus off the box and webOS closed it, and nothing took the × for a pointer
 * only. So a window opened with a controller keeps the box out of its focus
 * order and shows its own pad under the box: 1 to 9, 0, a dot, Bksp, and a
 * Keyboard key a row of its own, which opens aurora-tv's own keyboard
 * (ui/common/keyboard_text.h) for a name. It opens with 1 selected. The d-pad
 * walks the keys and Cross types; Up from the top row is the ×, Down from the
 * Keyboard key is Cancel and OK. Square is Bksp and Circle Cancel, as the line
 * under the pad says. A remote, a mouse or a keyboard find the window as before,
 * and a click on the box still opens webOS's keyboard. */

#pragma once

#include "lvgl.h"

/* For upstream's window (add.dialog.c), once it has made its box and buttons
 * and set them up, OK being button `ok` of `buttons`. */
void add_server_box(lv_obj_t *box, lv_obj_t *buttons, uint16_t ok);
