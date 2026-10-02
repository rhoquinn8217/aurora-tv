/* The pop-up a bridge raises when the TV's own mouse controls are on. The why
 * is in the header. */

#include "bridge_prompt.h"

#include <stdio.h>

#include "lvgl.h"

#include "app.h"            /* global */
#include "app_settings.h"   /* TOUCHPAD_MODE_MOUSE */
#include "logging.h"
#include "input/app_input.h"
#include "input/bridge_override.h"
#include "stream/session.h"
#include "stream/session_priv.h"
#include "streaming.controller.h"
#include "util/i18n.h"

/* How long it waits for an answer. ⓘ Three more than the notice it replaces,
 * which was set for reading two sentences: this one is read and then acted on. */
#define PROMPT_SECONDS 10

static lv_obj_t *s_mbox = NULL;
static lv_timer_t *s_timer = NULL;
static int s_seconds = 0;
/* Why it is closing, for the log: set by whoever closes it, and "dismissed"
 * when the theme's own Back handling does. */
static const char *s_outcome = NULL;
/* The stream's mouse grab was let go so a pointer can press the button. */
static bool s_released_grab = false;

/* The button's text is drawn from here every time, so the count is changed in
 * place and the button redrawn; the map itself is never set again, which
 * would take the selection off the button. */
static char s_ok_label[32];
static const char *s_btn_map[] = {s_ok_label, ""};

static const char *prompt_text(bool vmouse, bool touchpad_mouse) {
    if (vmouse && touchpad_mouse) {
        return locstr("Warning: Binding conflicts will occur with Virtual Mouse on and the touchpad sent as a mouse. "
                      "It is recommended to turn both off for bridged controllers.\n\n"
                      "Turn off both?");
    }
    if (vmouse) {
        return locstr("Warning: Binding conflicts will occur with Virtual Mouse on. "
                      "It is recommended to turn off Virtual Mouse for bridged controllers.\n\n"
                      "Turn off Virtual Mouse?");
    }
    return locstr("Warning: Binding conflicts will occur with the touchpad sent as a mouse. "
                  "It is recommended to turn that off for bridged controllers.\n\n"
                  "Turn off the touchpad's mouse mode?");
}

static void show_count(void) {
    snprintf(s_ok_label, sizeof(s_ok_label), "%s (%d)", locstr("OK"), s_seconds);
    if (s_mbox != NULL) {
        lv_obj_t *btns = lv_msgbox_get_btns(s_mbox);
        if (btns != NULL) {
            lv_obj_invalidate(btns);
        }
    }
}

/* Whatever closed it, this is where it ends. */
static void on_deleted(lv_event_t *event) {
    if (lv_event_get_target(event) != s_mbox) {
        return;
    }
    commons_log_info("Streaming", "bridge prompt: closed, %s", s_outcome != NULL ? s_outcome : "dismissed");
    s_mbox = NULL;
    s_outcome = NULL;
    s_seconds = 0;
    if (s_timer != NULL) {
        lv_timer_del(s_timer);
        s_timer = NULL;
    }
    if (s_released_grab) {
        s_released_grab = false;
        /* ⓘ Back to the stream, unless something else of the interface's has
         * come up since and wants the pointer. */
        if (global != NULL && global->session != NULL && !streaming_overlay_shown() &&
            !streaming_soft_keyboard_shown()) {
            app_set_mouse_grab(&global->input, true);
        }
    }
}

static void switch_off(const char *by) {
    commons_log_info("Streaming", "bridge prompt: OK, %s -- the TV's mouse controls go off", by);
    bridge_override_set(global != NULL ? global->session : NULL, true);
    /* Says what went off, by the names the Input settings use. */
    streaming_bridge_override_changed();
}

static void on_button(lv_event_t *event) {
    lv_obj_t *mbox = lv_event_get_current_target(event);
    if (mbox != s_mbox || s_outcome != NULL) {
        return;
    }
    s_outcome = "OK pressed";
    switch_off("pressed");
    /* ⓘ Not deleted inside its own event: the box is closed on the next pass. */
    lv_msgbox_close_async(mbox);
}

static void on_second(lv_timer_t *timer) {
    (void) timer;
    if (s_mbox == NULL || s_outcome != NULL) {
        return;
    }
    if (--s_seconds <= 0) {
        s_outcome = "the count ran out, nothing changed";
        lv_msgbox_close(s_mbox);
        return;
    }
    show_count();
}

void bridge_prompt_request(void) {
    if (global == NULL || global->session == NULL) {
        commons_log_info("Streaming", "bridge prompt: no stream");
        return;
    }
    if (bridge_override_active()) {
        commons_log_info("Streaming", "bridge prompt: the TV's mouse controls are off already, nothing to ask");
        return;
    }
    stream_input_t *input = session_get_input(global->session);
    const bool vmouse = session_vmouse_active(global->session);
    const bool touchpad_mouse = input != NULL && input->touchpad_mode == TOUCHPAD_MODE_MOUSE;
    if (!vmouse && !touchpad_mouse) {
        commons_log_info("Streaming", "bridge prompt: Virtual Mouse and the touchpad's mouse mode are both off, "
                                      "nothing to ask");
        return;
    }
    const char *text = prompt_text(vmouse, touchpad_mouse);
    s_seconds = PROMPT_SECONDS;
    if (s_mbox != NULL) {
        /* Another controller bridged while it is up: one question, not two. */
        if (s_outcome == NULL) {
            lv_label_set_text(lv_msgbox_get_text(s_mbox), text);
            show_count();
            commons_log_info("Streaming", "bridge prompt: already up, the count starts again");
        }
        return;
    }
    commons_log_info("Streaming", "bridge prompt: asking for %d s (virtual mouse %s, touchpad mouse %s)",
                     PROMPT_SECONDS, vmouse ? "on" : "off", touchpad_mouse ? "on" : "off");
    show_count();
    s_outcome = NULL;
    s_mbox = lv_msgbox_create(NULL, NULL, text, s_btn_map, false);
    lv_obj_add_event_cb(s_mbox, on_button, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(s_mbox, on_deleted, LV_EVENT_DELETE, NULL);
    lv_obj_center(s_mbox);
    /* The one button is the answer, so it is selected from the start: a press
     * of Cross, OK or Enter takes it without an arrow first. */
    lv_obj_t *btns = lv_msgbox_get_btns(s_mbox);
    if (btns != NULL) {
        lv_btnmatrix_set_selected_btn(btns, 0);
    }
    s_timer = lv_timer_create(on_second, 1000, NULL);
    /* A pointer has to be able to press it. The overlay and the on-screen
     * keyboard let the grab go themselves, so only take it from the stream. */
    if (!streaming_overlay_shown() && !streaming_soft_keyboard_shown()) {
        app_set_mouse_grab(&global->input, false);
        s_released_grab = true;
    }
}

bool bridge_prompt_shown(void) {
    return s_mbox != NULL;
}

void bridge_prompt_dismiss(void) {
    if (s_mbox == NULL || s_outcome != NULL) {
        return;
    }
    s_outcome = "dismissed, nothing changed";
    lv_msgbox_close(s_mbox);
}

int bridge_prompt_seconds_left(void) {
    return s_mbox != NULL && s_outcome == NULL ? s_seconds : 0;
}

bool bridge_prompt_accept(void) {
    if (s_mbox == NULL || s_outcome != NULL) {
        return false;
    }
    s_outcome = "OK from the control port";
    switch_off("from the control port");
    lv_msgbox_close(s_mbox);
    return true;
}
