/* ⭐⭐ WHILE WINDOWS IS LOCKED, A CONTROLLER UNLOCKS IT (rhoquinn8217, 2026-11-01: "I
 * want to be able to use only a controller when streaming. no keyboard, no mouse
 * and prefereable no remote").
 *
 * WHY THE TV DOES IT, RATHER THAN LEAVING IT TO WINDOWS. At a lock reached by
 * quitting a game and starting a new stream Windows ignored the pad, the stream's
 * controller and a bridged one alike. And its PIN box did not open for keys either:
 * "Password/pin prompt won't open with any input except a mouse click or dragging
 * the screen up with the mouse". A mouse and a keyboard sent through the stream do
 * reach that screen. So the TV makes them out of the pad's buttons, in two states.
 *
 * ⭐ THE NUMPAD CLOSED, HOW A LOCK STARTS: the controller is a mouse. Virtual Mouse
 * as GuiDev1994 made it, turned on for the lock (their word: "This matches gui's
 * original button rebindings which is intentional"): the right stick moves the
 * mouse, L2 is the left button and R2 the right, held as long as they are, so a drag
 * works; the touchpad is the mouse, a one-finger tap a left click and a two-finger
 * tap a right click; Triangle opens the on-screen keyboard. Beside his, as keys,
 * since the lock screen often ignores a controller: Cross is Enter, Circle is Esc,
 * the d-pad the arrow keys. Square opens the numpad.
 *
 * ⭐ THE NUMPAD OPEN: the buttons type (their word: "When it is open, that's when
 * the numpad works"). On Windows' own map, so one layout holds at every lock.
 * Microsoft publishes no table; it is read off the keys in the picture of its
 * announcement (Build 26200.5641), and a community project lists the same digits:
 *   1 up, 2 left, 3 down, 4 right, 5 L2, 6 R2, 7 L1, 8 R1, 9 Triangle, 0 Square.
 * Cross is Enter and Create is Backspace. Circle closes it. By where each button
 * sits, so an Xbox pad's Y types what a DualSense's Triangle does. The screen shows
 * either state: ui/streaming/lock_numpad.h.
 *
 * WHEN. Only DS5-USBIP can see the lock, and it says so in its answer to the TV's
 * probe. So nothing of this happens at the first sign-in after a restart, where
 * DS5-USBIP has not started yet. A bridged pad is not read here at all: its reports
 * go straight to Windows.
 *
 * ⛔ WHAT IS TYPED IS NEVER LOGGED, and there is no way to read it back: only a
 * count, on the control port.
 *
 * ⭐ A KEY IS A TAP, down and up together, never held: a key left down on someone's
 * PC is the failure to design against, and the input can leave this file at any
 * moment for the overlay or the keyboard.
 *
 * ⭐ NOTHING REACHES THE HOST AS A CONTROLLER MEANWHILE (bridge_switch_hold_send), or
 * a Windows that does see the pad would type each digit twice. It is told the pad is
 * at rest as this begins; what is held as it ends stays out of the stream until let
 * go, as at a bridge's handback.
 *
 * ⓘ Virtual Mouse goes back at the unlock to how it was, and the Input setting does
 * not follow it meanwhile (bridge_override_touchpad_tick). */

#pragma once

#include <stdbool.h>

#include <SDL.h>

typedef struct app_gamepad_state_t app_gamepad_state_t;
typedef struct stream_input_t stream_input_t;
typedef struct app_t app_t;

/* Once a pass of the app's loop: starts and ends it, and asks DS5-USBIP again twice
 * a second while Windows is locked, so that the pad is the stream's again about a
 * second after the unlock. */
void lock_keys_tick(app_t *app);

/* Is Windows locked, and the controller doing this? */
bool lock_keys_active(void);

/* Is the numpad open? A lock starts with it closed. */
bool lock_keys_numpad_open(void);

/* A button of a pad on the stream went down or up. True: it was taken here, and the
 * stream's handling of it does not run. */
bool lock_keys_button(stream_input_t *input, app_gamepad_state_t *gamepad, const SDL_ControllerButtonEvent *event);

/* A stick or trigger moved: the triggers click, or with the numpad open, type. The
 * stream's handling goes on either way, for Virtual Mouse's sticks. */
void lock_keys_axis(stream_input_t *input, app_gamepad_state_t *gamepad, const SDL_ControllerAxisEvent *event);

/* Are the triggers typing? Virtual Mouse then sees them at rest. */
bool lock_keys_triggers_type(void);

/* Is a pad connected that can do this? The screen is about nothing without one. */
bool lock_keys_pad_on_stream(void);

/* Keys typed since the app started: a count, for the control port. */
unsigned lock_keys_typed(void);
