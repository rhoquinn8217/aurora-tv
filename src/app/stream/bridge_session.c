#include "bridge_session.h"

#include "app.h"
#include "app_settings.h"
#include "session_priv.h"
#include "logging.h"
#include "stream/input/session_virt_mouse.h"
#include "ctm_bridge_glue.h"
#include "bridge_proof.h"
#include "input/bridge_override.h"
#include "input/bridge_keyboard.h"
#if defined(TARGET_WEBOS)
#include "input/ctm_bridge_gesture.h"
#include "input/auto_bridge.h"
#endif

bool bridge_session_vmouse_allowed(void) {
    return !bridge_override_active();
}

/* ⛔⛔ NOT INSIDE SDL'S EVENT FILTER (code review, 2026-10-05). The stream's
 * start and stop reach the two functions below from session_start_input and
 * session_stop_input, which upstream calls while it dispatches
 * USER_STREAM_OPEN and USER_STREAM_CLOSE from inside SDL_FilterEvents -- with
 * SDL's event-queue lock held. Starting the bridge talks to the listener, and
 * Auto Bridge plugs pads, which over a slow link takes seconds; stopping it
 * joins every bridged device's threads. Meanwhile any other thread that posts
 * an event waits on that lock, and one the stop is joining -- a bridged
 * keyboard's input thread posting its overlay chord -- never returns: a
 * deadlock.
 * ➡️ So the two only RECORD what was asked, and bridge_session_run_pending()
 * does it, from bridge_app_events, which runs straight after the filter in the
 * same pass of the main loop. A start and its stop asked in the same pass
 * cancel out: that stream never had a bridge. */
typedef enum { PENDING_START, PENDING_STOP } pending_kind_t;
#define PENDING_MAX 4
static struct {
    pending_kind_t kind;
    session_t *session;
} s_pending[PENDING_MAX];
static int s_pending_count;

static void bridge_session_start_now(session_t *session);
static void bridge_session_stop_now(void);

void bridge_session_started(session_t *session) {
    if (s_pending_count == PENDING_MAX) {
        /* ⓘ Never seen: four starts and stops in one pass. Run them rather
         * than lose one. */
        bridge_session_run_pending();
    }
    s_pending[s_pending_count].kind = PENDING_START;
    s_pending[s_pending_count].session = session;
    ++s_pending_count;
}

void bridge_session_stopped(void) {
    if (s_pending_count > 0 && s_pending[s_pending_count - 1].kind == PENDING_START) {
        --s_pending_count;   /* its start never ran: nothing to stop */
        return;
    }
    if (s_pending_count == PENDING_MAX) {
        bridge_session_run_pending();
    }
    s_pending[s_pending_count].kind = PENDING_STOP;
    s_pending[s_pending_count].session = NULL;
    ++s_pending_count;
}

void bridge_session_run_pending(void) {
    /* ⓘ Taken off the list before each runs, so a start or stop asked from
     * inside one lands after it rather than being lost. */
    while (s_pending_count > 0) {
        const pending_kind_t kind = s_pending[0].kind;
        session_t *session = s_pending[0].session;
        for (int i = 1; i < s_pending_count; ++i) {
            s_pending[i - 1] = s_pending[i];
        }
        --s_pending_count;
        if (kind == PENDING_START) {
            bridge_session_start_now(session);
        } else {
            bridge_session_stop_now();
        }
    }
}

static void bridge_session_start_now(session_t *session) {
    /* ⭐⭐ THE SETTINGS GO IN FIRST, before the bridge starts, so the core
     * starts with what they say now. */
    ctm_bridge_set_gesture_enabled(app_configuration->bridge.enable &&
                                   app_configuration->bridge.gesture);
    ctm_bridge_set_signals(app_configuration->bridge.signal_light,
                           app_configuration->bridge.signal_rumble,
                           app_configuration->bridge.signal_tone);
    /* ⓘ Wired only. The Bluetooth setting is greyed out on this branch and
     * the core refuses it regardless -- see app_settings.h. */
    ctm_bridge_set_mic_capture(app_configuration->bridge.mic_wired);
    /* ⓘ Off, the bridge starts nothing for this stream; see the glue. */
    ctm_bridge_set_enabled(app_configuration->bridge.enable);

    // Keep Moonlight's controllers open (UI nav still works); host sends are
    // gated and the controller-arrival is suppressed, so nothing reaches the
    // host. The bridge forwards the plugged controller to the game itself.
    //
    // The agent runs on the machine we are streaming from, so hand the
    // bridge that address rather than letting it broadcast for one: a
    // broadcast probe never leaves the local network, so a host reached
    // over the internet is never found.
    /* ⭐ The pairing proof's signer, before anything can bridge: the
     * listener asks a TV calling from the internet to prove it is paired
     * with the PC, with the key it paired with (bridge_proof.c). */
    bridge_proof_register(app_configuration->key_dir);
    ctm_bridge_set_host(session->server->serverInfo.address, 0);
    ctm_bridge_start();
    /* ⭐ The controllers the user marked bridge themselves now, and only
     * now. ⓘ Deliberately AFTER ctm_bridge_start, not inside it: the
     * mark keys on the controller's own MAC, which only SDL knows, and
     * the glue cannot see SDL.
     *
     * ⛔ And never with "Enable Device Bridging" off. The settings screen
     * greys the Auto Bridge button then, but the marks and "Bridge all"
     * stay saved for when it is switched back on, and this ran on them
     * regardless: every device bridged at stream start while bridging
     * was switched off. */
#if defined(TARGET_WEBOS)
    if (app_configuration->bridge.enable) {
        const int n = auto_bridge_run(app_configuration->bridge.auto_macs,
                                      app_configuration->bridge.auto_all);
        if (n > 0) {
            commons_log_info("Session", "auto bridge: asked for %d %s device(s)", n,
                             app_configuration->bridge.auto_all ? "connected" : "marked");
        }
    }
#endif
}

static void bridge_session_stop_now(void) {
    ctm_bridge_stop();
    /* ⭐ The stream is over, so every controller is the TV's again -- say so in
     * the light. One of only three places the player colour is set; see
     * ctm_bridge_gesture_restore_player_colours. */
#if defined(TARGET_WEBOS)
    ctm_bridge_gesture_restore_player_colours();
#endif
}

bool bridge_session_vmouse_pressed(session_t *session) {
    /* ⭐ THE PRESS WINS (rhoquinn8217, 2026-09-30, in place of a notice refusing
     * it): Virtual Mouse pressed while Bridge Override is on switches the
     * override off and the virtual mouse on.
     * ⓘ Every way of switching the virtual mouse comes through
     * session_toggle_vmouse(): the overlay's button, and the USER_TOGGLE_VMOUSE
     * event, which the control port sends. So this is the one place that
     * decides it, and the overlay's two handlers are upstream's.
     * ⭐ Since 2026-10-01 this is the ONLY way a person switches the override
     * off. The question a bridge raises switches it on (bridge_prompt.h), and
     * it has no button of its own. ⓘ Nothing is said about it: the override's
     * other switches come back with the mouse, without a notice. */
    if (!bridge_override_active()) {
        return false;
    }
    bridge_override_release_for_vmouse(session);
    return true;
}

bool bridge_session_vmouse_active(session_t *session) {
    return session != NULL && session_input_is_vmouse_active(&session->input.vmouse);
}
