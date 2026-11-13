/* Desktop Mode's switch. The why is in the header. */

#include "desktop_mode.h"

#include <stdio.h>
#include <string.h>

#include <SDL.h>

#include "app.h"
#include "logging.h"
#include "stream/session.h"
#include "stream/session_priv.h"
#include "stream/input/lock_keys.h"
#include "stream/input/session_input.h"
#include "stream/input/session_virt_mouse.h"
#include "input/touchpad_pointer.h"
#include "input/tv_cursor.h"
#include "ui/streaming/cursor_align.h"
#if defined(TARGET_WEBOS)
#include "ctm_bridge_glue.h"
#include "input/ctm_bridge_gesture.h"
#endif

#if defined(TARGET_WEBOS)
#define desktop_note(...) ctm_bridge_gesture_note("desktop mode: " __VA_ARGS__)
#else
#define desktop_note(...) ((void) 0)
#endif

/* ⭐ HOW LONG A STREAM'S START WAITS FOR DS5-USBIP before Desktop Mode starts by itself
 * (the header), from the moment the stream plays: three seconds (rhoquinn8217,
 * 2026-11-12: "lets play it by ear and do a 3 second wait. If there is no response for
 * 3 seconds turn on Desktop Mode."). The PC that streams answers only through
 * DS5-USBIP, so no answer at all is the sign-in after a restart, which this is for.
 * ⓘ Measured on the C1 the same day: a DS5-USBIP already running answered within
 * 0.1 s, and ones just opened (by the stream's start or after an install) after 8.6 to
 * 15.7 s. Such a start now turns Desktop Mode on and hands it over a few seconds later.
 * ➡️ The log says how long DS5-USBIP took at every stream's start ("DS5-USBIP
 * answered N ms into the stream"): read it before changing this. */
#define START_WAIT_MS 3000

static const void *s_session;     /* the stream last seen, to tell a new one */
/* ⭐ Desktop Mode, for this stream and a resume of it: not saved since 2026-11-12. */
static bool s_on;
/* A stream's start waits for DS5-USBIP to say whether Desktop Mode starts by itself;
 * the wait counts from the moment the stream plays. */
static bool s_start_waiting;
static Uint32 s_start_since;
/* Switched by hand in this stream: it does not start by itself after that. */
static bool s_by_hand;
/* Being handed over to DS5-USBIP (input/auto_bridge.c). */
static bool s_handover;

bool desktop_mode_on(void) {
    return s_on;
}

void desktop_mode_cursor_toggle(void) {
    if (tv_cursor_mode_on()) {
        if (tv_cursor_set_mode(false)) {
            desktop_note("the TV cursor off (L3)");
        }
        return;
    }
    /* ⓘ Refused without a word on the screen, as a bridged mouse is
     * (tv_cursor_set_mode): rhoquinn8217, 2026-11-12, "you created too many messages
     * about the cursor doesn't need explaining". */
    if (!tv_cursor_mode_switchable()) {
        desktop_note("the TV cursor stays off: Use mouse hardware is on (L3)");
        return;
    }
    if (!tv_cursor_set_mode(true)) {
        desktop_note("the TV cursor stays off (L3)");
        return;
    }
    desktop_note("the TV cursor on, and its alignment bar (L3)");
    cursor_align_request();
}

/* On or off, with how in the log. On also turns Virtual Mouse off. */
static void set_mode(bool on, const char *how) {
    if (on && app_configuration != NULL) {
        /* ⭐ Virtual Mouse goes off (the header), and stays off: its switch is saved. The
         * stream's own too, unless a lock or a prompt has it lent out: Desktop Mode
         * gives it back off when it ends either way (stream/input/lock_keys.c,
         * lock_begin). */
        const bool vmouse_was = app_configuration->virtual_mouse;
        app_configuration->virtual_mouse = false;
        if (global != NULL && global->session != NULL && !lock_keys_active()) {
            stream_input_t *input = session_get_input(global->session);
            if (input != NULL) {
                session_input_set_vmouse_active(&input->vmouse, false);
            }
        }
        if (vmouse_was) {
            settings_save(app_configuration);
        }
    }
    if (s_on == on) {
        return;
    }
    s_on = on;
    commons_log_info("Desktop", "Desktop Mode %s (%s)", on ? "on" : "off", how != NULL ? how : "?");
    desktop_note("%s (%s)", on ? "on" : "off", how != NULL ? how : "?");
}

void desktop_mode_set(bool on, const char *how) {
    s_by_hand = true;
    set_mode(on, how);
}

void desktop_mode_handover_begin(void) {
    s_handover = true;
}

bool desktop_mode_handing_over(void) {
    return s_handover;
}

void desktop_mode_handover_end(const char *why) {
    if (!s_handover) {
        return;
    }
    s_handover = false;
    set_mode(false, why);
    /* The cursor above stops following the controller now: it goes at once. */
    touchpad_pointer_stream_hide();
}

/* Is DS5-USBIP there to take the controllers: answering, and able to work with this
 * app? ⓘ None on a desktop build, where nothing starts by itself. */
static bool ds5usbip_on(void) {
#if defined(TARGET_WEBOS)
    if (!ctm_bridge_agent_probed() || !ctm_bridge_agent_online()) {
        return false;
    }
    const ctm_bridge_compat_t compat = ctm_bridge_agent_compat();
    return compat != CTM_BRIDGE_COMPAT_UPDATE_PC && compat != CTM_BRIDGE_COMPAT_UPDATE_TV;
#else
    return true;
#endif
}

/* ⭐ THIS APP'S OWN MEMORY OF THE STREAM BEFORE (2026-11-12, the C1, build 695: a
 * Disconnect, then the same app again on the same host, was taken for a stream's start, and
 * Desktop Mode turned itself on, though the host had kept the app running and ran no start
 * command for it: the host's running app, as the host list held it, did not say so). The
 * same host and app as the stream before, which ended without a quit: the app is still
 * running there. Kept while a stream runs; forgotten when this app restarts, where only the
 * host list's word is left. */
static struct {
    bool valid;
    char uuid[64];
    int app_id;
    bool quit;
} s_prev;

static void prev_keep(const session_t *session) {
    s_prev.valid = true;
    snprintf(s_prev.uuid, sizeof s_prev.uuid, "%s",
             session->server != NULL && session->server->uuid != NULL ? session->server->uuid : "");
    s_prev.app_id = session->app_id;
    s_prev.quit = session->quitapp;
}

/* A resume: the host is running the very app this stream asked for, by the host list's
 * word (gs_start_app resumes it then) or by this app's memory of the stream before
 * (s_prev). */
static bool stream_is_resume(const app_t *app) {
    const session_t *session = app->session;
    if (session == NULL || session->server == NULL) {
        return false;
    }
    if (session->server->currentGame != 0 && session->server->currentGame == session->app_id) {
        return true;
    }
    return s_prev.valid && !s_prev.quit && s_prev.app_id == session->app_id && session->server->uuid != NULL &&
           strcmp(s_prev.uuid, session->server->uuid) == 0;
}

void desktop_mode_tick(app_t *app) {
    const void *session = app != NULL ? (const void *) app->session : NULL;
    if (session != s_session) {
        /* A stream started, resumed or ended. */
        s_session = session;
        s_handover = false;
        s_start_waiting = false;
        /* ⓘ What it was decided on, in the log: the host's running app as the host list
         * held it, this stream's app, and the stream before. */
        if (session != NULL) {
            const int host_says = app->session->server != NULL ? app->session->server->currentGame : -1;
            if (s_prev.valid) {
                desktop_note("a stream of app %d: the host list says app %d runs; the stream before was app %d, %s",
                             app->session->app_id, host_says, s_prev.app_id, s_prev.quit ? "quit" : "not quit");
            } else {
                desktop_note("a stream of app %d: the host list says app %d runs; no stream before in this run",
                             app->session->app_id, host_says);
            }
        }
        if (session != NULL && stream_is_resume(app)) {
            desktop_note("a resume: %s, as it was", s_on ? "on" : "off");
        } else if (session != NULL) {
            /* ⭐ A STREAM'S START: Desktop Mode starts off, and may start by itself
             * below (the header). */
            set_mode(false, "a stream's start");
            s_by_hand = false;
            s_start_waiting = app_configuration != NULL && app_configuration->bridge.desktop_mode_start;
            s_start_since = 0;
        }
    }
    if (app != NULL && app->session != NULL) {
        prev_keep(app->session);   /* kept as it runs: a quit is known only at its end */
    }
    if (!s_start_waiting || app == NULL || app->session == NULL) {
        return;
    }
    if (s_by_hand || s_on) {
        s_start_waiting = false;   /* switched by hand meanwhile */
        return;
    }
    if (!session_is_streaming(app->session)) {
        return;   /* the wait counts from the moment the stream plays */
    }
    const Uint32 now = SDL_GetTicks();
    if (s_start_since == 0) {
        s_start_since = now != 0 ? now : 1;
    }
    if (ds5usbip_on()) {
        s_start_waiting = false;
        desktop_note("DS5-USBIP answered %u ms into the stream: not started", (unsigned) (now - s_start_since));
        return;
    }
    if ((Sint32) (now - s_start_since) < START_WAIT_MS) {
        return;
    }
    s_start_waiting = false;
    set_mode(true, "a stream's start, DS5-USBIP not on");
}
