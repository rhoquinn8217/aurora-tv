/* Which mice the TV has seen move, so a mouse in use can be told from a mouse
 * part with nothing behind it.
 *
 * ⭐ WHY: a part that says it is a mouse is not always one. The RONGYUAN
 * keyboard's receiver has a mouse part with no mouse behind it, and a keyboard
 * bridged with one must not count as a bridged mouse (mouse_seen_mouse_bridged).
 * And webOS's own pointer can be moved by the app only once a mouse has moved
 * since the app started (mouse_seen_any), and is woken through the mice seen
 * moving (mouse_seen_wake).
 * ⓘ Until 2026-11-12 a bridged mouse also raised a pop-up about the TV cursor,
 * which rhoquinn8217 had removed with every message about the cursor.
 *
 * ➡️ So the app watches every pointer the TV has, without taking any, and
 * remembers which have moved.
 *
 * ⓘ Each node is read only until it first moves, then closed: nothing is read
 * from a mouse in use. What moved is forgotten when the input devices change,
 * as a node's number can pass to another device. App's loop only. */

#ifndef MOUSE_SEEN_H
#define MOUSE_SEEN_H

#include <stdbool.h>

#include "ctm_bridge_glue.h"

/* Once per pass of the app's loop: watch the pointers, a short burst of reads
 * each. */
void mouse_seen_tick(void);

/* ⭐ WEBOS'S POINTER WOKEN, for the bar that aligns the TV cursor
 * (ui/streaming/cursor_align.h). webOS hides a mouse's pointer after 5 seconds
 * without movement, and keys never count; so a movement of one pixel right and
 * one back is written into each mouse the TV has seen move, through the
 * mouse's own input node, which to webOS is that mouse moving.
 * ⓘ Only mice already seen moving: one still watched would take the movement
 * for its own and count as seen moving. How many were woken. */
int mouse_seen_wake(void);

/* A test: webOS's pointer moved by `dx`, `dy` counts as the first mouse the TV saw
 * move, through its node (the control port's `cursor nudge`). 1 when a node took it. */
int mouse_seen_nudge(int dx, int dy);

/* Has the TV seen a mouse move since the app started? Then webOS has a pointer the
 * app can move (input/touchpad_pointer.h, webos_pointer_move). */
bool mouse_seen_any(void);

/* ⭐ IS A MOUSE BRIDGED? For the TV cursor, which does not come on while one
 * is (input/tv_cursor.h). A bridged mouse part counts when the bridge core
 * says it has been used since its bridge, or when the TV saw it move before:
 * a receiver's mouse part with nothing behind it does neither, so a keyboard
 * bridged with one does not count. `which` gets the part and why, for the
 * log. */
bool mouse_seen_mouse_bridged(char *which, size_t len);

#endif /* MOUSE_SEEN_H */
