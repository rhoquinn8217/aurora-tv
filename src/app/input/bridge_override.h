/* Bridge Override: one switch, beside the USB Bridge button in the streaming
 * overlay, that turns the TV's own mouse, touchpad and keyboard handling off.
 *
 * ⭐ WHY IT EXISTS. A bridged controller reaches the host as itself, over
 * USB/IP, and the TV goes on READING it. The virtual mouse turns its sticks and
 * triggers into a host mouse, the touchpad's mouse mode sends its click as a
 * host mouse click, and the USB keyboard grab (upstream v1.3.0) holds the
 * keyboard's node, which a bridge wants for itself. Each of those is an Input
 * setting of the upstream app, and none of them asks whether a pad is bridged.
 *
 * What it switches off for the stream: the virtual mouse, the touchpad's mouse
 * mode (the touchpad is sent as a touchpad), multi-touch gestures, natural
 * scrolling, battery reports to the host, and the USB keyboard grab. ⓘ The
 * middle three never reach a bridged pad anyway; they are included so that
 * everything the override leaves off is off for every pad, and the notice that
 * lists them is true.
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
