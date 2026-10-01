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
 *   TV's grab is started in that moment (Bridge Override going off did it,
 *   while the override still switched the grab) it takes the keyboard, the core
 *   cannot take it back, and every key arrives twice again.
 *
 * Both were seen on the C1 on 2026-09-30 (rhoquinn8217: "13. still double
 * types", typed double), and both are in the core's log as `grab refused ...
 * errno=16` and `taken back from the TV (0 grab(s) taken again)`.
 *
 * ➡️ So: the TV's grab SKIPS a keyboard that is bridged, by asking rather than
 * by losing a race; it LETS GO of every keyboard just before anything is
 * bridged; and it LOOKS AGAIN a moment after anything is bridged or released,
 * taking back the keyboards that are not bridged. Bridge Override does not have
 * to be on for a bridged keyboard to type once, and it no longer touches the
 * keyboard at all (bridge_override.h).
 *
 * ⭐ AND AURORA'S SHORTCUTS WORK ON A KEYBOARD THE TV HAS (rhoquinn8217,
 * 2026-09-30: "it doesn't work unless the keyboard is bridged. It should work
 * all the time"). Upstream's grab sends a keyboard's keys straight to the host,
 * around stream_input_handle_key(), which is where the Ctrl+Alt+Shift
 * shortcuts were found; and it went on sending them with the overlay open. So
 * that path asks here first:
 *
 * - Ctrl+Alt+Shift+O opens the overlay (ours), and so does Ctrl+Alt+Shift+S
 *   (Moonlight's stats shortcut, which is what it does in this app);
 * - Ctrl+Alt+Shift+Q ends the stream (Moonlight's quit), as the overlay's
 *   Disconnect does;
 * - Moonlight's other five (Z, X, M, C, D) do nothing in this app beyond a log
 *   line, so they are left for the host.
 *
 * While the overlay is open the grab lets its keyboards go for the TV to read
 * and sends the host nothing, the way the bridge core does for a bridged
 * keyboard. ⓘ Each shortcut and each look-again is written to
 * logs/tv-keyboard.log, where it can be read on every set. */

#ifndef BRIDGE_KEYBOARD_H
#define BRIDGE_KEYBOARD_H

#include <stdbool.h>

typedef struct stream_input_t stream_input_t;

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

/* Once per pass of the app's loop: does the look-again when it is due, hands
 * the TV's keyboards to the overlay while it is open, and acts on a shortcut
 * the reader thread found. */
void bridge_keyboard_tick(bool overlay_shown);

/* A key from the TV's own keyboard grab, on its reader thread, before it is
 * sent to the host. True when it is not to be sent: it completed one of
 * Aurora's shortcuts, or the overlay is open and the keyboard is the TV's. */
bool bridge_keyboard_evdev_key(stream_input_t *input, short vk, bool down, char modifiers);

#endif /* BRIDGE_KEYBOARD_H */
