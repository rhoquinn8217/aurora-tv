/* Bridge Override: one switch that turns the TV's own mouse and touchpad
 * handling off.
 *
 * ⭐ HOW A PERSON REACHES IT (2026-10-01). It is switched ON by the question a
 * bridge raises when the TV's mouse controls are on
 * (ui/streaming/bridge_prompt.h), and switched OFF by turning Virtual Mouse on
 * from the overlay. It has no button of its own, and its name is never shown:
 * to a person it is "turn off Virtual Mouse". ⓘ It stood beside the USB
 * Bridge button until the DS5-USBIP button took that place. The control port
 * still sets it by name.
 *
 * ⭐ WHY IT EXISTS. A bridged controller reaches the host as itself, over
 * USB/IP, and the TV goes on READING it. The virtual mouse turns its sticks and
 * triggers into a host mouse, and the touchpad's mouse mode sends its click as
 * a host mouse click. Each of those is an Input setting of the upstream app,
 * and neither asks whether a pad is bridged.
 *
 * What it switches off for the stream: the virtual mouse, the touchpad's mouse
 * mode (the touchpad is sent as a touchpad), multi-touch gestures, natural
 * scrolling and battery reports to the host. ⓘ The last three never reach a
 * bridged pad anyway; they are included so that everything the override leaves
 * off is off for every pad.
 *
 * ⛔ NOT THE KEYBOARD (rhoquinn8217, 2026-09-30). It used to switch upstream
 * v1.3.0's USB keyboard grab off as well, because that grab held a keyboard's
 * node against the bridge and every key arrived twice. The grab now leaves a
 * bridged keyboard alone by itself (bridge_keyboard.h), so the override taking
 * it away only cost the keyboards that are NOT bridged upstream's handling.
 *
 * ⛔⛔ THE SAVED SETTINGS ARE NEVER WRITTEN. They are saved on leaving Settings
 * and on exit, so changing them would overwrite the person's own choices. The
 * override changes only the running stream's LIVE copy, the one
 * session_input_init() makes from the stream's config, and switching it off
 * puts that copy back from the same config. The Input menu keeps showing what
 * the person chose.
 *
 * ⛔⛔ THE OVERLAY CHORD IS NEVER TOUCHED. Everything here REMOVES an early exit
 * from the button handler (the virtual mouse's Triangle, the touchpad's mouse
 * click); nothing adds one ahead of the chord. */

#ifndef BRIDGE_OVERRIDE_H
#define BRIDGE_OVERRIDE_H

#include <stdbool.h>
#include <stddef.h>

typedef struct session_t session_t;
typedef struct stream_input_t stream_input_t;

/** Switched on AND device bridging enabled: the only state in which it acts. */
bool bridge_override_active(void);

/** Put the override's current state onto a stream's live input settings. */
void bridge_override_apply(stream_input_t *input);

/** Switch it, keep the choice, and apply it to the running stream if there is
 * one. The choice is kept like any other setting: in memory now, and in
 * moonlight.ini when the app next saves its settings. */
void bridge_override_set(session_t *session, bool on);

/** The Virtual Mouse button, pressed while the override is on: the press wins.
 * The override is switched off exactly as its own button would, and the virtual
 * mouse comes on, which is what the press asked for. */
void bridge_override_release_for_vmouse(session_t *session);

/** The live input state as one line, for the log and the control port. */
void bridge_override_describe(stream_input_t *input, char *buf, size_t len);

/* ⭐ THE TOUCHPAD FOLLOWS VIRTUAL MOUSE (rhoquinn8217, 2026-10-20): "I want
 * virtual Mouse turn on both stick to mouse and touch pad to mouse during a
 * stream (not just in the overlay)". While Virtual Mouse is on, a pad's
 * touchpad is the host's mouse too, whatever the Input setting says, through
 * upstream's own touchpad mouse mode; when it goes off, the touchpad is the
 * setting's again. The override still wins: with it on, the touchpad is sent
 * as a touchpad. ⓘ Only outside the overlay: while the overlay is up the
 * stream takes no input, and the touchpad is the TV's pointer
 * (input/touchpad_pointer.h).
 *
 * ⓘ Followed every pass rather than at each switch, so every way Virtual
 * Mouse is switched (the overlay's button, the control port, the override,
 * the stream's start) is followed with no hook in each. ⛔ The saved settings
 * are not written, as the override never writes them. */
void bridge_override_touchpad_tick(session_t *session);

/* ⭐ AND A BRIDGED PAD'S TOUCHPAD TOO (rhoquinn8217, 2026-10-20): "If virtual
 * mouse is on, I want the touchpad to act as a connected mouse through
 * aurora-tv just like the stick acts like a connected mouse through aurora-tv
 * using Virtual Mouse". A bridged pad is left out of what the stream sends,
 * but upstream's Virtual Mouse reads its sticks before that check, so they
 * move the host's mouse while it is bridged; the touchpad met the check first
 * and moved nothing. True when the touchpad is the host's mouse and Virtual
 * Mouse is on: the touchpad handler then lets a bridged pad's fingers through
 * to the mouse, and only to the mouse (its touches as a touchpad stay the
 * bridge's). ⓘ As with the sticks, if the host also makes that pad's
 * touchpad a mouse (Steam, or DS5-USBIP's touchpad_to_mouse), the cursor
 * moves twice: turning Virtual Mouse off is the answer to both. */
bool bridge_override_vmouse_lends_touchpad(stream_input_t *input);

/* ⛔ TWO FINGERS PRESSED ARE THE BRIDGE GESTURE, NOT A RIGHT CLICK, while
 * Virtual Mouse lends the touchpad and the gesture is on. Upstream's mouse
 * mode makes a two-finger press the right button, so every bridge would
 * press the host's right button for the second the gesture is held, and let
 * it go after: a context menu at the cursor, each time. Virtual Mouse's own
 * right button is R2, and a press in the touchpad's lower-right corner is
 * still one. ⓘ A person who chose "Send as mouse" keeps upstream's two-finger
 * right click: that was their choice before this. */
bool bridge_override_two_finger_press_is_gesture(const stream_input_t *input);

#endif /* BRIDGE_OVERRIDE_H */
