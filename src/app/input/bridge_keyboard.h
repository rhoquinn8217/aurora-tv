/* A bridged keyboard is the bridge's, and the TV's own keyboard grab never takes
 * it.
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
 *   keyboard go while the overlay is up. If the TV's grab takes it in that
 *   moment, the core cannot take it back, and every key arrives twice again.
 *
 * Both were seen on the C1 on 2026-09-30 (rhoquinn8217: "13. still double
 * types", typed double), and both are in the core's log as `grab refused ...
 * errno=16` and `taken back from the TV (0 grab(s) taken again)`.
 *
 * ➡️ So: the TV's grab NEVER TAKES a keyboard that is bridged, by asking rather
 * than by losing a race; it LETS GO of every keyboard just before anything is
 * bridged; and it LOOKS AGAIN a moment after anything is bridged or released,
 * taking the keyboards that are not bridged. Bridge Override does not have to be
 * on for a bridged keyboard to type once, and it no longer touches the keyboard
 * at all (bridge_override.h).
 *
 * ⭐ AURORA'S SHORTCUTS WORK ON A KEYBOARD THE TV HAS (rhoquinn8217, 2026-09-30:
 * "it doesn't work unless the keyboard is bridged. It should work all the
 * time"). Upstream's grab sends a keyboard's keys straight to the host, around
 * stream_input_handle_key(), which is where the Ctrl+Alt+Shift shortcuts were
 * found. So that path asks here first:
 *
 * - Ctrl+Alt+Shift+S opens the overlay: Moonlight's stats shortcut, which is
 *   what it has always done in this app. ⓘ O did the same from 2026-09-15,
 *   added in the belief that there was no shortcut, and was taken out again
 *   on 2026-10-01 (rhoquinn8217: "I added O because I thought we didn't have
 *   one. We can remove it"). A bridged keyboard's S is found by the bridge
 *   core in its reports;
 * - Ctrl+Alt+Shift+Q ends the stream (Moonlight's quit), as the overlay's
 *   Disconnect does;
 * - Moonlight's other five (Z, X, M, C, D) do nothing in this app beyond a log
 *   line, so they are left for the host.
 *
 * ⭐ THE READER GIVES THE OVERLAY ITS KEYS, FROM EVERY KEYBOARD (rhoquinn8217 on
 * the C3, 2026-10-01: "escape or any key doesn't work in the overlay screen",
 * and of a bridged keyboard: "results in no control and I have to rely on a
 * different input"). webOS cannot be relied on to hand a keyboard's keys to the
 * app: on the C3 it handed them over in one overlay all morning and never
 * again, the keyboard connected from before the app started included. What
 * decides it is not known. So, while the overlay is open:
 *
 * - a keyboard the grab holds STAYS held, and the reader turns the keys the
 *   interface understands (the arrows, Enter, Escape, Tab, Backspace, Delete,
 *   Home, End) into SDL key events of its own. Nothing goes to the host;
 * - a BRIDGED keyboard is read as well, without being taken, and not by
 *   upstream's reader: the app's loop opens the keyboard's own input nodes
 *   as the overlay opens and closes them as it closes. The bridge core lets
 *   the keyboard go for exactly that long and sends the host nothing, so
 *   the keys can be heard, and nothing read this way can reach the host.
 *   Only the nodes that can send one of those keys are opened: a bridged
 *   controller's are left alone.
 *
 * Where webOS delivers the same key as well (a bridged keyboard the core has let
 * go, on a set that hands it over), whichever copy is taken second is dropped:
 * each key taken from one source is owed one twin from the other. ⓘ Counted,
 * not timed to a short window: build 454 used 250 ms, and a copy held up by the
 * app's loop was taken as a second press.
 *
 * ⭐ A KEYBOARD THAT CONNECTS MID-STREAM IS TAKEN (rhoquinn8217, the same
 * morning: "connecting a keyboard while the stream has already started doesn't
 * register"). The grab looks again whenever a keyboard arrives or goes. Any
 * other device coming or going leaves it as it is: looking again leaves every
 * keyboard unread for about half a second.
 *
 * ⛔ NO KEY STAYS DOWN ON THE HOST BEHIND THE KEYBOARD'S BACK. The host repeats
 * a held key by itself, so a release it never hears is a key typing on its
 * own. Two places could lose one, and in both the host's keys come up first:
 * the grab letting go to look again, and the overlay opening with a key held
 * (it swallows every key from then on, the releases too).
 *
 * ⛔ A HANDLE ON A DEVICE THAT HAS GONE IS CLOSED AT ONCE. A set that keeps its
 * input nodes as permanent files (the C3 does) lets nobody open the device
 * that takes the same number next while one is still open: a keyboard that
 * dropped off bridged came back dead that way (2026-10-01). The bridge core
 * ends such a session itself, and the nodes the overlay reads are closed as
 * soon as a read says the device has gone.
 *
 * ⓘ What the grab holds and reads, each shortcut, each look-again and who gave
 * the overlay its keys are written to logs/tv-keyboard.log, where they can be
 * read on every set. */

#ifndef BRIDGE_KEYBOARD_H
#define BRIDGE_KEYBOARD_H

#include <stdbool.h>
#include <stddef.h>

typedef struct stream_input_t stream_input_t;
struct SDL_KeyboardEvent;

/* Is this input node (/dev/input/eventN) part of a device that is bridged right
 * now? Asked by the TV's keyboard grab for each keyboard it finds: a bridged
 * one is read without being taken. */
bool bridge_keyboard_node_is_bridged(const char *event_path);

/* The TV's keyboard grab has taken this node. For the record only. */
void bridge_keyboard_took(const char *event_path, const char *name);

/* Something is about to be bridged: the TV's keyboard grab lets go of every
 * keyboard, so the bridge core's own grab is not refused. It looks again by
 * itself afterwards (bridge_keyboard_changed). */
void bridge_keyboard_before_plug(void);

/* Something was bridged or released, or a keyboard arrived or went: the TV's
 * keyboard grab looks again shortly, once the last change has settled. */
void bridge_keyboard_changed(void);

/* Once per pass of the app's loop: watches for input devices arriving or going,
 * does the look-again when it is due, tells the reader whether the overlay is
 * open, and acts on a shortcut the reader thread found. */
void bridge_keyboard_tick(bool overlay_shown);

/* A key from the TV's own keyboard reader, on its thread, before it is sent to
 * the host. True when it is not to be sent: it completed one of Aurora's
 * shortcuts, the overlay is open and the key is the overlay's, or it came from
 * a bridged keyboard. */
bool bridge_keyboard_evdev_key(stream_input_t *input, short vk, bool down, char modifiers);

/* A key event taken off SDL's queue by the interface, on the app's loop, before
 * anything else looks at it. What to do with it: */
enum {
    BRIDGE_KEYBOARD_KEY_PASS = 0,   /* nothing of ours: carry on as usual */
    BRIDGE_KEYBOARD_KEY_DROP,       /* the same key twice, or fed for an overlay that has closed */
    BRIDGE_KEYBOARD_KEY_RELEASE,    /* drop it, and the interface's key is no longer down */
};
int bridge_keyboard_sdl_key(const struct SDL_KeyboardEvent *event);

/* The counts as one line, for the control port: key events the reader saw and
 * fed, and key events that came through SDL. */
void bridge_keyboard_counts(char *buf, size_t len);

#endif /* BRIDGE_KEYBOARD_H */
