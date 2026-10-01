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
 * keyboard.
 *
 * ⭐ AND THE GRAB FEEDS THE OVERLAY ITSELF (rhoquinn8217 on the C3, 2026-10-01:
 * "escape or any key doesn't work in the overlay screen"). Letting the
 * keyboards go only helps where webOS then hands their keys to the app. It did
 * on the C1 with a USB keyboard; on the C3 with a Bluetooth one nothing
 * arrived, in the overlay or from a keyboard the grab did not hold. So the
 * reader, which goes on reading while the overlay is open, turns the keys the
 * interface understands (the arrows, Enter, Escape, Tab, Backspace, Delete,
 * Home, End) into SDL key events of its own. Where webOS delivers the same key
 * as well, whichever copy comes second is dropped.
 *
 * ⭐ AND A KEYBOARD THAT CONNECTS MID-STREAM IS TAKEN (rhoquinn8217, the same
 * morning: "connecting a keyboard while the stream has already started doesn't
 * register"). The grab looks again whenever an input device arrives or goes.
 *
 * ⓘ What the grab holds, each shortcut, each look-again and who gave the
 * overlay its keys are written to logs/tv-keyboard.log, where they can be read
 * on every set. */

#ifndef BRIDGE_KEYBOARD_H
#define BRIDGE_KEYBOARD_H

#include <stdbool.h>
#include <stddef.h>

typedef struct stream_input_t stream_input_t;
struct SDL_KeyboardEvent;

/* Is this input node (/dev/input/eventN) part of a device that is bridged right
 * now? Asked by the TV's keyboard grab for each keyboard it is about to take. */
bool bridge_keyboard_node_is_bridged(const char *event_path);

/* The TV's keyboard grab has taken this node. For the record only. */
void bridge_keyboard_took(const char *event_path, const char *name);

/* Something is about to be bridged: the TV's keyboard grab lets go of every
 * keyboard, so the bridge core's own grab is not refused. It looks again by
 * itself afterwards (bridge_keyboard_changed). */
void bridge_keyboard_before_plug(void);

/* Something was bridged or released, or an input device arrived or went: the
 * TV's keyboard grab looks again shortly, once the last change has settled. */
void bridge_keyboard_changed(void);

/* Once per pass of the app's loop: watches for input devices arriving or going,
 * does the look-again when it is due, hands the TV's keyboards to the overlay
 * while it is open, and acts on a shortcut the reader thread found. */
void bridge_keyboard_tick(bool overlay_shown);

/* A key from the TV's own keyboard grab, on its reader thread, before it is
 * sent to the host. True when it is not to be sent: it completed one of
 * Aurora's shortcuts, or the overlay is open and the key is the overlay's. */
bool bridge_keyboard_evdev_key(stream_input_t *input, short vk, bool down, char modifiers);

/* A key event taken off SDL's queue by the interface, on the app's loop, before
 * anything else looks at it. What to do with it: */
enum {
    BRIDGE_KEYBOARD_KEY_PASS = 0,   /* nothing of ours: carry on as usual */
    BRIDGE_KEYBOARD_KEY_DROP,       /* the same key twice, or fed for an overlay that has closed */
    BRIDGE_KEYBOARD_KEY_RELEASE,    /* drop it, and the interface's key is no longer down */
};
int bridge_keyboard_sdl_key(const struct SDL_KeyboardEvent *event);

/* The counts as one line, for the control port: key events the grab's reader
 * saw and fed, and key events that came through SDL. */
void bridge_keyboard_counts(char *buf, size_t len);

#endif /* BRIDGE_KEYBOARD_H */
