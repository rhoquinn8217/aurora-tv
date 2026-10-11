#include "app_input.h"
#include "logging.h"
#include "app.h"
#include "input_gamepad_mapping.h"
#include "input_gamepad.h"

#include "lvgl/lv_sdl_drv_input.h"

void app_input_init(app_input_t *input, app_t *app) {
    input->app = app;
    if (app->settings.condb_path != NULL) {
        app_input_copy_initial_gamepad_mapping(&app->settings);
#if SDL_VERSION_ATLEAST(2, 0, 10)
        SDL_SetHint(SDL_HINT_GAMECONTROLLERCONFIG_FILE, app->settings.condb_path);
#endif
    }
    SDL_InitSubSystem(SDL_INIT_JOYSTICK | SDL_INIT_GAMECONTROLLER);
    input->max_num_gamepads = 4;
    input->gamepads_count = 0;
    for (int i = 0; i < input->max_num_gamepads; i++) {
        input->gamepads[i].instance_id = -1;
        input->gamepads[i].gs_id = -1;
    }
    input->activeGamepadMask = 0;
    input->blank_cursor_surface = SDL_CreateRGBSurface(0, 16, 16, 32, 0x00FF0000, 0x0000FF00, 0x000000FF, 0xFF000000);
    input->blank_cursor_surface->userdata = SDL_CreateColorCursor(input->blank_cursor_surface, 0, 0);
    if (input->blank_cursor_surface->userdata == NULL) {
        commons_log_warn("Input", "Failed to create blank cursor: %s", SDL_GetError());
    }
#if !SDL_VERSION_ATLEAST(2, 0, 10)
    SDL_GameControllerAddMappingsFromFile(app->settings.condb_path);
#endif
    app_input_init_gamepad_mapping(input, app->backend.executor, &app->settings);
    /* Devices already connected at init may not deliver JOYDEVICEADDED until
     * later (or at all on webOS BT). Enumerate once up front. */
    app_input_scan_gamepads(input);
}

void app_input_deinit(app_input_t *input) {
    app_input_deinit_gamepad_mapping(input);
    if (input->blank_cursor_surface->userdata != NULL) {
        SDL_FreeCursor(input->blank_cursor_surface->userdata);
    }
    SDL_FreeSurface(input->blank_cursor_surface);
    SDL_QuitSubSystem(SDL_INIT_JOYSTICK | SDL_INIT_GAMECONTROLLER);
}

void app_set_mouse_grab(app_input_t *input, bool grab) {
    if (!app_configuration->hardware_mouse) {
        SDL_SetRelativeMouseMode(grab && !app_configuration->absmouse ? SDL_TRUE : SDL_FALSE);
    }
#ifdef TARGET_WEBOS
    // Temporary workaround for webOS 9: https://github.com/mariotaku/moonlight-tv/issues/466
    if (input->app->os_info.version.major >= 9) {
        return;
    }
    /* ⭐ THE TV'S CURSOR STAYS IN AN ABSOLUTE-MOUSE STREAM, ON EVERY webOS
     * (rhoquinn8217, 2026-10-11: "When the mouse is released, I like when the
     * tv cursor is visible with the windows cursor"). The webOS 9 sets above
     * already never hide it. In absolute mode the two cursors sit together,
     * since the host's follows wherever the TV's points; relative mode and
     * "Use mouse hardware" keep hiding it, as there they would drift apart. */
    if (grab && app_configuration->absmouse && !app_configuration->hardware_mouse) {
        SDL_ShowCursor(SDL_TRUE);
        return;
    }
#endif
    SDL_ShowCursor(grab ? SDL_FALSE : SDL_TRUE);
}

bool app_get_mouse_relative() {
    return SDL_GetRelativeMouseMode() == SDL_TRUE;
}
