/* A bridged keyboard is the bridge's, and the TV's own keyboard grab leaves it
 * alone.
 *
 * ⭐ WHY IT EXISTS. Upstream v1.3.0 takes every USB keyboard for the stream
 * (keyboard_evdev.c: an EVIOCGRAB on its input node, read by its own thread and
 * sent to the host as key events). A keyboard that is bridged reaches the host
 * as itself over USB/IP, and the bridge core grabs the same node so the TV stops
 * reading it. Two grabs cannot share a node, and whichever is second is refused:
 *
 * - the TV's grab came first (it starts with the stream): the core's is refused
 *   and never retried, the TV goes on sending the keys, and every key arrives
 *   twice;
 * - the core's grab came first, and then the overlay opened: the core lets the
 *   keyboard go while the overlay is up, so the TV can read it there. If the
 *   TV's grab is started in that moment (Bridge Override going off does it) it
 *   takes the keyboard, the core cannot take it back, and every key arrives
 *   twice again.
 *
 * Both were seen on the C1 on 2026-09-30 (rhoquinn8217: "13. still double
 * types", typed double), and both are in the core's log as `grab refused ...
 * errno=16` and `taken back from the TV (0 grab(s) taken again)`.
 *
 * ➡️ So: the TV's grab SKIPS a keyboard that is bridged, by asking rather than
 * by losing a race; it LETS GO of every keyboard just before anything is
 * bridged; and it LOOKS AGAIN a moment after anything is bridged or released,
 * taking back the keyboards that are not bridged. Bridge Override no longer has
 * to be on for a bridged keyboard to type once. */

#ifndef BRIDGE_KEYBOARD_H
#define BRIDGE_KEYBOARD_H

#include <stdbool.h>

/* Is this input node (/dev/input/eventN) part of a device that is bridged right
 * now? Asked by the TV's keyboard grab for each keyboard it is about to take. */
bool bridge_keyboard_node_is_bridged(const char *event_path);

/* Something is about to be bridged: the TV's keyboard grab lets go of every
 * keyboard, so the bridge core's own grab is not refused. It looks again by
 * itself afterwards (bridge_keyboard_changed). */
void bridge_keyboard_before_plug(void);

/* Something was bridged or released: the TV's keyboard grab looks again
 * shortly, once the last change has settled. */
void bridge_keyboard_changed(void);

/* Once per pass of the app's loop: does the look-again when it is due. */
void bridge_keyboard_tick(void);

#endif /* BRIDGE_KEYBOARD_H */
