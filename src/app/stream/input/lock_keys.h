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
 * as GuiDev1994 made it, turned on for the lock: the right stick moves the mouse; the
 * touchpad is the mouse, a one-finger tap a left click and a two-finger tap a right
 * click. The triggers are held as long as they are, so a drag works, and they are NOT
 * his: R2 is the left button and L2 the right, as DS5-USBIP's own configs have them
 * (their word, after a real lock: "Switch R2 and L2 mouse buttons to match ours"). His
 * stay his outside a lock. Beside his, as keys, since the lock screen often ignores a
 * controller: Cross is Enter, Circle is Esc, the d-pad the arrow keys, and Create Tab
 * (2026-11-10: "create create button binded to tab"), sent as Create goes up because it
 * is one of the overlay's chord. Square opens the on-screen keyboard and Triangle the
 * numpad ("Switch square and triangle functions"), the other way from his Triangle.
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
 * not follow it meanwhile (bridge_override_touchpad_tick).
 *
 * ⭐⭐ AND AT A USER ACCOUNT CONTROL PROMPT, EVERY CONTROLLER, BRIDGED OR NOT
 * (rhoquinn8217, 2026-11-09: "tell the TV, but override the controller inputs bridge
 * or not, similar to the lock screen", and "insure that the controller will be able
 * to answer any UAC prompt in any case including when a controller is not bridged and
 * or birdge but the controller has a config that doesn't have d-pad movement and x as
 * select or a mouse mode and click"). The same controls as the numpad closed, without
 * the numpad. Square opens this app's keyboard at every prompt, from a bridged pad as
 * well (2026-11-10: "lets just add square to all UAC prompts. there's no downside of
 * having it"; until then only where a prompt could ask for a password or a PIN), and
 * Create is Tab. DS5-USBIP says which kind is up in its answer to the probe. Its prompts sit on the same
 * secure desktop as the lock screen, which a mouse and keys through the stream reach.
 *   - A bridged pad goes on reaching the TV through SDL (input/bridge_override.h), so
 *     it is taken here as well; its reports to the PC are held at rest meanwhile, as
 *     while the overlay is open (lock_keys_holds_bridged), and stay held after the
 *     prompt until what was down is let go.
 *   - The stream's own pads are told they are at rest, as at the lock.
 *   - The control port can fake a prompt of either kind, to see this without one.
 *
 * ⭐⭐ AND IT CAN BE TURNED OFF BY HAND, a fail-safe (rhoquinn8217, 2026-11-10: "a
 * close button to right of screen", "not just a close the displayed legends but
 * disable the overrides as well so each controller controls like a plain
 * controller", "This is a fail safe option incase the rebinds happen
 * inappropriately. When someone closes it this way ... it needs to stay off until
 * the next time a login or UAC is detected"). A button "Close" at the right of the
 * list's heading, and the last row of the list says Options "- hold to close"
 * (ui/streaming/lock_numpad.c). A click on the button, as on the stats' pin, or Options
 * held 2 s on a pad this takes with nothing else pressed (4 s until 2026-11-10, as
 * Select pins the stats), asks first, and its OK ends this as an unlock or the
 * prompt's end would. It stays off until DS5-USBIP says neither, or says the other of
 * the two.
 * ⭐ THE QUESTION (2026-11-10: "On a close, open pop up with warning that rebindings
 * will be turned off and if they are sure ok or cancel"): while it is up, Cross is its
 * OK and Circle its Cancel (A and B on an Xbox pad), no other button does anything, and
 * the triggers do not click. The sticks and the touchpad go on as the mouse.
 * ⓘ While this is on, a bridge raises no Virtual Mouse pop-up
 * (ui/streaming/bridge_prompt.h): this turns Virtual Mouse on itself. */

#pragma once

#include <stdbool.h>

#include <SDL.h>

typedef struct app_gamepad_state_t app_gamepad_state_t;
typedef struct stream_input_t stream_input_t;
typedef struct app_t app_t;

/* Once a pass of the app's loop: starts and ends it, and asks DS5-USBIP again twice
 * a second while Windows is locked or a prompt is up, so that the pad is the stream's
 * again about a second after the unlock, or after the prompt is answered. */
void lock_keys_tick(app_t *app);

/* Is Windows locked, or a User Account Control prompt up, and the controller doing
 * this? */
bool lock_keys_active(void);

/* What the controls are up for. */
typedef enum {
    LOCK_KEYS_OFF = 0,
    LOCK_KEYS_LOCK,              /* Windows is locked */
    LOCK_KEYS_PROMPT,            /* a User Account Control prompt, Yes or No */
    LOCK_KEYS_PROMPT_PASSWORD,   /* one that can ask for a password or PIN */
} lock_keys_mode_t;

lock_keys_mode_t lock_keys_mode(void);

/* Are the bridged controllers' reports to be held at rest from the PC? While a
 * prompt is up, and after it until what was down on them is let go. */
bool lock_keys_holds_bridged(void);

/* For the control port: a prompt of this kind (0 none, 1 Yes or No, 2 a password or
 * PIN), as if DS5-USBIP had said so; -1 goes back to what it says. Not saved. */
void lock_keys_fake_prompt(int kind);

/* The kind faked, or -1. */
int lock_keys_faked_prompt(void);

/* Turned off by hand, with how in the log: off until the lock or the prompt is over,
 * and on again at the next one. Nothing while off. */
void lock_keys_dismiss(const char *how);

/* Turned off by hand, and the lock or prompt it was turned off at still there? */
bool lock_keys_dismissed(void);

/* A close: Close clicked, or Options held 2 s, with how in the log. It asks first
 * whether to turn this off (the header). Nothing while off, turned off already, or
 * asking already. */
void lock_keys_ask_dismiss(const char *how);

/* Is the question a close raises waiting for its answer? */
bool lock_keys_asking(void);

/* Its answer, with what gave it in the log: OK turns this off by hand
 * (lock_keys_dismiss), Cancel leaves it as it was. Nothing while no question waits. */
void lock_keys_answer(bool ok, const char *by);

/* Is the numpad open? A lock starts with it closed. */
bool lock_keys_numpad_open(void);

/* A button of a pad on the stream went down or up. True: it was taken here, and the
 * stream's handling of it does not run. */
bool lock_keys_button(stream_input_t *input, app_gamepad_state_t *gamepad, const SDL_ControllerButtonEvent *event);

/* A stick or trigger moved: the triggers click, or with the numpad open, type. The
 * stream's handling goes on either way, for Virtual Mouse's sticks. */
void lock_keys_axis(stream_input_t *input, app_gamepad_state_t *gamepad, const SDL_ControllerAxisEvent *event);

/* Are the triggers kept from the mouse: typing, with the numpad open, or doing
 * nothing while the question a close raises waits? Virtual Mouse then sees them at
 * rest. */
bool lock_keys_triggers_type(void);

/* Are the triggers the mouse's buttons, R2 the left and L2 the right? Virtual Mouse
 * then sees them the other way about from its own. */
bool lock_keys_triggers_swapped(void);

/* Is a pad connected that can do this? The screen is about nothing without one. At a
 * prompt any pad counts, bridged ones too. */
bool lock_keys_pad_on_stream(void);

/* Keys typed since the app started: a count, for the control port. */
unsigned lock_keys_typed(void);
