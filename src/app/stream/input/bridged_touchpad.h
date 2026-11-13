/* ⭐⭐ A BRIDGED PAD'S TOUCHPAD IS THE PC'S MOUSE UNDER THE TV'S CONTROLS (rhoquinn8217,
 * 2026-11-13, "1. yes" to a bridged pad's touchpad working as the PC's mouse in Desktop
 * Mode the same as an unbridged one's: moving, taps, clicks, press and drag, with the
 * sign-in and a User Account Control prompt sharing it; stream/input/lock_keys.h).
 * Under those controls a bridged pad's reports are held from the PC, and the stream's
 * own touchpad handling (stream/input/session_gamepad.c) took only a pad on the stream's
 * touches. A bridged pad is read from the bridge core only (input/overlay_volume.h says
 * why), so the core's reading of its touchpad (ctm_bridge_held_touches; its
 * held_touch.inl: a DualSense or an Edge, on a cable, a dongle or Bluetooth, both
 * fingers) is handed to that handling as SDL hands it a pad on the stream's: each
 * finger's going down, moving and lifting, and the press. So it moves, taps, clicks,
 * scrolls and drags as that pad's does. The two-finger press that shows or hides the
 * list is read by lock_keys.c itself, and the handling makes no right click of it.
 *
 * ⓘ SDL's own copy of a bridged pad's click, where SDL still reads the pad, is not the
 * mouse's meanwhile (bridged_touchpad_owns): the click would come twice.
 * ⓘ Not while Virtual Mouse lends a bridged pad's touchpad (input/bridge_override.h),
 * which reads it through SDL: the mouse would move twice.
 * ⓘ A DS4's touch is not read by the core yet, and a pad the stream has no state for is
 * not handed anything: their touchpads stay as they were. */

#pragma once

#include <stdbool.h>

typedef struct app_gamepad_state_t app_gamepad_state_t;
typedef struct stream_input_t stream_input_t;

/* Once a pass of the app's loop: `on` while the controls are up and the stream has the
 * input. Off, whatever it put down on the PC is let go. */
void bridged_touchpad_tick(stream_input_t *input, bool on);

/* True while the stream's touchpad handling is being handed one of a bridged pad's
 * touches or clicks, which it does not take from a bridged pad otherwise. */
bool bridged_touchpad_feeding(void);

/* True for a bridged pad whose touchpad this hands to the stream now: SDL's own copy of
 * its click is not the mouse's then. */
bool bridged_touchpad_owns(const app_gamepad_state_t *gamepad);
