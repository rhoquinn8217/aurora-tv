#include "bridge_app.h"

#include <SDL.h>

#include "control_server.h"
#include "ui/streaming/streaming.controller.h"   /* streaming_overlay_shown */
#include "ui/streaming/bridge_prompt.h"
#include "stream/bridge_session.h"         /* bridge_session_run_pending */
/* ⓘ On every target: it has the desktop's stand-ins for the gesture, and the
 * microphone guard below is the core's, which every target builds. */
#include "input/ctm_bridge_gesture.h"
#if defined(TARGET_WEBOS)
#include "input/bridge_keyboard.h"
#include "ctm_bridge_glue.h"   /* bridge_set_shortcut_request, bridge_set_pointer_used_request */
#endif
#include "input/tv_cursor.h"
#include "input/touchpad_pointer.h"   /* the touchpad as the TV's mouse */
#include "input/mouse_seen.h"
#include "input/overlay_volume.h"   /* the TV's volume from the right stick */
#include "input/auto_bridge.h"      /* a ticked device that connects, bridged */
#include "ui/streaming/cursor_align.h"   /* cursor_align_shown */

#if defined(TARGET_WEBOS)
/* ⭐ The pop-up that asks to turn the TV cursor off (ui/streaming/bridge_prompt.h)
 * comes as soon as a mouse the TV saw move is bridged (rhoquinn8217,
 * 2026-10-11: "The message should appear immediately after a mouse is
 * bridged"), and is raised here, on the app's loop. A mouse part the TV never
 * saw move (input/mouse_seen.h) asks instead at its first use, which the
 * bridge core tells on the mouse's own thread. ⓘ Not both for one bridge: a
 * pop-up raised at the bridge takes the first use that follows it. */
static SDL_atomic_t s_pointer_used;
static int s_asked_at_bridge = 0;

static void pointer_used(void) {
    SDL_AtomicSet(&s_pointer_used, 1);
}
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
    /* ⓘ Once the window is up: the TV's pointer with its tip on the point the
     * host is told, the cursor and the place this TV keeps (input/tv_cursor.h). */
    tv_cursor_init(app->ui.window);
    /* ⓘ After the cursor: the touchpad's pointer wears its picture
     * (input/touchpad_pointer.h). */
    touchpad_pointer_init(app);
    /* ⓘ The thread the TV's volume calls run on (input/overlay_volume.h). */
    overlay_volume_init();
#if defined(TARGET_WEBOS)
    /* ⓘ What the bridge core finds in a bridged keyboard's reports, beside
     * its S: Ctrl+Alt+Shift and A, C, M or N; and a bridged mouse's first use. */
    bridge_set_shortcut_request(bridge_keyboard_bridged_shortcut);
    bridge_set_pointer_used_request(pointer_used);
#endif
}

void bridge_app_stopping(void) {
    /* ⓘ A stop asked in the last pass of the loop still runs. */
    bridge_session_run_pending();
    control_server_stop();
    touchpad_pointer_deinit();
    tv_cursor_deinit();
    overlay_volume_deinit();
}

void bridge_app_events(app_t *app) {
    /* ⭐ First: the bridge's start or stop that the stream asked for inside
     * SDL's event filter, now that the filter has let go of the event lock
     * (bridge_session.c). The tick below then sees the bridge as it is. */
    bridge_session_run_pending();
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
     * overlay does (ui/streaming/bridge_prompt.h), and so does the bar that
     * aligns the TV cursor (ui/streaming/cursor_align.h), so all three count
     * here: a bridged controller is held while any is up, and a keyboard
     * works any. */
    const bool interface_has_input = streaming_overlay_shown() || bridge_prompt_shown() || cursor_align_shown();
    ctm_bridge_gesture_tick(&app->input, app->session, interface_has_input);
    /* ⭐ The TV's volume from the right stick while the overlay is up, from a
     * pad the TV holds or a bridged one (input/overlay_volume.h). After the
     * tick above, which is what holds a bridged pad's input for the overlay. */
    overlay_volume_tick(&app->input, streaming_overlay_shown() && app->session != NULL);
#if defined(TARGET_WEBOS)
    /* The TV's keyboard grab looking again after a bridge or a release. */
    bridge_keyboard_tick(interface_has_input);
    /* The pointers the TV has seen move, and the pop-up that asks about the TV
     * cursor: at the bridge for a mouse seen moving, at its first use for one
     * never seen. */
    mouse_seen_tick();
    /* ⭐ A ticked device bridges itself: at a stream's start once DS5-USBIP
     * answers, and when it connects during the stream (input/auto_bridge.h). */
    auto_bridge_poll(app->session != NULL);
    if (app->session == NULL) {
        s_asked_at_bridge = 0;
    }
    if (mouse_seen_take_bridged()) {
        s_asked_at_bridge++;
        bridge_prompt_request_tv_cursor();
    }
    if (SDL_AtomicSet(&s_pointer_used, 0) != 0) {
        if (s_asked_at_bridge > 0) {
            s_asked_at_bridge--;
            tv_cursor_log("the bridged mouse's first use: asked already, at its bridge");
        } else {
            bridge_prompt_request_tv_cursor();
        }
    }
#else
    (void) interface_has_input;
#endif
    /* The TV's cursor, put back if the window has the screen again. */
    tv_cursor_tick();
    /* ⭐ The touchpad as the TV's mouse while Aurora's own screens have the
     * input: no stream, or the overlay or a bridge's question up
     * (input/touchpad_pointer.h). */
    touchpad_pointer_tick(&app->input, app->session == NULL || streaming_overlay_shown() || bridge_prompt_shown());
    /* ⓘ Upstream v1.2.9's touchpad tap-hold, for its touchpad mouse mode, runs
     * in app.c right after this. It reads the same SDL touchpad state the
     * gesture polls here; neither consumes events, so the two coexist.
     * ⚠️ With touchpad_mode = mouse a two-finger hold would bridge AND drive
     * the host cursor -- the mode is opt-in and defaults to native, so that is
     * a choice, not a collision. */
}
