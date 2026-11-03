/* A webOS pop-up over the app, answered from a controller as from the TV's
 * remote.
 *
 * ⭐ WHY (rhoquinn8217, 2026-11-03): "When webOS has a pop up, like the one where
 * it says that you can't pair anymore devices because you already have 4, the
 * controller buttons don't reach that pop up box on the C1. ... I want the
 * controller to be able to answer webOS pop ups."
 *
 * ⓘ What happened before: a webOS pop-up takes the focus from the app's window
 * (the C1, 2026-11-02 23:16, the Bluetooth pairing's, over the overlay's
 * panel), and SDL drops a controller's presses while no window of the app has
 * the focus. webOS's pop-ups answer the remote only, so a controller did
 * nothing at all.
 *
 * ➡️ So while the app's window is shown and webOS has the focus, SDL is let
 * deliver the controllers' input (its background-events hint, on for exactly
 * that time) and this file turns a press into the remote's: the d-pad its
 * arrows (and the left stick, as Aurora's own screens take it), Cross (A) its
 * OK, Circle (B) its Back. The key is written into the
 * remote's own input node ("LGE RCU" on the C1), which webOS reads as the
 * remote pressed, as mouse_seen.c wakes a pointer. Nothing else a controller
 * does reaches the app or the game meanwhile, as before: a press goes no
 * further, and what SDL let through by itself goes on (a button let go, a
 * finger lifted, a stick or trigger back at rest). Aurora's touchpad pointer
 * and the stick's volume stand aside too (bridge_app.c): they read the pads
 * themselves.
 * ⓘ The keys are LG's own on-screen remote's (webOS's
 * /usr/lib/qml/ScreenRemote/KeyCodeModel.qml, read on the C1): OK is Enter, 28,
 * and Back is 412, linux's KEY_PREVIOUS; the Magic Remote's nodes list both.
 * Each press is the key going down and up at once: a key left down in webOS's
 * view, when the pop-up closes on the press, would repeat.
 * ⭐ A KEYBOARD ANSWERS TOO (rhoquinn8217, the same day: "can you route mouse
 * and keyboard to remote inputs the same way?"). One the TV holds in a stream
 * is upstream's, grabbed and read on its own thread, and its keys went to the
 * PC, or to the overlay behind the pop-up. While a pop-up has the focus its
 * arrows, Enter, and Esc or Backspace (as Back) are the remote's keys, and no
 * key goes down anywhere else (input/bridge_keyboard.c asks here first). With
 * no stream webOS reads a keyboard itself, and a bridged one the core has let
 * go for the overlay too: those it answers by itself.
 * ⓘ A mouse is a pointer, and webOS points with its own: one the TV holds is
 * never taken from it.
 * ⭐ A BRIDGED DEVICE ANSWERS TOO (the same day, asked whether it should: "yes,
 * do it now"). Its reports go to the PC and SDL never sees them, so while a
 * pop-up is up the bridge holds them as it does for the overlay (bridge_app.c):
 * a bridged keyboard and mouse go back to webOS, which answers with them
 * itself, and a bridged pad's d-pad, Cross and Circle and its left stick are
 * read by the core from the pad's own reports (its held_nav.inl) and sent on
 * here. The game sees nothing pressed from them meanwhile, as with the overlay.
 * ⓘ In the background (the window hidden or minimized: another app in front)
 * nothing changes, and SDL drops the presses as it always did. Logged in
 * logs/webos-popup.log. App's loop only, but for webos_popup_take_key. */

#ifndef WEBOS_POPUP_H
#define WEBOS_POPUP_H

#include <stdbool.h>
#include <stddef.h>

#include <SDL.h>

/* Once the window is up: the pop-ups over it are watched from here on. */
void webos_popup_init(SDL_Window *window);

/* Is a webOS pop-up in front of the app: its window shown, and the focus gone? */
bool webos_popup_up(void);

/* In the app's event filter, before anything else sees `event`: true when it is
 * a controller's and must go no further, a press made while the app has no
 * focus. A pop-up's press is kept, to be sent on as the remote's by the tick,
 * outside SDL's walk of its queue. */
bool webos_popup_take(const SDL_Event *event);

/* Once per pass of the app's loop, after the filter: the keys kept sent on, and
 * SDL's hint and the log as a pop-up comes and goes. */
void webos_popup_tick(void);

/* A keyboard's key, from whichever thread reads it: true when a webOS pop-up
 * has the focus and the key goes down, and so is the pop-up's. Its arrows,
 * Enter, and Esc or Backspace are kept to be sent on as the remote's by the
 * tick; any other goes nowhere. A key let go is not taken: it goes on as
 * before. `vk` is Windows' key code, as the stream's keyboard reader has it. */
bool webos_popup_take_key(short vk, bool down);

/* For the control port: a remote key by its name (up, down, left, right, ok,
 * back) or by its number, sent only while a pop-up has the focus, so it can
 * never reach the app. `said` gets the line to answer with. */
bool webos_popup_send(const char *key, char *said, size_t len);

/* For the control port: the state, one line without its end. */
void webos_popup_describe(char *out, size_t len);

/* For the control port: `test` has the app raise a webOS pop-up of its own,
 * with OK and Cancel, to answer by hand or with `remote`; `close` closes the
 * last one it raised. webOS answers in its own time: the answer goes to the log.
 * ⓘ A terminal on the TV cannot raise one: luna-send-pub's call to the
 * notification service gets no answer (the C1, 2026-11-03). */
bool webos_popup_test(const char *what, char *said, size_t len);

#endif /* WEBOS_POPUP_H */
