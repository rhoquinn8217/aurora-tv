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

/* How far it sits from the left and the bottom of the picture: in the corner,
 * and clear of both edges. 60 pixels of a 1080-line picture. */
#define PROMPT_MARGIN LV_DPX(30)

/* ⭐ WIDE, SHORT AND SEE-THROUGH (rhoquinn8217, 2026-10-01): it shares the
 * picture with a game, so it is a strip along the bottom, not a block out of
 * the corner. The warning is one sentence, and the box is made exactly as wide
 * as that sentence on one line; its height is then a line of text, the button
 * and the padding.
 * - the small type the notices use, where the theme gives the normal one
 * - half the theme's padding, and a smaller gap above the button
 * - the background at half strength, so the picture shows through it
 * - never wider than the picture less the margin on both sides: a sentence
 *   longer than that wraps, and the box is a line taller instead
 * ⓘ What it measures on a set is on the control port: `prompt` gives its size
 * and where it sits. */
#define PROMPT_PAD LV_DPX(12)
#define PROMPT_GAP LV_DPX(8)
#define PROMPT_BG_OPA LV_OPA_50
/* The room either side of the button's words. */
#define PROMPT_BUTTON_PAD LV_DPX(12)
/* ⓘ Pixels over the sentence's own width, so that no rounding in the layout
 * can push its last word onto a second line. */
#define PROMPT_SLACK 2

/* What it says, for whichever of the TV's mouse controls is on. */
typedef struct {
    const char *warning;
    /* The button, before its count. It names what pressing it does. */
    const char *action;
} prompt_words_t;

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
static const char *s_action = "";
static char s_button_label[96];
static const char *s_btn_map[] = {s_button_label, ""};

static prompt_words_t prompt_words(bool vmouse, bool touchpad_mouse) {
    if (vmouse && touchpad_mouse) {
        return (prompt_words_t) {
                locstr("Warning: Turn off Virtual Mouse and the touchpad's mouse mode to prevent binding conflicts "
                       "with bridged controllers."),
                locstr("Turn off both")};
    }
    if (vmouse) {
        return (prompt_words_t) {
                locstr("Warning: Turn off Virtual Mouse to prevent binding conflicts with bridged controllers."),
                locstr("Turn off Virtual Mouse")};
    }
    return (prompt_words_t) {
            locstr("Warning: Turn off the touchpad's mouse mode to prevent binding conflicts with bridged "
                   "controllers."),
            locstr("Turn off touchpad mouse mode")};
}

/* How wide some words come out on one line. */
static lv_coord_t line_width(const char *text, const lv_font_t *font, lv_coord_t letter_space) {
    lv_point_t size;
    lv_txt_get_size(&size, text, font, letter_space, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    return size.x;
}

/* The theme's message box, made compact and see-through. ⓘ Everything here is
 * a style of this one box: the theme and the app's other dialogues are as they
 * were. */
static void make_compact(lv_obj_t *mbox) {
    /* ⛔ The theme puts a least width of 40% and a greatest of 60% on every
     * message box, as styles of the box's own. Both have to be replaced, or a
     * width outside them is ignored. */
    lv_obj_set_style_min_width(mbox, 0, 0);
    lv_obj_set_style_max_width(mbox, lv_disp_get_hor_res(NULL) - 2 * PROMPT_MARGIN, 0);
    lv_obj_set_style_pad_all(mbox, PROMPT_PAD, 0);
    lv_obj_set_style_pad_row(mbox, PROMPT_GAP, 0);
    /* ⓘ The border stays solid, so the box keeps an edge over any picture,
     * and so does the button. */
    lv_obj_set_style_bg_opa(mbox, PROMPT_BG_OPA, 0);
    lv_obj_t *label = lv_msgbox_get_text(mbox);
    if (label != NULL) {
        lv_obj_set_style_text_font(label, lv_theme_get_font_small(mbox), 0);
    }
}

/* Put the words on the box and fit it to them: the box as wide as the warning
 * on one line, the button as wide as its own words. For when it is made, and
 * again if a second bridge finds a different control on. */
static void set_words(lv_obj_t *mbox, const prompt_words_t *words) {
    s_action = words->action;
    lv_obj_t *label = lv_msgbox_get_text(mbox);
    if (label != NULL) {
        lv_label_set_text(label, words->warning);
        const lv_coord_t edges = lv_obj_get_style_pad_left(mbox, LV_PART_MAIN) +
                                 lv_obj_get_style_pad_right(mbox, LV_PART_MAIN) +
                                 2 * lv_obj_get_style_border_width(mbox, LV_PART_MAIN);
        lv_obj_set_width(mbox, line_width(words->warning, lv_obj_get_style_text_font(label, LV_PART_MAIN),
                                          lv_obj_get_style_text_letter_space(label, LV_PART_MAIN)) +
                               edges + PROMPT_SLACK);
    }
    lv_obj_t *btns = lv_msgbox_get_btns(mbox);
    if (btns != NULL) {
        /* ⓘ For the count at its widest, so the button keeps one size while
         * it counts down. The library gives a message box's button a fixed
         * width, which these words do not fit in. */
        char widest[sizeof(s_button_label)];
        snprintf(widest, sizeof(widest), "%s (%d)", words->action, PROMPT_SECONDS);
        lv_obj_set_width(btns, line_width(widest, lv_obj_get_style_text_font(btns, LV_PART_ITEMS),
                                          lv_obj_get_style_text_letter_space(btns, LV_PART_ITEMS)) +
                               2 * PROMPT_BUTTON_PAD +
                               lv_obj_get_style_pad_left(btns, LV_PART_MAIN) +
                               lv_obj_get_style_pad_right(btns, LV_PART_MAIN));
    }
}

static void show_count(void) {
    snprintf(s_button_label, sizeof(s_button_label), "%s (%d)", s_action, s_seconds);
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
    commons_log_info("Streaming", "bridge prompt: accepted, %s -- the TV's mouse controls go off", by);
    /* ⓘ Nothing is shown afterwards: the pop-up closing is the answer. */
    bridge_override_set(global != NULL ? global->session : NULL, true);
}

static void on_button(lv_event_t *event) {
    lv_obj_t *mbox = lv_event_get_current_target(event);
    if (mbox != s_mbox || s_outcome != NULL) {
        return;
    }
    s_outcome = "the button was pressed";
    switch_off("the button was pressed");
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
    const prompt_words_t words = prompt_words(vmouse, touchpad_mouse);
    s_seconds = PROMPT_SECONDS;
    if (s_mbox != NULL) {
        /* Another controller bridged while it is up: one pop-up, not two. */
        if (s_outcome == NULL) {
            set_words(s_mbox, &words);
            show_count();
            commons_log_info("Streaming", "bridge prompt: already up, the count starts again");
        }
        return;
    }
    commons_log_info("Streaming", "bridge prompt: asking for %d s (virtual mouse %s, touchpad mouse %s)",
                     PROMPT_SECONDS, vmouse ? "on" : "off", touchpad_mouse ? "on" : "off");
    s_action = words.action;
    show_count();
    s_outcome = NULL;
    s_mbox = lv_msgbox_create(NULL, NULL, words.warning, s_btn_map, false);
    lv_obj_add_event_cb(s_mbox, on_button, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(s_mbox, on_deleted, LV_EVENT_DELETE, NULL);
    make_compact(s_mbox);
    set_words(s_mbox, &words);
    /* Bottom left, off the middle of the picture (rhoquinn8217, 2026-10-01). */
    lv_obj_align(s_mbox, LV_ALIGN_BOTTOM_LEFT, PROMPT_MARGIN, -PROMPT_MARGIN);
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
    s_outcome = "accepted from the control port";
    switch_off("from the control port");
    lv_msgbox_close(s_mbox);
    return true;
}

bool bridge_prompt_measure(bridge_prompt_measure_t *out) {
    if (s_mbox == NULL || out == NULL) {
        return false;
    }
    /* ⓘ Laid out now rather than at the next redraw, so a reading taken the
     * moment it is raised is of the box as it will be drawn. */
    lv_obj_update_layout(s_mbox);
    lv_area_t box;
    lv_obj_get_coords(s_mbox, &box);
    out->x = box.x1;
    out->y = box.y1;
    out->width = lv_area_get_width(&box);
    out->height = lv_area_get_height(&box);
    out->screen_width = lv_disp_get_hor_res(NULL);
    out->screen_height = lv_disp_get_ver_res(NULL);
    out->text_lines = 0;
    lv_obj_t *label = lv_msgbox_get_text(s_mbox);
    if (label != NULL) {
        const lv_font_t *font = lv_obj_get_style_text_font(label, LV_PART_MAIN);
        const lv_coord_t line = lv_font_get_line_height(font) + lv_obj_get_style_text_line_space(label, LV_PART_MAIN);
        if (line > 0) {
            out->text_lines = (lv_obj_get_height(label) + lv_obj_get_style_text_line_space(label, LV_PART_MAIN)) / line;
        }
    }
    return true;
}
