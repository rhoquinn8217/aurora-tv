/* The TV's own mouse controls while controllers are bridged. The why, and
 * what is kept from one stream to the next, are in the header. */

#include "bridge_override.h"

#include <stdio.h>

#include "app.h"            /* app_configuration */
#include "app_settings.h"
#include "logging.h"
#include "stream/session.h"
#include "stream/session_priv.h"
#include "stream/input/session_input.h"
#include "stream/input/session_virt_mouse.h"

/* The pop-up's button turned the touchpad's mouse mode off, for this stream. */
static bool s_touchpad_off;

void bridge_override_mouse_off(session_t *session) {
    if (session == NULL) {
        return;
    }
    stream_input_t *input = session_get_input(session);
    if (input == NULL) {
        return;
    }
    s_touchpad_off = true;
    session_input_set_vmouse_active(&input->vmouse, false);
    /* The touchpad goes with it now, not at the next pass. */
    bridge_override_touchpad_tick(session);
    char state[256];
    bridge_override_describe(input, state, sizeof state);
    commons_log_info("Input", "The TV's mouse controls off for the bridge: %s", state);
}

void bridge_override_stream_started(void) {
    s_touchpad_off = false;
}

void bridge_override_describe(stream_input_t *input, char *buf, size_t len) {
    if (buf == NULL || len == 0) {
        return;
    }
    if (input == NULL) {
        snprintf(buf, len, "no stream");
        return;
    }
#if defined(TARGET_WEBOS)
    const char *grab = input->keyboard_evdev != NULL ? "on" : "off";
#else
    const char *grab = "n/a";
#endif
    snprintf(buf, len, "vmouse=%s touchpad=%s multitouch=%s natural_scroll=%s battery=%s keyboard_grab=%s",
             session_input_is_vmouse_active(&input->vmouse) ? "on" : "off",
             input->touchpad_mode == TOUCHPAD_MODE_MOUSE ? "mouse" : "touchpad",
             input->touchpad_multitouch ? "on" : "off",
             input->touchpad_scroll_scale > 0 ? "on" : "off",
             input->report_gamepad_battery ? "on" : "off",
             grab);
}

void bridge_override_touchpad_tick(session_t *session) {
    if (session == NULL) {
        return;
    }
    stream_input_t *input = session_get_input(session);
    if (input == NULL || !input->started) {
        return;
    }
    const bool vmouse = session_input_is_vmouse_active(&input->vmouse);
    /* Virtual Mouse as it was left: the next stream starts from it. Saved at
     * once, as the TV cursor's shift is: a toggle is rare, and an app that
     * dies on its way out saves nothing after. */
    if (app_configuration != NULL && app_configuration->virtual_mouse != vmouse) {
        app_configuration->virtual_mouse = vmouse;
        settings_save(app_configuration);
        commons_log_info("Input", "Virtual Mouse %s: kept for the next stream", vmouse ? "on" : "off");
    }
    if (vmouse) {
        /* Turned on again: the touchpad comes back with it. */
        s_touchpad_off = false;
    }
    uint8_t wanted = (uint8_t) session->config.touchpad_mode;
    if (s_touchpad_off) {
        wanted = TOUCHPAD_MODE_NATIVE;
    } else if (vmouse) {
        wanted = TOUCHPAD_MODE_MOUSE;
    }
    if (input->touchpad_mode == wanted) {
        return;
    }
    input->touchpad_mode = wanted;
    /* Mouse mode's per-pad state made, or let go, as at a stream's start and end. */
    if (wanted == TOUCHPAD_MODE_MOUSE) {
        stream_input_touchpad_mouse_init(input);
    } else {
        stream_input_touchpad_mouse_deinit(input);
    }
    commons_log_info("Input", "Touchpad %s: Virtual Mouse %s",
                     wanted == TOUCHPAD_MODE_MOUSE ? "is the host's mouse" : "is sent as a touchpad",
                     vmouse ? "on" : "off");
}

bool bridge_override_vmouse_lends_touchpad(stream_input_t *input) {
    return input != NULL && !input->view_only && input->touchpad_mode == TOUCHPAD_MODE_MOUSE &&
           session_input_is_vmouse_active(&input->vmouse);
}

bool bridge_override_two_finger_press_is_gesture(const stream_input_t *input) {
    if (input == NULL || input->session == NULL || app_configuration == NULL) {
        return false;
    }
    return input->touchpad_mode == TOUCHPAD_MODE_MOUSE &&
           input->session->config.touchpad_mode != TOUCHPAD_MODE_MOUSE &&
           app_configuration->bridge.enable && app_configuration->bridge.gesture;
}
