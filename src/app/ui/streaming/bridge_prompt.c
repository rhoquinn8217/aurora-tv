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

/* How far it sits from the left of the picture: in the corner, and clear of the
 * edge. 60 pixels of a 1920-wide picture. ⓘ Also how far from the bottom, when
 * there is no row of buttons to sit above. */
#define PROMPT_MARGIN LV_DPX(30)

/* ⭐ IT SITS ABOVE THE OVERLAY'S ROW OF BUTTONS, by this much (rhoquinn8217,
 * 2026-10-01: a little higher, to clear them). A bridge from the USB Bridge
 * panel raises it with the overlay open, and at the margin alone it lay across
 * the top of Full keyboard, Virtual Mouse and the rest. ⓘ The same place
 * whether the overlay is open or not, so it is found where it was last time. */
#define PROMPT_CLEAR LV_DPX(10)

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

/* ⭐ THE MESSAGE AFTER THE BUTTON (rhoquinn8217, 2026-10-01): pressing the
 * button is answered, in the pop-up's own place, with what went off and where
 * to bring it back. Only the button raises it: the count running out and a
 * dismissal change nothing, so they say nothing.
 * ⓘ It stays this long, the time the app's other timed notice has for two
 * sentences, and takes no input: the game has the controller back at once. */
#define PROMPT_NOTE_MS 7000
/* How often it asks whether it is still true. */
#define PROMPT_NOTE_TICK_MS 250

/* What it says, for whichever of the TV's mouse controls is on. */
typedef struct {
    const char *warning;
    /* The button, before its count. It names what pressing it does. */
    const char *action;
    /* The message once the button has been pressed. */
    const char *done;
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

/* The message after the button, for the words the pop-up is showing. */
static const char *s_done = "";
static lv_obj_t *s_note = NULL;
static lv_timer_t *s_note_timer = NULL;
static uint32_t s_note_since = 0;

static prompt_words_t prompt_words(bool vmouse, bool touchpad_mouse) {
    /* ⓘ Virtual Mouse in the overlay is the way back for all three: pressing
     * it puts every one of the TV's mouse controls back as the Input settings
     * have them, and Virtual Mouse on. */
    if (vmouse && touchpad_mouse) {
        return (prompt_words_t) {
                locstr("Warning: Turn off Virtual Mouse and the touchpad's mouse mode to prevent binding conflicts "
                       "with bridged controllers."),
                locstr("Turn off both"),
                locstr("Virtual Mouse and the touchpad's mouse mode have turned off. "
                       "Toggle Virtual Mouse on in the streaming overlay to bring both back.")};
    }
    if (vmouse) {
        return (prompt_words_t) {
                locstr("Warning: Turn off Virtual Mouse to prevent binding conflicts with bridged controllers."),
                locstr("Turn off Virtual Mouse"),
                locstr("Virtual Mouse has turned off. Toggle it back on in the streaming overlay.")};
    }
    return (prompt_words_t) {
            locstr("Warning: Turn off the touchpad's mouse mode to prevent binding conflicts with bridged "
                   "controllers."),
            locstr("Turn off touchpad mouse mode"),
            locstr("The touchpad's mouse mode has turned off. "
                   "Toggle Virtual Mouse on in the streaming overlay to bring it back.")};
}

/* The top of the overlay's row of buttons, in pixels down the picture, read
 * off the buttons themselves so that it follows whatever that row becomes.
 * ⓘ The overlay is laid out whether or not it is shown, so this answers the
 * same either way. -1 when there is no streaming screen to ask. */
static lv_coord_t overlay_buttons_top(void) {
    lv_fragment_t *top = global != NULL && global->ui.fm != NULL ? lv_fragment_manager_get_top(global->ui.fm) : NULL;
    if (top == NULL || top->cls != &streaming_controller_class) {
        return -1;
    }
    const streaming_controller_t *controller = (const streaming_controller_t *) top;
    if (controller->actions == NULL) {
        return -1;
    }
    lv_obj_update_layout(controller->actions);
    lv_coord_t row_top = -1;
    const uint32_t count = lv_obj_get_child_cnt(controller->actions);
    for (uint32_t i = 0; i < count; i++) {
        const lv_obj_t *child = lv_obj_get_child(controller->actions, (int32_t) i);
        /* ⓘ Buttons only: the row also holds a spacer, which is not one, and
         * buttons that are hidden with device bridging switched off. */
        if (!lv_obj_check_type(child, &lv_btn_class) || lv_obj_has_flag(child, LV_OBJ_FLAG_HIDDEN)) {
            continue;
        }
        lv_area_t area;
        lv_obj_get_coords(child, &area);
        if (row_top < 0 || area.y1 < row_top) {
            row_top = area.y1;
        }
    }
    return row_top;
}

/* How far its bottom edge sits above the bottom of the picture. */
static lv_coord_t prompt_lift(void) {
    const lv_coord_t screen = lv_disp_get_ver_res(NULL);
    const lv_coord_t buttons = overlay_buttons_top();
    /* ⛔ Only a row that is where a row along the bottom would be. A layout
     * not worked out yet reads as zero, and trusting that would send the
     * pop-up off the top of the picture. */
    if (buttons > screen / 2 && buttons < screen) {
        return screen - buttons + PROMPT_CLEAR;
    }
    return PROMPT_MARGIN;
}

/* How wide some words come out on one line. */
static lv_coord_t line_width(const char *text, const lv_font_t *font, lv_coord_t letter_space) {
    lv_point_t size;
    lv_txt_get_size(&size, text, font, letter_space, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    return size.x;
}

static void note_remove(void) {
    if (s_note_timer != NULL) {
        lv_timer_del(s_note_timer);
        s_note_timer = NULL;
    }
    if (s_note != NULL) {
        lv_obj_del(s_note);
        s_note = NULL;
        commons_log_info("Streaming", "bridge prompt: the message after the button is gone");
    }
}

/* ⓘ It is on screen only while what it says is true. Virtual Mouse pressed in
 * the overlay brings the TV's controls back, and takes this down with them; so
 * does the stream ending. */
static void note_tick(lv_timer_t *timer) {
    (void) timer;
    if (lv_tick_elaps(s_note_since) >= PROMPT_NOTE_MS || !bridge_override_active() || global == NULL ||
        global->session == NULL) {
        note_remove();
    }
}

/* The message after the button: the pop-up's box without a button, where the
 * pop-up was.
 *
 * ⭐ THE SAME LOOK WITH NO DIM BEHIND IT. The pop-up is half see-through over a
 * picture that a dialogue's backdrop has already darkened. This takes no
 * input, so it has no backdrop, and at the pop-up's own strength it would lie
 * on the picture at full brightness, where a notice at 40% could not be read
 * (rhoquinn8217, 2026-09-19). So the two layers are worked into one: it is
 * given the backdrop's share as well as its own, and comes out as dark and as
 * see-through as the pop-up looked.
 * ⓘ On the system layer, like the app's other notices: nothing else that comes
 * up can lie over it, and a dialogue's dim does not grey it. */
static void note_show(const char *text, lv_opa_t dim_opa, lv_color_t dim_color) {
    note_remove();
    if (text == NULL || text[0] == '\0') {
        return;
    }
    lv_obj_t *note = lv_obj_create(lv_layer_sys());
    lv_obj_clear_flag(note, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_pad_all(note, PROMPT_PAD, 0);
    lv_obj_set_style_max_width(note, lv_disp_get_hor_res(NULL) - 2 * PROMPT_MARGIN, 0);
    /* Of what shows, how much is the backdrop's colour and how much the
     * box's own; the rest is the picture. */
    const uint32_t dim_share = (uint32_t) dim_opa * (255 - PROMPT_BG_OPA) / 255;
    const uint32_t box_share = PROMPT_BG_OPA;
    const lv_color_t box_color = lv_obj_get_style_bg_color(note, LV_PART_MAIN);
    lv_obj_set_style_bg_color(note, lv_color_mix(box_color, dim_color,
                                                 (uint8_t) (box_share * 255 / (box_share + dim_share))), 0);
    lv_obj_set_style_bg_opa(note, (lv_opa_t) LV_MIN(box_share + dim_share, 255), 0);

    lv_obj_t *label = lv_label_create(note);
    const lv_font_t *font = lv_theme_get_font_small(note);
    lv_obj_set_style_text_font(label, font, 0);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(label, LV_PCT(100));
    lv_label_set_text(label, text);
    /* As wide as its words on one line, as the pop-up is. */
    const lv_coord_t edges = 2 * PROMPT_PAD + 2 * lv_obj_get_style_border_width(note, LV_PART_MAIN);
    lv_obj_set_size(note, line_width(text, font, lv_obj_get_style_text_letter_space(label, LV_PART_MAIN)) +
                          edges + PROMPT_SLACK, LV_SIZE_CONTENT);
    /* Its lower left corner where the pop-up's was. */
    lv_obj_align(note, LV_ALIGN_BOTTOM_LEFT, PROMPT_MARGIN, -prompt_lift());

    s_note = note;
    s_note_since = lv_tick_get();
    s_note_timer = lv_timer_create(note_tick, PROMPT_NOTE_TICK_MS, NULL);
    commons_log_info("Streaming", "bridge prompt: the message after the button is up for %d ms", PROMPT_NOTE_MS);
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
    s_done = words->done;
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

/* The button was pressed. ⚠️ Call it while the pop-up still stands: the message
 * that follows takes its look from the pop-up and its backdrop. */
static void switch_off(const char *by) {
    commons_log_info("Streaming", "bridge prompt: accepted, %s -- the TV's mouse controls go off", by);
    bridge_override_set(global != NULL ? global->session : NULL, true);
    /* ⓘ Said only if it is so: the switch does not act with device bridging
     * off, though nothing bridges then to raise the pop-up either. */
    if (!bridge_override_active() || s_mbox == NULL) {
        return;
    }
    const lv_obj_t *backdrop = lv_obj_get_parent(s_mbox);
    note_show(s_done, lv_obj_get_style_bg_opa(backdrop, LV_PART_MAIN),
              lv_obj_get_style_bg_color(backdrop, LV_PART_MAIN));
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
    /* ⓘ The message a button left behind is in this same place, and what it
     * says stopped being true when the controls came back on. */
    note_remove();
    s_action = words.action;
    show_count();
    s_outcome = NULL;
    s_mbox = lv_msgbox_create(NULL, NULL, words.warning, s_btn_map, false);
    lv_obj_add_event_cb(s_mbox, on_button, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(s_mbox, on_deleted, LV_EVENT_DELETE, NULL);
    make_compact(s_mbox);
    set_words(s_mbox, &words);
    /* Bottom left, off the middle of the picture, and above the overlay's row
     * of buttons (rhoquinn8217, 2026-10-01). */
    lv_obj_align(s_mbox, LV_ALIGN_BOTTOM_LEFT, PROMPT_MARGIN, -prompt_lift());
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

void bridge_prompt_stream_ended(void) {
    bridge_prompt_dismiss();
    note_remove();
}

static void measure(const lv_obj_t *box, const lv_obj_t *label, bridge_prompt_measure_t *out) {
    /* ⓘ Laid out now rather than at the next redraw, so a reading taken the
     * moment it is raised is of the box as it will be drawn. */
    lv_obj_update_layout(box);
    lv_area_t area;
    lv_obj_get_coords(box, &area);
    out->x = area.x1;
    out->y = area.y1;
    out->width = lv_area_get_width(&area);
    out->height = lv_area_get_height(&area);
    out->screen_width = lv_disp_get_hor_res(NULL);
    out->screen_height = lv_disp_get_ver_res(NULL);
    out->buttons_top = overlay_buttons_top();
    out->text_lines = 0;
    if (label != NULL) {
        const lv_coord_t space = lv_obj_get_style_text_line_space(label, LV_PART_MAIN);
        const lv_coord_t line = lv_font_get_line_height(lv_obj_get_style_text_font(label, LV_PART_MAIN)) + space;
        if (line > 0) {
            out->text_lines = (lv_obj_get_height(label) + space) / line;
        }
    }
}

bool bridge_prompt_measure(bridge_prompt_measure_t *out) {
    if (s_mbox == NULL || out == NULL) {
        return false;
    }
    measure(s_mbox, lv_msgbox_get_text(s_mbox), out);
    return true;
}

int bridge_prompt_note_measure(bridge_prompt_measure_t *out) {
    if (s_note == NULL || out == NULL) {
        return -1;
    }
    measure(s_note, lv_obj_get_child(s_note, 0), out);
    const uint32_t shown = lv_tick_elaps(s_note_since);
    return shown >= PROMPT_NOTE_MS ? 0 : (int) (PROMPT_NOTE_MS - shown);
}
