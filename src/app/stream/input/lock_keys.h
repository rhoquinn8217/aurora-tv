/* ⭐⭐ WHILE WINDOWS IS LOCKED, A CONTROLLER'S BUTTONS TYPE (rhoquinn8217,
 * 2026-11-01: "I want to be able to use only a controller when streaming. no
 * keyboard, no mouse and prefereable no remote", and "buttons typle digits
 * directly").
 *
 * WHY THE TV TYPES, RATHER THAN LEAVING IT TO WINDOWS. Windows has a map of its
 * own for a PIN, and at a lock reached by quitting a game and starting a new
 * stream it ignored the pad, the stream's controller and a bridged one alike
 * (their hands, the same day). A keyboard's keys sent through the stream reach
 * that screen. So the TV turns the buttons into keys, and whether Windows sees a
 * controller stops mattering.
 *
 * ⭐ THE SAME MAP AS WINDOWS' OWN, so one layout holds at every lock. Microsoft
 * publishes no table; it is read off the keys in the picture of its announcement
 * (Build 26200.5641), and a community project lists the same digits:
 *   1 up, 2 left, 3 down, 4 right, 5 L2, 6 R2, 7 L1, 8 R1, 9 Triangle, 0 Square.
 * Cross is Enter and Create is Backspace, Options opens the on-screen keyboard
 * for a password, and Circle hides the numpad and brings it back
 * (ui/streaming/lock_numpad.h), by where each sits on the pad, so an Xbox pad's
 * Y types what a DualSense's Triangle does.
 *
 * WHEN. Only DS5-USBIP can see the lock, and it says so in its answer to the TV's
 * probe. So nothing of this happens at the first sign-in after a restart, where
 * DS5-USBIP has not started yet; Windows' own map and the overlay's Full keyboard
 * are there. A bridged pad is not read here at all: its reports go straight to
 * Windows.
 *
 * ⛔ WHAT IS TYPED IS NEVER LOGGED, and there is no way to read it back: only a
 * count, on the control port.
 *
 * ⭐ A KEY IS A TAP, down and up together, never held: a key left down on
 * someone's PC is the failure to design against, and the input can leave this
 * file at any moment for the overlay or the keyboard.
 *
 * ⭐ NOTHING REACHES THE HOST AS A CONTROLLER MEANWHILE (bridge_switch_hold_send),
 * or a Windows that does see the pad would type each digit twice. It is told the
 * pad is at rest as this begins; what is held as it ends stays out of the stream
 * until let go, as at a bridge's handback. */

#pragma once

#include <stdbool.h>

#include <SDL.h>

typedef struct app_gamepad_state_t app_gamepad_state_t;
typedef struct stream_input_t stream_input_t;
typedef struct app_t app_t;

/* Once a pass of the app's loop: starts and ends it, and asks DS5-USBIP again
 * twice a second while Windows is locked, so that the buttons stop typing about a
 * second after the unlock. */
void lock_keys_tick(app_t *app);

/* Are the buttons typing? */
bool lock_keys_active(void);

/* A button of a pad on the stream went down or up. True: it was taken here, and
 * the stream's handling of it does not run. */
bool lock_keys_button(stream_input_t *input, app_gamepad_state_t *gamepad, const SDL_ControllerButtonEvent *event);

/* A stick or trigger moved: the triggers type too. The stream's handling goes on,
 * for Virtual Mouse's sticks; bridge_switch_hold_axes sees the triggers at rest. */
void lock_keys_axis(stream_input_t *input, app_gamepad_state_t *gamepad, const SDL_ControllerAxisEvent *event);

/* Is a pad connected that can type? The numpad is about nothing without one. */
bool lock_keys_pad_on_stream(void);

/* Has Circle hidden the numpad? Each lock starts with it showing. */
bool lock_keys_numpad_hidden(void);

/* Keys typed since the app started: a count, for the control port. */
unsigned lock_keys_typed(void);
