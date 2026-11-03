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
 * ⚠️ A BRIDGED controller does not answer: its reports go to the PC and SDL
 * never sees them, so it plays on in the game as before.
 * ⓘ In the background (the window hidden or minimized: another app in front)
 * nothing changes, and SDL drops the presses as it always did. Logged in
 * logs/webos-popup.log. App's loop only. */

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

/* For the control port: a remote key by its name (up, down, left, right, ok,
 * back) or by its number, sent only while a pop-up has the focus, so it can
 * never reach the app. `said` gets the line to answer with. */
bool webos_popup_send(const char *key, char *said, size_t len);

/* For the control port: the state, one line without its end. */
void webos_popup_describe(char *out, size_t len);

#endif /* WEBOS_POPUP_H */
