/* Bridge Override. The why, and the two rules it is built around, are in the
 * header. */

#include "bridge_override.h"

#include <stdio.h>

#include "app.h"            /* app_configuration */
#include "app_settings.h"
#include "logging.h"
#include "stream/session.h"
#include "stream/session_priv.h"
#include "stream/input/session_input.h"
#include "stream/input/session_virt_mouse.h"

bool bridge_override_active(void) {
    /* ⓘ Only with bridging enabled: with it off there is nothing bridged for
     * the TV's own handling to get in the way of, and the switch is hidden with
     * the USB Bridge button, so it could not be seen to be on. */
    return app_configuration != NULL && app_configuration->bridge_enable &&
           app_configuration->bridge_override;
}

void bridge_override_apply(stream_input_t *input) {
    if (input == NULL || input->session == NULL) {
        return;
    }
    /* ⭐ THE STREAM'S CONFIG IS THE ONE TO RESTORE FROM. It is copied from the
     * saved settings when the stream is created and nothing writes it after, so
     * it still says what the person chose. */
    const session_config_t *config = &input->session->config;
    const bool on = bridge_override_active();

    input->touchpad_mode = (uint8_t) (on ? TOUCHPAD_MODE_NATIVE : config->touchpad_mode);
    if (input->touchpad_mode != TOUCHPAD_MODE_MOUSE) {
        /* Mouse mode's per-pad state goes, as it does when a stream ends. */
        stream_input_touchpad_mouse_deinit(input);
    } else if (input->started) {
        /* Back to mouse mode mid-stream: the state it needs is normally made
         * as the stream starts, which has already happened. */
        stream_input_touchpad_mouse_init(input);
    }
    input->touchpad_multitouch = on ? false : config->touchpad_multitouch;
    /* The scroll direction is the sign of the scale; its size is not ours. */
    const float scroll = input->touchpad_scroll_scale < 0 ? -input->touchpad_scroll_scale
                                                          : input->touchpad_scroll_scale;
    input->touchpad_scroll_scale = (!on && config->touchpad_natural_scroll) ? scroll : -scroll;
    input->report_gamepad_battery = on ? false : config->report_gamepad_battery;

    if (on) {
        session_input_set_vmouse_active(&input->vmouse, false);
    } else if (input->started && config->vmouse) {
        /* ⓘ Before the stream starts there is nothing to restore: the stream
         * start turns it on from the same config. */
        session_input_set_vmouse_active(&input->vmouse, true);
    }
    /* ⓘ The USB keyboard grab is NOT one of them any more (rhoquinn8217,
     * 2026-09-30). The grab itself leaves a bridged keyboard to the bridge
     * (bridge_keyboard.h), so switching it off here only took upstream's
     * handling away from the keyboards that are not bridged. */
}

void bridge_override_set(session_t *session, bool on) {
    if (app_configuration == NULL) {
        return;
    }
    app_configuration->bridge_override = on;
    if (session == NULL) {
        commons_log_info("Input", "Bridge override %s, from the next stream", on ? "on" : "off");
        return;
    }
    stream_input_t *input = session_get_input(session);
    bridge_override_apply(input);
    char state[256];
    bridge_override_describe(input, state, sizeof state);
    commons_log_info("Input", "Bridge override switched %s: %s", on ? "on" : "off", state);
}

void bridge_override_release_for_vmouse(session_t *session) {
    bridge_override_set(session, false);
    if (session == NULL) {
        return;
    }
    stream_input_t *input = session_get_input(session);
    /* ⓘ Switching the override off brings the virtual mouse back only if the
     * Input setting has it on. The press asked for it either way. */
    if (input->started) {
        session_input_set_vmouse_active(&input->vmouse, true);
    }
    commons_log_info("Input", "Virtual Mouse pressed with Bridge Override on: override off, virtual mouse %s",
                     session_input_is_vmouse_active(&input->vmouse) ? "on" : "off");
}

void bridge_override_describe(stream_input_t *input, char *buf, size_t len) {
    if (buf == NULL || len == 0) {
        return;
    }
    if (input == NULL) {
        snprintf(buf, len, "override=%s", bridge_override_active() ? "on" : "off");
        return;
    }
#if defined(TARGET_WEBOS)
    const char *grab = input->keyboard_evdev != NULL ? "on" : "off";
#else
    const char *grab = "n/a";
#endif
    snprintf(buf, len,
             "override=%s vmouse=%s touchpad=%s multitouch=%s natural_scroll=%s battery=%s keyboard_grab=%s",
             bridge_override_active() ? "on" : "off",
             session_input_is_vmouse_active(&input->vmouse) ? "on" : "off",
             input->touchpad_mode == TOUCHPAD_MODE_MOUSE ? "mouse" : "touchpad",
             input->touchpad_multitouch ? "on" : "off",
             input->touchpad_scroll_scale > 0 ? "on" : "off",
             input->report_gamepad_battery ? "on" : "off",
             grab);
}
