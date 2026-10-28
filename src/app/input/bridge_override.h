/* The TV's own mouse controls while controllers are bridged: Virtual Mouse,
 * and the touchpad as the host's mouse.
 *
 * ⭐ WHY THIS EXISTS. A bridged controller reaches the host as itself, over
 * USB/IP, and the TV goes on READING it. The virtual mouse turns its sticks and
 * triggers into a host mouse, and the touchpad's mouse mode sends its click as
 * a host mouse click. Each of those is an Input setting of the upstream app,
 * and neither asks whether a pad is bridged. So a bridge asks the person
 * (ui/streaming/bridge_prompt.h).
 *
 * ⭐⭐ NOTHING IS REMEMBERED BEHIND THE PERSON'S BACK (rhoquinn8217,
 * 2026-10-28). Until then the pop-up's first button saved a hidden switch,
 * "Bridge Override", that turned the TV's mouse controls off in every stream
 * after, while Settings > Input went on showing them on: "I don't want it to
 * remember forever because it make you believe mouse mode doesn't exist or
 * will make people forget that mouse mode is a thing ... The stream will only
 * remember if mouse mode was on or off when it was closed. So if it was off,
 * not message. If it was on, then always a message, not assumed and
 * automatically turned off or on." So now:
 *   - Virtual Mouse is kept as it was left: whatever it is when a stream
 *     closes is the Input setting the next stream starts from;
 *   - the pop-up's first button turns Virtual Mouse off, and the touchpad's
 *     mouse mode off for that one stream; the touchpad follows Settings again
 *     in the next;
 *   - nothing else is touched: multi-touch, natural scrolling and battery
 *     reports stay as Settings has them.
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

/** The pop-up's first button: Virtual Mouse off (kept so by the tick below,
 * for the next stream too), and the touchpad's mouse mode off for the rest of
 * this stream. */
void bridge_override_mouse_off(session_t *session);

/** A stream starts: the touchpad's mouse mode is Settings' again. */
void bridge_override_stream_started(void);

/** The live input state as one line, for the log and the control port. */
void bridge_override_describe(stream_input_t *input, char *buf, size_t len);

/* ⭐ THE TOUCHPAD FOLLOWS VIRTUAL MOUSE (rhoquinn8217, 2026-10-20): "I want
 * virtual Mouse turn on both stick to mouse and touch pad to mouse during a
 * stream (not just in the overlay)". While Virtual Mouse is on, a pad's
 * touchpad is the host's mouse too, whatever the Input setting says, through
 * upstream's own touchpad mouse mode; when it goes off, the touchpad is the
 * setting's again, unless the pop-up's button turned it off for this stream.
 * ⓘ Only outside the overlay: while the overlay is up the stream takes no
 * input, and the touchpad is the TV's pointer (input/touchpad_pointer.h).
 *
 * ⭐ AND VIRTUAL MOUSE IS REMEMBERED HERE: when the stream's Virtual Mouse
 * differs from the Input setting, the setting takes its value, so the next
 * stream starts as this one was left (saved with the other settings).
 *
 * ⓘ Followed every pass rather than at each switch, so every way Virtual
 * Mouse is switched (the overlay's button, the control port, the pop-up, the
 * stream's start) is followed with no hook in each. */
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
