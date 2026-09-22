#include "app_input.h"
#include "input_gamepad.h"

#include <SDL_events.h>

#include "logging.h"
#include "util/user_event.h"
#include "input_gamepad_mapping.h"
#include "ctm_bridge_glue.h"
#include "ctm_bridge_gesture.h"

void app_input_handle_event(app_input_t *input, const SDL_Event *event) {
    if (event->type == SDL_JOYDEVICEADDED) {
        if (app_input_get_gamepads_count(input) >= app_input_get_max_gamepads(input)) {
            // Ignore controllers more than supported
            commons_log_warn("Input", "Too many controllers, ignoring.");
            return;
        }
        /* ⛔⛔ THE SAME GUARD UPSTREAM GAVE THE CONTROLLER EVENT BELOW, which
         * this one needs now that v1.2.10 has three other openers: the scan at
         * init, at session start and before launch, plus CONTROLLERDEVICEADDED.
         * A pad the scan has already opened arrives here again on its own queued
         * event, is opened a SECOND time, and takes a second gs_id -- so the
         * launch mask carries two bits and the host makes two pads for one
         * controller.
         * ⚠️ MEASURED 2026-09-21 on the C1, in Apollo's own log: "Gamepad 0 will
         * be DualShock 4" and "Gamepad 1 will be DualShock 4", two milliseconds
         * apart, with a single Edge connected. */
#if SDL_VERSION_ATLEAST(2, 0, 6)
        SDL_JoystickID joy_instance_id = SDL_JoystickGetDeviceInstanceID(event->jdevice.which);
        if (joy_instance_id >= 0 && app_input_gamepad_state_by_instance_id(input, joy_instance_id) != NULL) {
            return;
        }
#endif
        app_input_init_gamepad(input, event->jdevice.which);
    } else if (event->type == SDL_JOYDEVICEREMOVED) {
        ctm_bridge_gesture_reset(event->jdevice.which);
        app_input_close_gamepad(input, event->jdevice.which);
    } else if (event->type == SDL_CONTROLLERDEVICEADDED) {
        commons_log_debug("Input", "SDL_CONTROLLERDEVICEADDED");
        /* Some webOS DualSense paths surface CONTROLLERDEVICEADDED without a
         * matching JOYDEVICEADDED (or after it was dropped). Open if missing. */
        if (app_input_get_gamepads_count(input) >= app_input_get_max_gamepads(input)) {
            commons_log_warn("Input", "Too many controllers, ignoring.");
            return;
        }
#if SDL_VERSION_ATLEAST(2, 0, 6)
        SDL_JoystickID instance_id = SDL_JoystickGetDeviceInstanceID(event->cdevice.which);
        if (instance_id >= 0 && app_input_gamepad_state_by_instance_id(input, instance_id) != NULL) {
            return;
        }
#endif
        app_input_init_gamepad(input, event->cdevice.which);
    } else if (event->type == SDL_CONTROLLERDEVICEREMOVED) {
        commons_log_debug("Input", "SDL_CONTROLLERDEVICEREMOVED");
        app_input_close_gamepad(input, event->cdevice.which);
    } else if (event->type == SDL_CONTROLLERDEVICEREMAPPED) {
        commons_log_debug("Input", "SDL_CONTROLLERDEVICEREMAPPED");
    } else if (event->type == SDL_USEREVENT) {
        if (event->user.code == USER_INPUT_CONTROLLERDB_UPDATED) {
            app_input_reload_gamepad_mapping(input);
        }
    }
}
