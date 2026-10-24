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
 * stays off while the box is empty or ends with a dot. */

#pragma once

#include "lvgl.h"

/* For upstream's window (add.dialog.c), once it has made its box and buttons
 * and set them up, OK being button `ok` of `buttons`. */
void add_server_box(lv_obj_t *box, lv_obj_t *buttons, uint16_t ok);
