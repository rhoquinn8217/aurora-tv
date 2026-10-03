#include "bridge_session.h"

#include "app.h"
#include "app_settings.h"
#include "session_priv.h"
#include "logging.h"
#include "stream/input/session_virt_mouse.h"
#include "ctm_bridge_glue.h"
#include "input/bridge_override.h"
#include "input/bridge_keyboard.h"
#if defined(TARGET_WEBOS)
#include "input/ctm_bridge_gesture.h"
#include "input/auto_bridge.h"
#endif

bool bridge_session_vmouse_allowed(void) {
    return !bridge_override_active();
}

void bridge_session_started(session_t *session) {
    /* ⭐⭐ THE SETTINGS GO IN FIRST, whether or not the bridge is already
     * running. ⛔ They used to sit in the branch below that starts a fresh
     * bridge, so a stream that came back from an auto-reconnect kept
     * whatever the core had from last time -- and a changed setting looked
     * like it did nothing. */
    ctm_bridge_set_gesture_enabled(app_configuration->bridge_enable &&
                                   app_configuration->bridge_gesture);
    ctm_bridge_set_signals(app_configuration->bridge_signal_light,
                           app_configuration->bridge_signal_rumble,
                           app_configuration->bridge_signal_tone);
    /* ⓘ Wired only. The Bluetooth setting is greyed out on this branch and
     * the core refuses it regardless -- see app_settings.h. */
    ctm_bridge_set_mic_capture(app_configuration->bridge_mic_wired);

    if (ctm_bridge_active()) {
        // Stream came back after an auto-reconnect: the bridge was left
        // running so the controllers stayed plugged through the outage.
        // Re-plug anything a longer outage dropped (no-op when still plugged).
        // The TV's keyboard grab lets go first, as for any bridge.
        bridge_keyboard_before_plug();
        ctm_bridge_plug_all();
    } else {
        // Keep Moonlight's controllers open (UI nav still works); host sends are
        // gated and the controller-arrival is suppressed, so nothing reaches the
        // host. The bridge forwards the plugged controller to the game itself.
        //
        // The agent runs on the machine we are streaming from, so hand the
        // bridge that address rather than letting it broadcast for one: a
        // broadcast probe never leaves the local network, so a host reached
        // over the internet is never found.
        ctm_bridge_set_host(session->server->serverInfo.address, 0);
        ctm_bridge_start();
        /* ⭐ The controllers the user marked bridge themselves now, and only
         * now. ⓘ Deliberately AFTER ctm_bridge_start, not inside it: the
         * mark keys on the controller's own MAC, which only SDL knows, and
         * the glue cannot see SDL. ⛔ Not in the reconnect branch above --
         * that is a stream resuming, and its controllers never left.
         *
         * ⛔ And never with "Enable Device Bridging" off. The settings screen
         * greys the Auto Bridge button then, but the marks and "Bridge all"
         * stay saved for when it is switched back on, and this ran on them
         * regardless: every device bridged at stream start while bridging
         * was switched off. */
        if (app_configuration->bridge_enable) {
            const int n = auto_bridge_run(app_configuration->bridge_auto_macs,
                                          app_configuration->bridge_auto_all);
            if (n > 0) {
                commons_log_info("Session", "auto bridge: asked for %d %s device(s)", n,
                                 app_configuration->bridge_auto_all ? "connected" : "marked");
            }
        }
    }
}

void bridge_session_stopped(void) {
    ctm_bridge_stop();
    /* ⭐ The stream is over, so every controller is the TV's again -- say so in
     * the light. One of only three places the player colour is set; see
     * ctm_bridge_gesture_restore_player_colours. */
#if defined(TARGET_WEBOS)
    ctm_bridge_gesture_restore_player_colours();
#endif
}

bool bridge_session_hold_vmouse_off(session_t *session) {
    /* ⭐ Every way of switching the virtual mouse comes through
     * session_toggle_vmouse(): the overlay button and the USER_TOGGLE_VMOUSE
     * event both do. The overlay's own callers switch Bridge Override off
     * first, since the press wins there; this holds the mouse off for anything
     * that does not. */
    if (bridge_override_active()) {
        session_input_set_vmouse_active(&session->input.vmouse, false);
        return true;
    }
    return false;
}

bool bridge_session_vmouse_active(session_t *session) {
    return session != NULL && session_input_is_vmouse_active(&session->input.vmouse);
}
