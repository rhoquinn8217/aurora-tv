/* Desktop Mode's switch. The why is in the header. */

#include "desktop_mode.h"

#include "app.h"
#include "logging.h"
#include "stream/session.h"
#include "stream/input/lock_keys.h"
#include "stream/input/session_input.h"
#include "stream/input/session_virt_mouse.h"
#include "input/tv_cursor.h"
#include "ui/streaming/cursor_align.h"
#if defined(TARGET_WEBOS)
#include "input/ctm_bridge_gesture.h"
#endif

#if defined(TARGET_WEBOS)
#define desktop_note(...) ctm_bridge_gesture_note("desktop mode: " __VA_ARGS__)
#else
#define desktop_note(...) ((void) 0)
#endif

static const void *s_session;     /* the stream last seen, to tell a new one */
/* The PS button has its list over the screen; hidden again at each stream and as it is
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
    /* ⓘ Refused without a word on the screen, as a bridged mouse is
     * (tv_cursor_set_mode): rhoquinn8217, 2026-11-12, "you created too many messages
     * about the cursor doesn't need explaining". */
    if (!tv_cursor_mode_switchable()) {
        desktop_note("the TV cursor stays off: Use mouse hardware is on (Triangle)");
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
    } else {
        /* Its list, turned off, starts hidden again. */
        s_legend = false;
    }
    if (was == on) {
        return;
    }
    settings_save(app_configuration);
    commons_log_info("Desktop", "Desktop Mode %s (%s)", on ? "on" : "off", how != NULL ? how : "?");
    desktop_note("%s (%s)", on ? "on" : "off", how != NULL ? how : "?");
}

void desktop_mode_tick(app_t *app) {
    const void *session = app != NULL ? (const void *) app->session : NULL;
    if (session != s_session) {
        /* A stream started, resumed or ended: its list starts hidden ("5. hidden"). */
        s_session = session;
        s_legend = false;
    }
}
