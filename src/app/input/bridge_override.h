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
 * off is off for every pad, and the notice that lists them is true.
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

#endif /* BRIDGE_OVERRIDE_H */
