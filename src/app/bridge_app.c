#include "bridge_app.h"

#include <SDL.h>

#include "control_server.h"
#include "ui/streaming/streaming.controller.h"   /* streaming_overlay_shown */
#include "ui/streaming/bridge_prompt.h"
/* ⓘ On every target: it has the desktop's stand-ins for the gesture, and the
 * microphone guard below is the core's, which every target builds. */
#include "input/ctm_bridge_gesture.h"
#if defined(TARGET_WEBOS)
#include "input/bridge_keyboard.h"
#endif

void bridge_app_before_sdl(void) {
    /* ⛔⛔ BEFORE SDL TOUCHES ANY CONTROLLER.
     *
     * A DualSense told to stream microphone audio keeps doing it when a
     * program dies -- it only forgets when its Bluetooth link drops. So an app
     * that crashed while one was streaming comes back to find SDL reading
     * encoded sound as sticks and buttons, several hundred times a second.
     *
     * Measured 2026-08-13: that is exactly what happened. The app crashed,
     * restarted, and its menus were activated at random until the controller
     * was powered off.
     *
     * ⚠️ CALLING THIS AFTER SDL_Init WOULD QUIETLY REMOVE THE PROTECTION, which
     * is why app.c calls it on the line before (tests/merge-guard.sh checks the
     * order). Nothing in this app arms a microphone; this is here for the state
     * we cannot cause and cannot otherwise escape. */
    ctm_mic_safety_disarm_all();
}

void bridge_app_sdl_hints(void) {
#if TARGET_WEBOS
    /* Ask PlayStation controllers for their full input report rather than
     * waiting for a reason to. Over Bluetooth a DualSense sends a cut-down
     * report -- sticks and buttons, ten bytes, no touchpad at all -- until a
     * host asks for more. Without this the touchpad gesture that bridges a
     * controller has nothing to read, so it can only ever work on a cable.
     * Measured 2026-08-06 on the rooted monitor. */
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS5, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS5_RUMBLE, "1");
#endif
}

void bridge_app_started(app_t *app) {
    /* ⓘ Once the UI and the bus are up, because every command runs through
     * them. A no-op unless built with AURORA_TERMINAL_CONTROL. */
    control_server_start(app);
}

void bridge_app_stopping(void) {
    control_server_stop();
}

void bridge_app_events(app_t *app) {
    /* ⛔⛔ streaming_overlay_shown(), NOT app_ui_is_opened(). Third attempt, and
     * this one has evidence rather than reasoning behind it.
     *
     * ⓘ app_ui_is_opened asks whether the LVGL display exists, and on webOS it
     * exists for the whole life of the app -- the video is drawn behind it. So
     * it reads TRUE while a game is being played, and the input hold stayed on
     * the entire time.
     *
     * ⚠️ THE SYMPTOM THAT PROVED IT: on a bridged DualSense, the lightbar,
     * rumble, speaker and GYRO all worked while buttons, sticks and the
     * touchpad did nothing. ⭐ Those are exactly the fields the blanker zeroes,
     * and the gyro is exactly what it deliberately leaves alone. Output is
     * unaffected either way. Nothing else could produce that pattern. */
    /* ⓘ The question a bridge raises takes the input the same way the
     * overlay does (ui/streaming/bridge_prompt.h), so both count here: a
     * bridged controller is held while either is up, and a keyboard works
     * either. */
    const bool interface_has_input = streaming_overlay_shown() || bridge_prompt_shown();
    ctm_bridge_gesture_tick(&app->input, app->session, interface_has_input);
#if defined(TARGET_WEBOS)
    /* The TV's keyboard grab looking again after a bridge or a release. */
    bridge_keyboard_tick(interface_has_input);
#else
    (void) interface_has_input;
#endif
    /* ⓘ Upstream v1.2.9's touchpad tap-hold, for its touchpad mouse mode, runs
     * in app.c right after this. It reads the same SDL touchpad state the
     * gesture polls here; neither consumes events, so the two coexist.
     * ⚠️ With touchpad_mode = mouse a two-finger hold would bridge AND drive
     * the host cursor -- the mode is opt-in and defaults to native, so that is
     * a choice, not a collision. */
}
