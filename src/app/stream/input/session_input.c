/*
 * Copyright (c) 2023 Ningyuan Li <https://github.com/mariotaku>.
 * 
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 3 of the License, or (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 * 
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "session_input.h"
#include "session_pointer_gesture.h"
#include "stream/session.h"
#include "stream/session_priv.h"
#include "session_evmouse.h"
#include "input/app_input.h"

void session_input_init(stream_input_t *input, session_t *session, app_input_t *app_input,
                        const session_config_t *config) {
    input->session = session;
    input->input = app_input;
    input->announcedGamepadMask = 0;
    input->moonlightExcludedMask = 0;
    input->remoteOkPressed = false;
    input->remoteOkPressedAt = 0;
    input->remoteOkModifiers = 0;
    input->pointerGestureActive = false;
    input->pointerGestureDragging = false;
    input->pointerGestureLeftDown = false;
    input->pointerGesturePressTime = 0;
    input->pointerGestureStartX = 0;
    input->pointerGestureStartY = 0;
    input->view_only = config->view_only;
    input->no_host_gamepad = config->no_host_gamepad;
    input->stick_deadzone = config->stick_deadzone;
    input->no_sdl_mouse = config->hardware_mouse;
#if FEATURE_INPUT_EVMOUSE
    if (!config->view_only && config->hardware_mouse) {
        session_evmouse_init(&input->evmouse, session);
    }
#endif
}

void session_input_deinit(stream_input_t *input) {
#if FEATURE_INPUT_EVMOUSE
    const session_config_t *config = &input->session->config;
    if (!config->view_only && config->hardware_mouse) {
        session_evmouse_deinit(&input->evmouse);
    }
#endif
}

void session_input_interrupt(stream_input_t *input) {
    pointer_gesture_reset(input);
#if FEATURE_INPUT_EVMOUSE
    const session_config_t *config = &input->session->config;
    if (!config->view_only && config->hardware_mouse) {
        session_evmouse_interrupt(&input->evmouse);
    }
#endif
}

void session_input_started(stream_input_t *input) {
    input->started = true;
    if (input->view_only) {
        return;
    }
    /* Announce every controller EXCEPT ones already handed to the bridge.
     *
     * This returned early whenever the bridge was enabled, announcing nothing
     * at all -- the comment said that prevented a phantom pad on the host. It
     * does not. Measured on C1 2026-08-10: with nothing announced, both pads
     * still appeared in Windows the moment a stick moved, because ordinary
     * input events carry a mask the host builds pads from. The suppression
     * removed the announcement and not the pad, and left the pad impossible
     * to retire, since the remove path only acts on something announced. */
    for (int i = 0, j = app_input_get_max_gamepads(input->input); i < j; ++i) {
        app_gamepad_state_t *gamepad = app_input_gamepad_state_by_index(input->input, i);
        if (gamepad == NULL) {
            continue;
        }
        if (input->moonlightExcludedMask & (1u << gamepad->gs_id)) {
            continue;
        }
        stream_input_send_gamepad_arrive(input, gamepad);
    }
}

void session_input_stopped(stream_input_t *input) {
    pointer_gesture_reset(input);
    input->started = false;
    input->announcedGamepadMask = 0;
    input->moonlightExcludedMask = 0;
    input->remoteOkPressed = false;
    input->remoteOkPressedAt = 0;
    input->remoteOkModifiers = 0;
}

void session_input_screen_keyboard_opened(stream_input_t *input) {
#if FEATURE_INPUT_EVMOUSE
    const session_config_t *config = &input->session->config;
    if (config->hardware_mouse) {
        session_evmouse_disable(&input->evmouse);
    }
#endif
}

void session_input_screen_keyboard_closed(stream_input_t *input) {
#if FEATURE_INPUT_EVMOUSE
    const session_config_t *config = &input->session->config;
    if (config->hardware_mouse) {
        session_evmouse_enable(&input->evmouse);
    }
#endif
    stream_input_flush_pressed_keys(input);
}
/* --- handing a controller to the bridge, and taking it back ---------------
 *
 * THE ORDER IN EACH IS LOAD-BEARING, and doing it the other way round makes
 * both a no-op: the send paths consult the mask, so a remove sent after the
 * bit is set would refuse, and an arrive sent before the bit is cleared would
 * refuse too.
 *
 * `started` is checked because a controller can be bridged before a stream
 * begins. There is nothing to tell the host at that point -- the mask is
 * enough, and session_input_started() reads it when the stream opens. */
void stream_input_exclude_gamepad(stream_input_t *input, app_gamepad_state_t *gamepad) {
    if (input == NULL || gamepad == NULL || gamepad->gs_id < 0) {
        return;
    }
    if (input->moonlightExcludedMask & (1u << gamepad->gs_id)) {
        return;
    }
    if (input->started) {
        stream_input_send_gamepad_remove(input, gamepad);   /* BEFORE the bit */
    }
    input->moonlightExcludedMask |= (uint16_t) (1u << gamepad->gs_id);
}

void stream_input_restore_gamepad(stream_input_t *input, app_gamepad_state_t *gamepad) {
    if (input == NULL || gamepad == NULL || gamepad->gs_id < 0) {
        return;
    }
    if ((input->moonlightExcludedMask & (1u << gamepad->gs_id)) == 0) {
        return;
    }
    input->moonlightExcludedMask &= (uint16_t) ~(1u << gamepad->gs_id);   /* BEFORE the arrive */
    if (input->started) {
        stream_input_send_gamepad_arrive(input, gamepad);
    }
}
