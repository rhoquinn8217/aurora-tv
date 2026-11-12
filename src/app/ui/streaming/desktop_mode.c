/* Desktop Mode's switch and its questions. The why is in the header. */

#include "desktop_mode.h"

#include <SDL.h>

#include "app.h"
#include "logging.h"
#include "stream/session.h"
#include "stream/input/lock_keys.h"
#include "stream/input/session_input.h"
#include "stream/input/session_virt_mouse.h"
#include "input/tv_cursor.h"
#include "ui/streaming/bridge_prompt.h"
#include "ui/streaming/cursor_align.h"
#include "ui/streaming/streaming.controller.h"
#include "util/i18n.h"
#if defined(TARGET_WEBOS)
#include "ctm_bridge_glue.h"
#include "input/ctm_bridge_gesture.h"
#endif

/* A question not asked this long after what raised it is not asked. */
#define DESKTOP_ASK_WITHIN_MS 10000

typedef enum {
    DESKTOP_ASK_NONE = 0,
    DESKTOP_ASK_ON,    /* "Turn on Desktop Mode?" */
    DESKTOP_ASK_OFF,   /* "Turn off Desktop Mode?" */
} desktop_ask_t;

#if defined(TARGET_WEBOS)
#define desktop_note(...) ctm_bridge_gesture_note("desktop mode: " __VA_ARGS__)
#else
#define desktop_note(...) ((void) 0)
#endif

static const void *s_session;     /* the stream last seen, to tell a new one */
static bool s_start_looked;       /* this stream's start was looked at, or needs no question */
static bool s_verdict_seen;       /* DS5-USBIP's first answer in this stream came */
static bool s_was_online;         /* DS5-USBIP answered, at the last look */
/* The stream it was last switched by hand in: that stream's start asks nothing (found
 * on the rooted monitor, 2026-11-11: turned on in a stream's first seconds, before
 * DS5-USBIP's first answer, it asked straight away whether to turn it off). */
static const void *s_set_in_session;
static desktop_ask_t s_pending = DESKTOP_ASK_NONE;
static uint32_t s_pending_since;
static const char *s_pending_why = "";
/* Triangle has its list over the screen; hidden again at each stream and as it is
 * turned off (the header). */
static bool s_legend;

bool desktop_mode_on(void) {
    return app_configuration != NULL && app_configuration->bridge.desktop_mode;
}

bool desktop_mode_legend_shown(void) {
    return s_legend;
}

void desktop_mode_legend_toggle(void) {
    s_legend = !s_legend;
    desktop_note("its list %s (PS)", s_legend ? "over the screen" : "in the overlay only again");
}

void desktop_mode_cursor_toggle(void) {
    if (tv_cursor_mode_on()) {
        if (tv_cursor_set_mode(false)) {
            desktop_note("the TV cursor off (Triangle)");
        }
        return;
    }
    /* ⓘ A bridged mouse is refused with a message of its own (tv_cursor_set_mode); Use
     * mouse hardware is refused with none, so it is said here. */
    if (!tv_cursor_mode_switchable()) {
        desktop_note("the TV cursor stays off: Use mouse hardware is on (Triangle)");
        bridge_prompt_note(locstr("TV cursor can't be turned on while Use mouse hardware is on."));
        return;
    }
    if (!tv_cursor_set_mode(true)) {
        desktop_note("the TV cursor stays off (Triangle)");
        return;
    }
    desktop_note("the TV cursor on, and its alignment bar (Triangle)");
    cursor_align_request();
}

void desktop_mode_off_on_pad(void) {
    desktop_mode_set(false, "Options held 2 s on a controller");
    bridge_prompt_note(locstr("Desktop Mode is off."));
}

void desktop_mode_set(bool on, const char *how) {
    if (app_configuration == NULL) {
        return;
    }
    const bool was = app_configuration->bridge.desktop_mode;
    app_configuration->bridge.desktop_mode = on;
    if (on) {
        /* ⭐ Virtual Mouse goes off (the header). The stream's own too, unless a lock or a
         * prompt has it lent out: Desktop Mode gives it back off when it ends either way
         * (stream/input/lock_keys.c, lock_begin). ⓘ Even when it was on already: Settings'
         * box writes the switch before its handler asks for this. */
        app_configuration->virtual_mouse = false;
        if (global != NULL && global->session != NULL && !lock_keys_active()) {
            stream_input_t *input = session_get_input(global->session);
            if (input != NULL) {
                session_input_set_vmouse_active(&input->vmouse, false);
            }
        }
    }
    /* A question about it means nothing now, and this stream's start asks none. Its
     * list, turned off, starts hidden again. */
    s_pending = DESKTOP_ASK_NONE;
    if (!on) {
        s_legend = false;
    }
    s_set_in_session = global != NULL ? (const void *) global->session : NULL;
    if (s_set_in_session != NULL && s_set_in_session == s_session) {
        s_start_looked = true;
    }
    if (was == on) {
        return;
    }
    settings_save(app_configuration);
    commons_log_info("Desktop", "Desktop Mode %s (%s)", on ? "on" : "off", how != NULL ? how : "?");
    desktop_note("%s (%s)", on ? "on" : "off", how != NULL ? how : "?");
}

const char *desktop_mode_pending(void) {
    return s_pending == DESKTOP_ASK_ON ? "on" : s_pending == DESKTOP_ASK_OFF ? "off" : "none";
}

#if defined(TARGET_WEBOS)
static void ask(desktop_ask_t what, uint32_t now, const char *why) {
    if (what == DESKTOP_ASK_NONE) {
        return;
    }
    s_pending = what;
    s_pending_since = now;
    s_pending_why = why;
}

/* The question waiting, asked once nothing else is in its way. */
static void pending_try(uint32_t now) {
    if (s_pending == DESKTOP_ASK_NONE) {
        return;
    }
    if (now - s_pending_since >= DESKTOP_ASK_WITHIN_MS) {
        desktop_note("not asked whether to turn it %s (%s): something else was up for %d s",
                     s_pending == DESKTOP_ASK_ON ? "on" : "off", s_pending_why, DESKTOP_ASK_WITHIN_MS / 1000);
        s_pending = DESKTOP_ASK_NONE;
        return;
    }
    /* Already as it would ask. */
    if ((s_pending == DESKTOP_ASK_ON) == desktop_mode_on()) {
        s_pending = DESKTOP_ASK_NONE;
        return;
    }
    /* ⓘ A lock's or a prompt's list has the screen and the controls: they take over
     * from Desktop Mode, and come before it. */
    const lock_keys_mode_t mode = lock_keys_mode();
    if (mode != LOCK_KEYS_OFF && mode != LOCK_KEYS_DESKTOP) {
        return;
    }
    if (bridge_prompt_shown() || bridge_prompt_note_up() || streaming_overlay_shown() ||
        streaming_soft_keyboard_shown()) {
        return;
    }
    const bool on = s_pending == DESKTOP_ASK_ON;
    if (bridge_prompt_request_desktop(on)) {
        desktop_note("asking whether to turn it %s (%s)", on ? "on" : "off", s_pending_why);
        s_pending = DESKTOP_ASK_NONE;
    }
}
#endif

void desktop_mode_tick(app_t *app) {
#if defined(TARGET_WEBOS)
    const void *session = app != NULL ? (const void *) app->session : NULL;
    if (session == NULL) {
        s_session = NULL;
        s_set_in_session = NULL;
        s_pending = DESKTOP_ASK_NONE;
        s_legend = false;
        return;
    }
    const uint32_t now = SDL_GetTicks();
    if (session != s_session) {
        /* A stream started, or resumed: its list starts hidden ("5. hidden"). */
        s_session = session;
        s_legend = false;
        s_start_looked = s_set_in_session == session;
        s_verdict_seen = false;
        s_was_online = false;
        s_pending = DESKTOP_ASK_NONE;
    }
    /* ⓘ Nothing is known of DS5-USBIP until the probe's first answer in this stream. */
    if (!ctm_bridge_active() || !ctm_bridge_agent_probed()) {
        pending_try(now);
        return;
    }
    const bool online = ctm_bridge_agent_online();
    if (!s_verdict_seen) {
        s_verdict_seen = true;
        if (!s_start_looked) {
            s_start_looked = true;
            if (desktop_mode_on()) {
                ask(online ? DESKTOP_ASK_OFF : DESKTOP_ASK_NONE, now, "the stream's start, DS5-USBIP answering");
            } else {
                ask(online ? DESKTOP_ASK_NONE : DESKTOP_ASK_ON, now, "the stream's start, DS5-USBIP not answering");
            }
        }
    } else if (online && !s_was_online && desktop_mode_on()) {
        ask(DESKTOP_ASK_OFF, now, "DS5-USBIP came online");
    }
    s_was_online = online;
    pending_try(now);
#else
    (void) app;
#endif
}
