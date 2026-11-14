/* ⭐⭐ PRESS AND DRAG (rhoquinn8217, 2026-11-13: "aurora-tv does not have
 * touchpad_press_touch_drag", then "1. yes" to giving it to the controls the sign-in, a
 * User Account Control prompt and Desktop Mode share, stream/input/lock_keys.h). The left
 * button a touchpad press puts down stays down until no finger is left on the pad, not
 * until the click comes back up, as DS5-USBIP's default_windows_config does it
 * (touchpad_press_touch_drag = MouseLeft), so the switch from one to the other cannot be
 * felt. A finger slides on after the click is let go and the drag goes on, which
 * trackpads call drag lock.
 *
 * Only while those controls are on: the stream's own touchpad is as it was otherwise. The
 * hooks are in stream/input/session_gamepad.c, upstream's file, one line each; nothing
 * here touches its state. ⛔ The button is never left down on the PC: the last finger
 * lifted lets it go, and so does the touchpad's state being reset as the stream's input
 * ends. */

#pragma once

#include <stdbool.h>
#include <stdint.h>

/* The touchpad's click went down or up as the left button, on the touchpad state
 * `state`, with `fingers` the fingers on it now. True: send nothing. Down: the button is
 * down already, kept from the click before. Up: it is kept down for the drag. */
bool touchpad_drag_lock_click(const void *state, bool down, uint8_t fingers);

/* A finger lifted. With none left and the button kept down, it goes up now, sent here. */
void touchpad_drag_lock_finger_up(const void *state, uint8_t fingers);

/* The touchpad's state was reset: a button kept down goes up, sent here. */
void touchpad_drag_lock_drop(const void *state);
