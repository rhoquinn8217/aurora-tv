/* The stream's two pop-ups and two messages: Desktop Mode's warning at a bridge, a
 * refusal that offers Desktop Mode, and the messages in their place. The why is in the
 * header. */

#include "bridge_prompt.h"

#include <stdio.h>

#include "lvgl.h"

#include "app.h"            /* global */
#include "logging.h"
#include "input/app_input.h"
#include "stream/session.h"
#include "stream/session_priv.h"
#include "stream/input/lock_keys.h"
#include "streaming.controller.h"
#include "cursor_align.h"
#include "desktop_mode.h"
#include "util/i18n.h"
#if defined(TARGET_WEBOS)
#include "ctm_bridge_glue.h"   /* whether DS5-USBIP answers */
#endif

/* ⭐⭐ TWO POP-UPS AND TWO MESSAGES, AND NO OTHERS (rhoquinn8217, 2026-11-12, shown all
 * eighteen: "we may have to many or they have an inconsistent structure", then "Get rid
 * of all of these and one have this one", "Use only these two", and "you created too
 * many messages about the cursor doesn't need explaining"). Gone since: Virtual Mouse's
 * and the TV cursor's warnings at a bridge, Desktop Mode's questions at a stream's start,
 * the refusal at a lock, and every message but the two below. Each pop-up has the count
 * on its second button, which is what a count running out leaves, and that button
 * selected, so an accidental press changes nothing. */
/* Desktop Mode's warning when a controller is bridged ("Leave it on(10)"). */
#define PROMPT_SECONDS 10
/* The refusal's question ("Yes No(5)"). */
#define PROMPT_REFUSAL_SECONDS 5

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
/* The room either side of a button's words. */
#define PROMPT_BUTTON_PAD LV_DPX(12)
/* ⓘ Pixels over the sentence's own width, so that no rounding in the layout
 * can push its last word onto a second line. */
#define PROMPT_SLACK 2

/* ⓘ A message lasts four seconds, as every message that asks for no choice
 * (rhoquinn8217, 2026-11-01: "7 seconds is too long"), and takes no input: the
 * game has the controller back at once. */
#define PROMPT_NOTE_MS 4000
/* How often it looks whether its time or the stream is up. */
#define PROMPT_NOTE_TICK_MS 250

/* ⭐ TWO BUTTONS, THE FIRST DOING SOMETHING AND THE SECOND NOTHING, WITH THE COUNT
 * ON THE SECOND (rhoquinn8217, 2026-10-01, and their words for both pop-ups,
 * 2026-11-12). With one button, nothing on the screen said how to say no, and its
 * count sat on the one thing that does NOT happen at zero. ⓘ The second is
 * selected to start (the header); Left and Right move between them, and the
 * library's button row does that. */
enum {
    PROMPT_BUTTON_FIRST = 0,
    PROMPT_BUTTON_SECOND = 1,
};

/* What a pop-up says. */
typedef struct {
    const char *text;
    /* The first button: what pressing it does. */
    const char *first;
    /* The second button, before its count. */
    const char *second;
} prompt_words_t;

/* ⭐ WHAT IT ASKS ABOUT. One pop-up at a time, in one place, with one count. */
enum {
    PROMPT_ABOUT_DESKTOP_WARNING = 0,   /* a controller bridged with Desktop Mode on */
    PROMPT_ABOUT_REFUSAL,               /* a bridge refused, DS5-USBIP offline: Desktop Mode offered */
};

static lv_obj_t *s_mbox = NULL;
static int s_about = PROMPT_ABOUT_DESKTOP_WARNING;
static lv_timer_t *s_timer = NULL;
static int s_seconds = 0;
/* What the count started from, for the widest the second button gets. */
static int s_seconds_full = PROMPT_SECONDS;
/* Why it is closing, for the log: set by whoever closes it, and "dismissed"
 * when the theme's own Back handling does. */
static const char *s_outcome = NULL;
/* The stream's mouse grab was let go so a pointer can press a button. */
static bool s_released_grab = false;

/* The buttons' words are drawn from here every time, so the count is changed
 * in place and the row redrawn; the map itself is never set again. */
static const char *s_second = "";
static char s_second_label[96];
static const char *s_btn_map[] = {"", s_second_label, ""};

/* The message in the pop-up's place. */
static lv_obj_t *s_note = NULL;
static lv_timer_t *s_note_timer = NULL;
static uint32_t s_note_since = 0;
/* Which message is up, for the log. */
static const char *s_note_what = "";

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
        commons_log_info("Streaming", "bridge prompt: %s is gone", s_note_what);
    }
}

/* ⓘ Its four seconds, or the stream ending, take it down. */
static void note_tick(lv_timer_t *timer) {
    (void) timer;
    if (lv_tick_elaps(s_note_since) >= PROMPT_NOTE_MS || global == NULL || global->session == NULL) {
        note_remove();
    }
}

/* The dim the theme puts behind a dialogue, for a message with no dialogue
 * under it: read off a backdrop made for the purpose inside `parent`, so that
 * nothing outside it is redrawn, and deleted at once. ⓘ The theme gives every
 * dialogue's backdrop the same. */
static void dialogue_dim(lv_obj_t *parent, lv_opa_t *opa, lv_color_t *color) {
    lv_obj_t *backdrop = lv_obj_class_create_obj(&lv_msgbox_backdrop_class, parent);
    lv_obj_class_init_obj(backdrop);
    *opa = lv_obj_get_style_bg_opa(backdrop, LV_PART_MAIN);
    *color = lv_obj_get_style_bg_color(backdrop, LV_PART_MAIN);
    lv_obj_del(backdrop);
}

/* A message: the pop-up's box without a button, where the pop-up is.
 *
 * ⭐ THE SAME LOOK WITH NO DIM BEHIND IT. The pop-up is half see-through over a
 * picture that a dialogue's backdrop has already darkened. This takes no
 * input, so it has no backdrop, and at the pop-up's own strength it would lie
 * on the picture at full brightness, where a notice at 40% could not be read
 * (rhoquinn8217, 2026-09-19). So the two layers are worked into one: it is
 * given the backdrop's share as well as its own, and comes out as dark and as
 * see-through as the pop-up looked.
 * ⓘ On the system layer, like the app's other notices: nothing else that comes
 * up can lie over it, and a dialogue's dim does not grey it.
 *
 * `backdrop` is the dim behind the pop-up it follows; NULL for a message that
 * follows no pop-up, which takes the theme's dim for a dialogue, so that both
 * look the same. `what` names it in the log. */
static void note_show(const char *text, const char *what, const lv_obj_t *backdrop) {
    note_remove();
    if (text == NULL || text[0] == '\0') {
        return;
    }
    lv_obj_t *note = lv_obj_create(lv_layer_sys());
    lv_obj_clear_flag(note, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_opa_t dim_opa;
    lv_color_t dim_color;
    if (backdrop != NULL) {
        dim_opa = lv_obj_get_style_bg_opa(backdrop, LV_PART_MAIN);
        dim_color = lv_obj_get_style_bg_color(backdrop, LV_PART_MAIN);
    } else {
        dialogue_dim(note, &dim_opa, &dim_color);
    }
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
    s_note_what = what;
    s_note_since = lv_tick_get();
    s_note_timer = lv_timer_create(note_tick, PROMPT_NOTE_TICK_MS, NULL);
    commons_log_info("Streaming", "bridge prompt: %s is up for %d ms", what, PROMPT_NOTE_MS);
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

/* Put the words on the box and fit it to them: the box as wide as its sentence
 * on one line, the buttons as wide as their own words. */
static void set_words(lv_obj_t *mbox, const prompt_words_t *words) {
    s_btn_map[PROMPT_BUTTON_FIRST] = words->first;
    s_second = words->second;
    /* The row of buttons first: the box may not be narrower than it. */
    lv_coord_t row_width = 0;
    lv_obj_t *btns = lv_msgbox_get_btns(mbox);
    if (btns != NULL) {
        /* ⓘ The two are the same width, the wider one's words and the room
         * either side: the library shares a row out equally. The count is
         * taken at its widest, so the row keeps one size while it counts
         * down. ⓘ The library's own width for a message box's buttons is a
         * fixed one, which these words do not fit in. */
        const lv_font_t *font = lv_obj_get_style_text_font(btns, LV_PART_ITEMS);
        const lv_coord_t space = lv_obj_get_style_text_letter_space(btns, LV_PART_ITEMS);
        char widest[sizeof(s_second_label)];
        snprintf(widest, sizeof(widest), "%s (%d)", words->second, s_seconds_full);
        const lv_coord_t button = LV_MAX(line_width(words->first, font, space), line_width(widest, font, space)) +
                                  2 * PROMPT_BUTTON_PAD;
        row_width = 2 * button + lv_obj_get_style_pad_column(btns, LV_PART_MAIN) +
                    lv_obj_get_style_pad_left(btns, LV_PART_MAIN) + lv_obj_get_style_pad_right(btns, LV_PART_MAIN);
        lv_obj_set_width(btns, row_width);
        lv_obj_invalidate(btns);
    }
    lv_obj_t *label = lv_msgbox_get_text(mbox);
    if (label != NULL) {
        lv_label_set_text(label, words->text);
        const lv_coord_t edges = lv_obj_get_style_pad_left(mbox, LV_PART_MAIN) +
                                 lv_obj_get_style_pad_right(mbox, LV_PART_MAIN) +
                                 2 * lv_obj_get_style_border_width(mbox, LV_PART_MAIN);
        const lv_coord_t sentence = line_width(words->text, lv_obj_get_style_text_font(label, LV_PART_MAIN),
                                               lv_obj_get_style_text_letter_space(label, LV_PART_MAIN));
        lv_obj_set_width(mbox, LV_MAX(sentence, row_width) + edges + PROMPT_SLACK);
    }
}

static void show_count(void) {
    snprintf(s_second_label, sizeof(s_second_label), "%s (%d)", s_second, s_seconds);
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
         * come up since and wants the pointer. ⛔ And only while the stream
         * still takes input (2026-10-23): the session lives on through
         * "Disconnecting...", after the stream's close has let the pointer
         * go, and a grab taken then stayed in the launcher. */
        if (global != NULL && global->session != NULL && session_has_input(global->session) &&
            !streaming_overlay_shown() && !streaming_soft_keyboard_shown()) {
            app_set_mouse_grab(&global->input, true);
        }
    }
}

/* Does DS5-USBIP answer, so its configs are what Desktop Mode puts aside? */
static bool ds5_usbip_online(void) {
#if defined(TARGET_WEBOS)
    return ctm_bridge_active() && ctm_bridge_agent_probed() && ctm_bridge_agent_online();
#else
    return false;
#endif
}

/* ⭐ THE MESSAGE AS DESKTOP MODE GOES ON, WITH DS5-USBIP ON (rhoquinn8217, 2026-11-12:
 * "Warning: DS5-USBIP configs are disabled when Desktop Mode is on." "Whenever Desktop
 * Mode is turned on and DS5-USBIP is on"), in the pop-up's place for the usual four
 * seconds. `backdrop` as for note_show: the dim of the pop-up it follows, or NULL. Only in
 * a stream. */
static void say_desktop_on(const lv_obj_t *backdrop) {
    if (global == NULL || global->session == NULL || !ds5_usbip_online()) {
        return;
    }
    note_show(locstr("Warning: DS5-USBIP configs are disabled when Desktop Mode is on."),
              "Desktop Mode's message", backdrop);
}

/* The first button was pressed. ⚠️ Call it while the pop-up still stands: a
 * message that follows takes its look from the pop-up and its backdrop. */
static void first_button(const char *by) {
    if (s_about == PROMPT_ABOUT_REFUSAL) {
        commons_log_info("Streaming", "bridge prompt: accepted, %s -- Desktop Mode goes on", by);
        desktop_mode_set(true, "a refusal's pop-up");
        say_desktop_on(s_mbox != NULL ? lv_obj_get_parent(s_mbox) : NULL);
        return;
    }
    commons_log_info("Streaming", "bridge prompt: accepted, %s -- Desktop Mode goes off", by);
    desktop_mode_set(false, "the warning at a bridge");
}

static void on_button(lv_event_t *event) {
    lv_obj_t *mbox = lv_event_get_current_target(event);
    if (mbox != s_mbox || s_outcome != NULL) {
        return;
    }
    /* ⓘ The row sends the number of the button with the event. */
    const uint32_t *sent = lv_event_get_param(event);
    const uint32_t pressed = sent != NULL ? *sent : lv_msgbox_get_active_btn(mbox);
    /* ⛔ Only the first button changes anything. Anything else, a number that
     * is not a button's included, changes nothing. */
    if (pressed == PROMPT_BUTTON_FIRST) {
        s_outcome = "its first button was pressed";
        first_button("its first button was pressed");
    } else {
        s_outcome = "its second button was pressed, nothing changed";
    }
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

/* Up it comes, with these words, about this, counting from `seconds`, with its second
 * button selected. */
static void prompt_raise(const prompt_words_t *words, int about, int seconds) {
    /* ⓘ A message is in this same place. */
    note_remove();
    s_about = about;
    s_seconds_full = seconds;
    s_seconds = seconds;
    /* ⓘ Both buttons have their words before the box is made: the library
     * counts the buttons by their words, and an empty one ends the row. */
    s_btn_map[PROMPT_BUTTON_FIRST] = words->first;
    s_second = words->second;
    show_count();
    s_outcome = NULL;
    s_mbox = lv_msgbox_create(NULL, NULL, words->text, s_btn_map, false);
    lv_obj_add_event_cb(s_mbox, on_button, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(s_mbox, on_deleted, LV_EVENT_DELETE, NULL);
    make_compact(s_mbox);
    set_words(s_mbox, words);
    /* Bottom left, off the middle of the picture, and above the overlay's row
     * of buttons (rhoquinn8217, 2026-10-01). */
    lv_obj_align(s_mbox, LV_ALIGN_BOTTOM_LEFT, PROMPT_MARGIN, -prompt_lift());
    /* ⭐ The second button is selected from the start, so an accidental press of
     * Cross, OK or Enter changes nothing (rhoquinn8217, 2026-11-11: "Halo selector
     * on No when it comes up ... an accidental press should default to no"). ⭐ And
     * it is SHOWN as selected. The highlight is the row's focus-key state, which is
     * set here outright rather than left to how the row came by its focus: with two
     * buttons, a row with no highlight gives no way to know which one Cross is about
     * to press. */
    lv_obj_t *btns = lv_msgbox_get_btns(s_mbox);
    if (btns != NULL) {
        lv_btnmatrix_set_selected_btn(btns, PROMPT_BUTTON_SECOND);
        lv_obj_add_state(btns, LV_STATE_FOCUS_KEY);
    }
    s_timer = lv_timer_create(on_second, 1000, NULL);
    /* A pointer has to be able to press it. The overlay and the on-screen
     * keyboard let the grab go themselves, so only take it from the stream. */
    if (!streaming_overlay_shown() && !streaming_soft_keyboard_shown()) {
        app_set_mouse_grab(&global->input, false);
        s_released_grab = true;
    }
}

/* One pop-up at a time: this one up already, and its count starts again; another, and
 * this one is not raised. True when one is up. */
static bool already_up(int about) {
    if (s_mbox == NULL) {
        return false;
    }
    if (s_about == about && s_outcome == NULL) {
        s_seconds = s_seconds_full;
        show_count();
        commons_log_info("Streaming", "bridge prompt: already up, the count starts again");
    } else {
        commons_log_info("Streaming", "bridge prompt: another pop-up is up, this one is not raised");
    }
    return true;
}

/* ⭐ DESKTOP MODE'S WARNING AT A BRIDGE (rhoquinn8217, 2026-11-12: "when Desktop Mode is
 * on after bridging a controller"), in their words. ⓘ Every path that bridges a
 * controller raises it, Auto Bridge's included (input/ctm_bridge_gesture.c). */
void bridge_prompt_request(void) {
    if (global == NULL || global->session == NULL) {
        commons_log_info("Streaming", "bridge prompt: no stream");
        return;
    }
    if (!desktop_mode_on()) {
        commons_log_info("Streaming", "bridge prompt: Desktop Mode is off, nothing to warn of");
        return;
    }
    /* ⓘ Not while Desktop Mode is being handed over to DS5-USBIP (desktop_mode.h): this
     * bridge is the handover, and Desktop Mode turns off once it is made. */
    if (desktop_mode_handing_over()) {
        commons_log_info("Streaming", "bridge prompt: Desktop Mode is being handed over to DS5-USBIP, no warning");
        return;
    }
    /* ⓘ Not while a lock's or a User Account Control prompt's list has the controls
     * (rhoquinn8217, 2026-11-10, "yes" to skipping the pop-up then): they come before
     * Desktop Mode, and the pop-up only hid their list. */
    const lock_keys_mode_t mode = lock_keys_mode();
    if (mode != LOCK_KEYS_OFF && mode != LOCK_KEYS_DESKTOP) {
        commons_log_info("Streaming", "bridge prompt: a lock's or a prompt's list is up, no warning");
        return;
    }
    if (already_up(PROMPT_ABOUT_DESKTOP_WARNING)) {
        return;
    }
    const prompt_words_t words = {
            locstr("Warning: Desktop Mode is on. DS5-USBIP configs are disabled when Desktop Mode is on."),
            locstr("Turn off Desktop Mode"), locstr("Leave it on")};
    commons_log_info("Streaming", "bridge prompt: Desktop Mode's warning at a bridge, for %d s", PROMPT_SECONDS);
    prompt_raise(&words, PROMPT_ABOUT_DESKTOP_WARNING, PROMPT_SECONDS);
}

void bridge_prompt_say_desktop_on(void) {
    /* ⓘ Not over a pop-up: it is in this same place. */
    if (s_mbox != NULL) {
        return;
    }
    say_desktop_on(NULL);
}

void bridge_prompt_say_ds5usbip_stopped(void) {
    if (global == NULL || global->session == NULL) {
        return;
    }
    commons_log_info("Streaming", "bridge prompt: DS5-USBIP stopped, its devices released");
    bridge_prompt_note(locstr("DS5-USBIP has stopped. All bridged devices have been released."));
}

/* ⭐ Their words, with "until the next stream" (rhoquinn8217, 2026-11-19: "Auto bridge
 * disabled to prevent possible bridge/release loop", and yes to saying how it comes back). */
void bridge_prompt_say_auto_bridge_off(void) {
    if (global == NULL || global->session == NULL) {
        return;
    }
    commons_log_info("Streaming", "bridge prompt: Auto Bridge off until the next stream, a bridge/release loop");
    bridge_prompt_note(locstr("Auto Bridge disabled until the next stream to prevent a possible bridge/release loop."));
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
    first_button("from the control port");
    lv_msgbox_close(s_mbox);
    return true;
}

void bridge_prompt_stream_ended(void) {
    bridge_prompt_dismiss();
    note_remove();
    /* ⓘ And the bar that aligns the TV cursor, which also sits above the
     * stream's screen and outlives it (cursor_align.h): the screen's teardown
     * calls here for this fork's pieces. */
    cursor_align_stream_ended();
}

void bridge_prompt_note(const char *text) {
    /* ⓘ Not while the pop-up is up: it is in this same place. */
    if (s_mbox != NULL) {
        return;
    }
    note_show(text, "a message in its place", NULL);
}

bool bridge_prompt_note_up(void) {
    return s_note != NULL;
}

/* ⭐ A REFUSAL OFFERS DESKTOP MODE (rhoquinn8217, 2026-11-11: "I want to add an option
 * to turn on Desktop Mode when the refusal messages come up"; "Desktop Mode is the
 * solution to first log in after a reboot"), in their words of 2026-11-12 ("refusal on
 * lock screen after reboot or DS5-USBIP is off"). Only with Desktop Mode off, for which
 * the question is meant; a locked Windows says nothing, as its list is up. */
void bridge_prompt_note_bridge_refused(bool locked) {
    if (locked) {
        commons_log_info("Streaming", "bridge prompt: a refusal, Windows is locked: nothing said");
        return;
    }
    if (desktop_mode_on()) {
        commons_log_info("Streaming", "bridge prompt: a refusal, DS5-USBIP is offline, Desktop Mode is on: "
                                      "nothing said");
        return;
    }
    if (global == NULL || global->session == NULL || already_up(PROMPT_ABOUT_REFUSAL)) {
        return;
    }
    const prompt_words_t words = {locstr("Unable to bridge device. DS5-USBIP is offline. Turn on Desktop Mode?"),
                                  locstr("Yes"), locstr("No")};
    commons_log_info("Streaming", "bridge prompt: a refusal, DS5-USBIP is offline: offering Desktop Mode for %d s",
                     PROMPT_REFUSAL_SECONDS);
    prompt_raise(&words, PROMPT_ABOUT_REFUSAL, PROMPT_REFUSAL_SECONDS);
}

const char *bridge_prompt_words(void) {
    if (s_mbox == NULL) {
        return "";
    }
    lv_obj_t *label = lv_msgbox_get_text(s_mbox);
    return label != NULL ? lv_label_get_text(label) : "";
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
    out->row_width = 0;
    out->selected = -1;
    out->highlighted = false;
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
    lv_obj_t *btns = lv_msgbox_get_btns(s_mbox);
    if (btns != NULL) {
        out->row_width = lv_obj_get_width(btns);
        const uint16_t selected = lv_btnmatrix_get_selected_btn(btns);
        out->selected = selected == LV_BTNMATRIX_BTN_NONE ? -1 : (int) selected;
        out->highlighted = lv_obj_has_state(btns, LV_STATE_FOCUS_KEY);
    }
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

const char *bridge_prompt_note_words(void) {
    return s_note != NULL ? lv_label_get_text(lv_obj_get_child(s_note, 0)) : "";
}
