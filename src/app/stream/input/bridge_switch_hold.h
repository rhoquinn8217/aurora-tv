#pragma once

#include <stdbool.h>

typedef struct app_gamepad_state_t app_gamepad_state_t;
typedef struct stream_input_t stream_input_t;

/* ⭐⭐ NOTHING HELD AS A PAD COMES BACK FROM A BRIDGE REACHES THE STREAM
 * (rhoquinn8217, 2026-10-09: "if you bridge while hold anything down should
 * automatically cancel every input. We don't want any lingering inputs being
 * held down"). A release switches controllers too: the bridged device leaves
 * the PC and the stream's controller arrives again. So what is held at that
 * moment -- a button, a stick, a trigger, a finger on the touchpad -- reaches
 * the stream released, and stays released until it is let go. The bridge's
 * side of the same rule is the core's switch_hold.inl.
 *
 * ⓘ Kept per Moonlight slot and the SDL instance that held it, so a pad
 * that leaves while something is held passes nothing on to the next one. */

/* The pad is coming back from a bridge: what it holds now is kept released.
 * Call before it is announced to the host again. ⓘ Read from the pad itself,
 * not from the stream's copy of it, which stands still while the overlay is
 * open; `input` gives the stream's dead zone and touchpad mode. */
void bridge_switch_hold_begin(const stream_input_t *input, const app_gamepad_state_t *gamepad);

/* Nothing held, at a stream's start and end. */
void bridge_switch_hold_reset(void);

/* A button went down or up: one held from the switch works again once up. */
void bridge_switch_hold_note_button(const app_gamepad_state_t *gamepad, int button_flag, bool down);

/* The sticks and triggers changed: one held from the switch works again once
 * back at rest. */
void bridge_switch_hold_note_axes(const app_gamepad_state_t *gamepad);

/* The pad's sticks and triggers as the stream may have them: held ones at rest. */
typedef struct {
    unsigned char lt, rt;
    short lx, ly, rx, ry;
} bridge_switch_axes_t;
bridge_switch_axes_t bridge_switch_hold_axes(const app_gamepad_state_t *gamepad);

/* Is this touch on a finger that was already down at the switch? Its events,
 * its lift included, then stay off the stream. */
bool bridge_switch_hold_skips_finger(const app_gamepad_state_t *gamepad, int finger, bool lifted);

/* Send the pad's state to the stream, held ones released. `buttons_only` for
 * the Virtual Mouse, which keeps the sticks and triggers for itself. */
void bridge_switch_hold_send(const app_gamepad_state_t *gamepad, short active_mask, bool buttons_only);
