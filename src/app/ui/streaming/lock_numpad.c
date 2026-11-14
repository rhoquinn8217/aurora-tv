/* What a controller does over a locked Windows, on the screen. The why is in the
 * header. */

#include "lock_numpad.h"

#include <stdint.h>

#include "util/font.h"   /* app_fonts_t, which the theme's header names */
#include "lvgl/theme/lv_theme_moonlight.h"
#include "stream/input/lock_keys.h"
#include "ui/common/bt_panel.h"
#include "ui/common/pad_marks.h"
#include "ui/root.h"
#include "ui/streaming/ctm_panel.h"
#include "ui/streaming/desktop_mode.h"
#include "ui/streaming/streaming.controller.h"
#include "util/i18n.h"
#if defined(TARGET_WEBOS)
#include "input/auto_bridge.h"
#include "input/ctm_bridge_gesture.h"
#endif

/* The keyboard's own colours (soft_keyboard.c), as this gives way to it. */
#define NUMPAD_PANEL_BG lv_color_hex(0x1B1B1B)
#define NUMPAD_KEY_BG lv_color_hex(0x2C2C2C)
#define NUMPAD_KEY_BORDER lv_color_hex(0x3A3A3A)
#define NUMPAD_TEXT lv_color_hex(0xFFFFFF)
#define NUMPAD_MARK lv_color_hex(0xE8E8E8)

/* ⓘ LV_DPX(n) is 2n pixels on a 1920-wide interface. */
#define NUMPAD_MARGIN_X LV_DPX(30)
#define NUMPAD_MARGIN_Y LV_DPX(20)
#define NUMPAD_PAD LV_DPX(10)
#define NUMPAD_GAP LV_DPX(5)
#define NUMPAD_KEY_W LV_DPX(64)
#define NUMPAD_KEY_H LV_DPX(42)
#define NUMPAD_KEY_MARK LV_DPX(19)
#define NUMPAD_KEY_INSET LV_DPX(3)
#define NUMPAD_LEGEND_MARK LV_DPX(18)
/* The sticks' marks a fifth larger than the rest ("make them a little bigger"). */
#define NUMPAD_STICK_MARK (NUMPAD_LEGEND_MARK * 6 / 5)
/* A category's name, dimmer than the rows under it. */
#define NUMPAD_CAT lv_color_hex(0x9FB3C8)
#define NUMPAD_LIST_GAP LV_DPX(3)
/* A digit sits a little left of the middle and below it: the mark has the corner.
 * A word is wider, so it sits along the foot of the key, under the mark. */
#define NUMPAD_DIGIT_LEFT LV_DPX(10)
#define NUMPAD_DIGIT_DROP LV_DPX(5)
#define NUMPAD_WORD_LIFT LV_DPX(3)
/* ⭐ WHERE THE NUMPAD GOES, in thousandths of the screen, read off their photo of the
 * lock screen on a 4K stream (2026-11-01). The PIN box ends 599 across, so this starts
 * a little past it; and it starts level with the top of the picture of the user, 234
 * down, which puts its foot about level with "Sign-in options". */
#define NUMPAD_AT_X 625
#define NUMPAD_AT_Y 234
/* Where the line about Auto Bridge goes: under the list, NUMPAD_AFTER_GAP below its
 * foot, and it stays there while the numpad is open, so that it does not move.
 * ⭐ MEASURED OFF THE LIST, not a constant (rhoquinn8217, 2026-11-10: "the rebind
 * legend with the added create button symbol increased the box area and is now
 * touching the message below it. Push the message down."): a constant sat under a
 * list of twelve rows, and the thirteenth, Create - Tab, ran into it. ⓘ The constant
 * stays for a lock with no list up yet, one row lower than it was, and one more for
 * the Options row (2026-11-10). */
#define NUMPAD_AFTER_GAP LV_DPX(10)
#define NUMPAD_AFTER_Y (NUMPAD_MARGIN_Y + LV_DPX(327))

/* ⭐ CLOSE, AT THE RIGHT OF THE LIST'S HEADING (rhoquinn8217, 2026-11-10, at build
 * 668: "Lets put a clickable (x) are the right of the legend. Clear so space after the
 * last word in the title. Remove the button at the corner of the screen", then "Change
 * the x in a circle to a button that simple labeled "close""). A fail-safe that only
 * closes: it turns these controls off until the next lock or prompt
 * (lock_keys_dismiss), and nothing brings them back meanwhile ("holding the options
 * button for 4 seconds does not bring it back. It's a close only path"). Not on the
 * numpad, where Circle closes the numpad ("no" to one there too): the Options hold
 * closes from there. Through build 668 it was a button of its own at the top right of
 * the screen ("a close button to right of screen"), lastly reading "Turn off rebind
 * overrides"; the Options hold has had its row in the legends since 668.
 * ⭐ AND A CLOSE ASKS FIRST ("On a close, open pop up with warning that rebindings will
 * be turned off and if they are sure ok or cancel"): a question at the screen's
 * centre, in the words of the third of three offered, with OK and Cancel, each with
 * the pad's button that answers the same (stream/input/lock_keys.h). Close clicked and
 * Options held 2 s alike.
 * ⓘ Clicked as the stats' pin is ("use the same functionality as the performance stats
 * pin button that removes it"): the TV's pointer reaches it during a stream as it
 * reaches the pin. A click on Close or the question is Aurora's alone, the touchpad's
 * (input/touchpad_pointer.h) and a mouse's (lock_numpad_mouse_takes) alike: the PC is
 * not told. */
/* ⭐ HIDE, IN CLOSE'S PLACE, IN ALL THREE LISTS SINCE 2026-11-12 (rhoquinn8217: "close
 * need to change to hide"): it hides the list, which the PS button brings back
 * (lock_keys_list_toggle), and changes nothing a button does. Nothing asks: the
 * question is Options held 2 s's alone now. What follows is Close's history. */
/* Room after the heading's last word, before Hide. */
#define NUMPAD_HEADING_GAP LV_DPX(12)
/* The question's width: its words wrap to it. */
#define NUMPAD_QUESTION_W LV_DPX(330)

static lv_obj_t *s_after;
static lv_obj_t *s_hide;         /* the list's, gone with it; none with the numpad */
static lv_obj_t *s_tog_touch;    /* the Mouse category's two choices, gone with the list */
static lv_obj_t *s_tog_stick;
static bool s_stick;             /* which of the two the list shows; the touchpad's until chosen */
static bool s_shown_stick;
static lv_obj_t *s_question;
static pad_family_t s_question_family;
static lv_coord_t s_after_y = -1;   /* the list's foot plus the gap; -1 until a list is up */
static lv_coord_t s_after_at = -1;  /* where the line is now */
static lv_obj_t *s_up;
static bool s_up_open;
static pad_family_t s_shown_family;
static lock_keys_mode_t s_shown_mode;
static uint32_t s_mouse_taken;   /* the mouse's buttons pressed on Hide, one bit each, until let go */

/* Into the gesture log, beside the lock keys' own lines. */
#if defined(TARGET_WEBOS)
#define mouse_note(...) ctm_bridge_gesture_note("lock list: " __VA_ARGS__)
#else
#define mouse_note(...) ((void) 0)
#endif

static lv_obj_t *box_create(lv_obj_t *parent) {
    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_remove_style_all(box);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    return box;
}

static lv_obj_t *words_create(lv_obj_t *parent, const char *words, const lv_font_t *font) {
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_remove_style_all(label);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, NUMPAD_TEXT, 0);
    lv_label_set_text(label, words);
    return label;
}

static void key_create(lv_obj_t *parent, const char *words, bool digit, pad_mark_t mark, pad_family_t family) {
    lv_obj_t *key = box_create(parent);
    lv_obj_set_size(key, NUMPAD_KEY_W, NUMPAD_KEY_H);
    lv_obj_set_style_bg_color(key, NUMPAD_KEY_BG, 0);
    lv_obj_set_style_bg_opa(key, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(key, NUMPAD_KEY_BORDER, 0);
    lv_obj_set_style_border_width(key, LV_DPX(1), 0);
    lv_obj_set_style_radius(key, LV_DPX(8), 0);

    lv_obj_t *label = words_create(key, words, digit ? lv_theme_get_font_large(parent) : lv_theme_get_font_small(parent));
    if (digit) {
        lv_obj_align(label, LV_ALIGN_CENTER, -NUMPAD_DIGIT_LEFT, NUMPAD_DIGIT_DROP);
    } else {
        lv_obj_align(label, LV_ALIGN_BOTTOM_MID, 0, -NUMPAD_WORD_LIFT);
    }

    lv_obj_t *drawn = pad_mark_create(key, mark, family, NUMPAD_KEY_MARK, NUMPAD_MARK);
    lv_obj_align(drawn, LV_ALIGN_TOP_RIGHT, -NUMPAD_KEY_INSET, NUMPAD_KEY_INSET);
}

static lv_obj_t *row_create(lv_obj_t *parent) {
    lv_obj_t *row = box_create(parent);
    lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, LV_DPX(5), 0);
    return row;
}

/* The cell a row's first mark sits in: as wide as the touchpad's mark, the widest, so
 * the words of every row line up. A shoulder button's was the widest until 2026-11-13. */
#define NUMPAD_MARK_CELL_W (NUMPAD_LEGEND_MARK * 7 / 5)

static lv_coord_t mark_height(pad_mark_t mark) {
    return (mark == PAD_MARK_STICK_R || mark == PAD_MARK_STICK_L || mark == PAD_MARK_STICK_PRESS)
                   ? NUMPAD_STICK_MARK
                   : NUMPAD_LEGEND_MARK;
}

/* A row of the list: a button's mark, in its cell, then what it does. ⓘ Nothing in bold
 * since 2026-11-12 (rhoquinn8217: "no more bold anything", and on 2026-11-13 of a bold
 * |, "unbold it"); Desktop Mode's list had its clicks and the Full Keyboard in bold until
 * then. */
static void mark_row_create(lv_obj_t *parent, pad_mark_t mark, pad_family_t family, const char *words) {
    lv_obj_t *row = row_create(parent);
    lv_obj_t *cell = box_create(row);
    lv_obj_set_size(cell, NUMPAD_MARK_CELL_W, mark_height(mark));
    lv_obj_t *drawn = pad_mark_create(cell, mark, family, mark_height(mark), NUMPAD_MARK);
    lv_obj_center(drawn);
    words_create(row, words, lv_theme_get_font_small(parent));
}

/* One piece of a row: a mark, or words. */
typedef struct {
    pad_mark_t mark;     /* when `words` is NULL */
    const char *words;
} piece_t;

#define PIECE_MARK(m) {(m), NULL}
#define PIECE_WORDS(w) {PAD_MARK_UP, (w)}

/* ⭐ A ROW OF PIECES IN ORDER, marks and words (rhoquinn8217, 2026-11-13: "combine Right
 * stick and touchpad to mouse ... Do this for other that are similar", "use | instead of
 * / and use | to separate L1 and R1 for window select"). A first mark sits in its cell,
 * as mark_row_create's does; a mark after words sits as it is. */
static void pieces_row_create(lv_obj_t *parent, pad_family_t family, const piece_t *pieces, int count) {
    lv_obj_t *row = row_create(parent);
    const lv_font_t *font = lv_theme_get_font_small(parent);
    for (int i = 0; i < count; ++i) {
        if (pieces[i].words != NULL) {
            words_create(row, pieces[i].words, font);
            continue;
        }
        if (i == 0) {
            lv_obj_t *cell = box_create(row);
            lv_obj_set_size(cell, NUMPAD_MARK_CELL_W, mark_height(pieces[i].mark));
            lv_obj_center(pad_mark_create(cell, pieces[i].mark, family, mark_height(pieces[i].mark), NUMPAD_MARK));
        } else {
            pad_mark_create(row, pieces[i].mark, family, mark_height(pieces[i].mark), NUMPAD_MARK);
        }
    }
}

/* The last row of every legend, the list's and the numpad's: Options (Menu on an Xbox
 * pad) held 2 s is a close, as Close is. Its words without the time (rhoquinn8217,
 * 2026-11-10, as the hold went from 4 s to 2: "change the 4 seconds to 2 seconds and
 * change the legend description for options symbol to "hold to close""); until then
 * "hold for 4s to close". */
static void options_row_create(lv_obj_t *parent, pad_family_t family) {
    /* ⭐ "- hold to turn off rebinds" since 2026-11-12 (rhoquinn8217: "options: hold to
     * turn off rebinds"), in all three lists and the numpad's. */
    mark_row_create(parent, PAD_MARK_MENU, family, locstr("- hold to turn off rebinds"));
}

/* The list's own row, in every list and the numpad's (rhoquinn8217, 2026-11-13): a
 * DualSense's two-finger press and an Xbox pad's Share button show or hide it ("DualSense
 * two finger press in brings up the legend", "xbox share button in brings up the
 * legend"), with the touchpad's mark, at the top of the list ("Move the legend key to the
 * top of the list"). An Xbox pad's Share button has no mark here, so its row is words.
 * ⓘ PS and the Xbox button have no row: they go to Windows as they are, not rebound
 * (rhoquinn8217, 2026-11-13: "The button is pass through and windows will have an action
 * for it that isn't necessarily steam. Removing it means it's not rebound and works as it
 * should"). Their rows said "- Steam" from that morning until then. */
static void legend_toggle_row_create(lv_obj_t *parent, pad_family_t family) {
    if (family == PAD_FAMILY_XBOX) {
        words_create(parent, locstr("Share button - show/hide legend"), lv_theme_get_font_small(parent));
    } else {
        const piece_t press[] = {PIECE_MARK(PAD_MARK_TOUCH_TWO), PIECE_WORDS(locstr("press - show/hide legend"))};
        pieces_row_create(parent, family, press, 2);
    }
}

static lv_obj_t *dark_panel_create(lv_coord_t x, lv_coord_t y) {
    lv_obj_t *panel = box_create(lv_layer_sys());
    lv_obj_set_style_bg_color(panel, NUMPAD_PANEL_BG, 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_90, 0);
    lv_obj_set_style_radius(panel, LV_DPX(10), 0);
    lv_obj_set_style_pad_all(panel, NUMPAD_PAD, 0);
    lv_obj_set_size(panel, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_align(panel, LV_ALIGN_TOP_LEFT, x, y);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    return panel;
}

static lv_obj_t *numpad_create(pad_family_t family) {
    lv_obj_t *panel = dark_panel_create((lv_coord_t) (lv_disp_get_hor_res(NULL) * NUMPAD_AT_X / 1000),
                                        (lv_coord_t) (lv_disp_get_ver_res(NULL) * NUMPAD_AT_Y / 1000));
    lv_obj_set_style_pad_row(panel, LV_DPX(8), 0);

    words_create(panel, locstr("Controller to numpad input."), lv_theme_get_font_small(panel));

    lv_obj_t *grid = box_create(panel);
    lv_obj_set_size(grid, 3 * NUMPAD_KEY_W + 2 * NUMPAD_GAP, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_row(grid, NUMPAD_GAP, 0);
    lv_obj_set_style_pad_column(grid, NUMPAD_GAP, 0);
    key_create(grid, "1", true, PAD_MARK_UP, family);
    key_create(grid, "2", true, PAD_MARK_LEFT, family);
    key_create(grid, "3", true, PAD_MARK_DOWN, family);
    key_create(grid, "4", true, PAD_MARK_RIGHT, family);
    key_create(grid, "5", true, PAD_MARK_L2, family);
    key_create(grid, "6", true, PAD_MARK_R2, family);
    key_create(grid, "7", true, PAD_MARK_L1, family);
    key_create(grid, "8", true, PAD_MARK_R1, family);
    key_create(grid, "9", true, PAD_MARK_NORTH, family);
    key_create(grid, locstr("Enter"), false, PAD_MARK_SOUTH, family);
    key_create(grid, "0", true, PAD_MARK_WEST, family);
    key_create(grid, locstr("Bksp"), false, PAD_MARK_VIEW, family);

    mark_row_create(panel, PAD_MARK_EAST, family, locstr("- close numpad"));
    legend_toggle_row_create(panel, family);
    options_row_create(panel, family);
    return panel;
}

void lock_numpad_hide_click(void) {
    lock_keys_list_hide("Hide clicked");
}

static void hide_clicked(lv_event_t *event) {
    LV_UNUSED(event);
    lock_numpad_hide_click();
}

/* Hide, in the heading: its word in a frame, as small as the word. */
static lv_obj_t *hide_create(lv_obj_t *parent) {
    lv_obj_t *button = box_create(parent);
    lv_obj_add_flag(button, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(button, NUMPAD_KEY_BG, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_border_color(button, NUMPAD_MARK, 0);
    lv_obj_set_style_border_width(button, LV_DPX(1), 0);
    lv_obj_set_style_radius(button, LV_DPX(6), 0);
    lv_obj_set_style_pad_hor(button, LV_DPX(10), 0);
    lv_obj_set_style_pad_ver(button, LV_DPX(3), 0);
    lv_obj_set_size(button, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    words_create(button, locstr("Hide"), lv_theme_get_font_small(button));
    lv_obj_add_event_cb(button, hide_clicked, LV_EVENT_CLICKED, NULL);
    return button;
}

/* ⭐ THE MOUSE CATEGORY'S CHOICE (rhoquinn8217, 2026-11-14: "Mouse should have a toggle
 * button next to it. Toggle between stick and touchpad. Default is touchpad"): two
 * halves of one pill, the chosen one filled. It changes which of the two sets of rows the
 * list shows and nothing a controller does; the list is rebuilt on the next pass
 * (lock_numpad_tick). Not saved: the touchpad's again at every lock, prompt and
 * Desktop Mode. Clicked as Hide is. */
static void toggle_touch_clicked(lv_event_t *event) {
    LV_UNUSED(event);
    s_stick = false;
}

static void toggle_stick_clicked(lv_event_t *event) {
    LV_UNUSED(event);
    s_stick = true;
}

static lv_obj_t *toggle_half(lv_obj_t *parent, const char *words, bool chosen, lv_event_cb_t clicked) {
    lv_obj_t *half = box_create(parent);
    lv_obj_add_flag(half, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(half, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_hor(half, LV_DPX(10), 0);
    lv_obj_set_style_pad_ver(half, LV_DPX(2), 0);
    lv_obj_set_style_bg_color(half, NUMPAD_MARK, 0);
    lv_obj_set_style_bg_opa(half, chosen ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_color(half, NUMPAD_KEY_BG, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(half, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_t *label = words_create(half, words, lv_theme_get_font_small(half));
    lv_obj_set_style_text_color(label, chosen ? lv_color_hex(0x111111) : NUMPAD_TEXT, 0);
    lv_obj_add_event_cb(half, clicked, LV_EVENT_CLICKED, NULL);
    return half;
}

static void toggle_create(lv_obj_t *parent) {
    lv_obj_t *pill = box_create(parent);
    lv_obj_set_size(pill, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(pill, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(pill, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_border_color(pill, NUMPAD_MARK, 0);
    lv_obj_set_style_border_width(pill, LV_DPX(1), 0);
    lv_obj_set_style_radius(pill, LV_DPX(12), 0);
    lv_obj_set_style_clip_corner(pill, true, 0);
    s_tog_touch = toggle_half(pill, locstr("Touchpad"), !s_stick, toggle_touch_clicked);
    s_tog_stick = toggle_half(pill, locstr("Stick"), s_stick, toggle_stick_clicked);
}

/* A category's name in the list, and its choice if it has one. */
static lv_obj_t *category_create(lv_obj_t *parent, const char *name) {
    lv_obj_t *row = row_create(parent);
    lv_obj_set_style_pad_top(row, LV_DPX(6), 0);
    lv_obj_set_style_pad_column(row, LV_DPX(10), 0);
    lv_obj_t *label = words_create(row, name, lv_theme_get_font_small(parent));
    lv_obj_set_style_text_color(label, NUMPAD_CAT, 0);
    return row;
}

/* The numpad closed: every button that does something, as the configs window's rows
 * read: a mark, a plain hyphen, what it does.
 * ⭐ At a User Account Control prompt, the same list under its own heading, without
 * the numpad (rhoquinn8217, 2026-11-09: "Keep mostly the same legends as the lock
 * screen but remove numpad and keyboard(unless there is a UAC screen that may require
 * keyboard input, then add the keybaord(aurora-tv's))"). The keyboard is on every
 * prompt since 2026-11-10 ("lets just add square to all UAC prompts. there's no
 * downside of having it"), and Create is Tab in both lists ("create create button
 * binded to tab"), under the d-pad: both move the focus. */
/* ⭐ AND DESKTOP MODE'S, the prompt's list under "Desktop: Controller" (rhoquinn8217,
 * 2026-11-11: "Legend Title should be just Desktop: Controller"), without Close and
 * without the Options row ("remove the close button and close button rebind"), its
 * clicks and the Full Keyboard in bold. Only while the overlay is open, and not over the
 * screen as the other two are ("It does not persist over the screen like windows log
 * in, UAC and performance states"). ⓘ "Full Keyboard" in every list since then
 * ("Change keyboard in all legend to Full Keyboard"). */
static lv_obj_t *list_create(pad_family_t family, lock_keys_mode_t mode) {
    lv_obj_t *panel = dark_panel_create(NUMPAD_MARGIN_X, NUMPAD_MARGIN_Y);
    lv_obj_set_style_pad_row(panel, NUMPAD_LIST_GAP, 0);
    const lv_font_t *font = lv_theme_get_font_small(panel);
    const bool lock = mode == LOCK_KEYS_LOCK;
    const bool desktop = mode == LOCK_KEYS_DESKTOP;

    /* The heading, then Close, NUMPAD_HEADING_GAP apart at the least, and a line under
     * the two to set them off from the rebinds. ⓘ The heading's own border, the heading
     * made as wide as the list's widest line once its rows are laid out (below), so the
     * rule runs the list's width and Close goes to its right end: the Options row can be
     * wider than "Windows Sign In: Controller". Spaced between, not grown: a row sized
     * by its content gives a grown child no room. A box of 100% would come out with no
     * width at all in a panel sized by its content. */
    lv_obj_t *heading = row_create(panel);
    lv_obj_set_flex_align(heading, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(heading, NUMPAD_HEADING_GAP, 0);
    lv_obj_set_style_pad_bottom(heading, LV_DPX(4), 0);
    lv_obj_set_style_border_side(heading, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(heading, LV_DPX(1), 0);
    lv_obj_set_style_border_color(heading, NUMPAD_MARK, 0);
    lv_obj_set_style_border_opa(heading, LV_OPA_COVER, 0);
    lv_obj_t *title = words_create(heading,
                                   lock      ? locstr("Windows Sign In: Controller")
                                   : desktop ? locstr("Desktop: Controller")
                                             : locstr("User Account Control: Controller"),
                                   font);
    s_hide = hide_create(heading);
    lv_obj_t *gap = box_create(panel);
    lv_obj_set_size(gap, 1, LV_DPX(2));
    /* ⭐ THE SAME ROWS IN ALL THREE since 2026-11-12 (rhoquinn8217: "Lets consolidate
     * windows lock, UAC and Desktop mode. It doesn't make sense to make them
     * different."; stream/input/lock_keys.h), only the heading says which. */
    /* ⭐ IN THREE CATEGORIES since 2026-11-14 (rhoquinn8217: "separate the items into
     * categories", "combine Windows, Cursor, Legend and rebinds into one category", "Mouse
     * should have a toggle button next to it ... default is touchpad", "move scroll after
     * mouse and add "tilt" after the stick symbol followed by the dash"): Mouse, with its
     * choice between the touchpad's rows and the stick's; Keys; and Other. An Xbox pad has
     * no touchpad: its list is the stick's, with no choice. The sticks' marks are plain
     * ("remove the arrow on the stick for mouse and scroll"). */
    const bool stick = family == PAD_FAMILY_XBOX || s_stick;
    lv_obj_t *mouse_head = category_create(panel, locstr("MOUSE"));
    if (family != PAD_FAMILY_XBOX) {
        toggle_create(mouse_head);
    }
    if (stick) {
        const piece_t mouse[] = {PIECE_MARK(PAD_MARK_STICK_R), PIECE_WORDS(locstr("tilt - mouse"))};
        const piece_t scroll[] = {PIECE_MARK(PAD_MARK_STICK_L), PIECE_WORDS(locstr("tilt - scroll"))};
        const piece_t drag[] = {PIECE_MARK(PAD_MARK_R2), PIECE_WORDS(locstr("hold - drag"))};
        pieces_row_create(panel, family, mouse, 2);
        pieces_row_create(panel, family, scroll, 2);
        mark_row_create(panel, PAD_MARK_R2, family, locstr("- left click"));
        mark_row_create(panel, PAD_MARK_L2, family, locstr("- right click"));
        pieces_row_create(panel, family, drag, 2);
    } else {
        const piece_t mouse[] = {PIECE_MARK(PAD_MARK_TOUCH_ONE), PIECE_WORDS(locstr("slide - mouse"))};
        const piece_t scroll[] = {PIECE_MARK(PAD_MARK_TOUCH_TWO), PIECE_WORDS(locstr("swipe - scroll"))};
        const piece_t left[] = {PIECE_MARK(PAD_MARK_TOUCH_ONE), PIECE_WORDS(locstr("tap - left click"))};
        const piece_t right[] = {PIECE_MARK(PAD_MARK_TOUCH_TWO), PIECE_WORDS(locstr("tap - right click"))};
        const piece_t drag[] = {PIECE_MARK(PAD_MARK_TOUCH_ONE), PIECE_WORDS(locstr("press, hold touch - drag"))};
        pieces_row_create(panel, family, mouse, 2);
        pieces_row_create(panel, family, scroll, 2);
        pieces_row_create(panel, family, left, 2);
        pieces_row_create(panel, family, right, 2);
        pieces_row_create(panel, family, drag, 2);
    }
    category_create(panel, locstr("KEYS"));
    mark_row_create(panel, PAD_MARK_DPAD, family, locstr("- arrow keys"));
    mark_row_create(panel, PAD_MARK_VIEW, family, locstr("- Tab"));
    mark_row_create(panel, PAD_MARK_SOUTH, family, locstr("- Enter"));
    mark_row_create(panel, PAD_MARK_EAST, family, locstr("- Esc"));
    mark_row_create(panel, PAD_MARK_WEST, family, locstr("- Full Keyboard"));
    mark_row_create(panel, PAD_MARK_NORTH, family, locstr("- numpad"));
    category_create(panel, locstr("OTHER"));
    const piece_t window[] = {PIECE_MARK(PAD_MARK_L1), PIECE_WORDS("|"), PIECE_MARK(PAD_MARK_R1),
                              PIECE_WORDS(locstr("- Window Select"))};
    pieces_row_create(panel, family, window, 4);
    const piece_t align[] = {PIECE_MARK(PAD_MARK_STICK_PRESS),
                             PIECE_WORDS(family == PAD_FAMILY_XBOX ? locstr("LS - Align Cursor")
                                                                   : locstr("L3 - Align Cursor"))};
    pieces_row_create(panel, family, align, 2);
    legend_toggle_row_create(panel, family);
    options_row_create(panel, family);
    /* ⭐ THE LIST AS WIDE AS ITS WIDEST LINE, THE HEADING WITH CLOSE AMONG THEM
     * (rhoquinn8217, 2026-11-10, at build 669, where Close sat over the heading's last
     * word: "Expand the legend horizontally to fit the close button. Check the UAD
     * legend as well"). Measured off the lines themselves: the list's own width, read
     * straight after a first layout, still had the heading as wide as its words alone.
     * And the list set to it as well, not left to follow its heading. */
    lv_obj_update_layout(panel);
    lv_coord_t widest = (lv_coord_t) (lv_obj_get_width(title) +
                                      (s_hide != NULL ? NUMPAD_HEADING_GAP + lv_obj_get_width(s_hide) : 0));
    const uint32_t lines = lv_obj_get_child_cnt(panel);
    for (uint32_t i = 0; i < lines; ++i) {
        lv_obj_t *line = lv_obj_get_child(panel, (int32_t) i);
        if (line != heading && lv_obj_get_width(line) > widest) {
            widest = lv_obj_get_width(line);
        }
    }
    lv_obj_set_width(heading, widest);
    lv_obj_set_width(panel, (lv_coord_t) (widest + 2 * NUMPAD_PAD));
    return panel;
}

static void question_ok_clicked(lv_event_t *event) {
    LV_UNUSED(event);
    lock_keys_answer(true, "clicked");
}

static void question_cancel_clicked(lv_event_t *event) {
    LV_UNUSED(event);
    lock_keys_answer(false, "clicked");
}

/* One of the question's answers: the pad's button that gives the same answer, then its
 * word, in a key's colours. */
static void answer_create(lv_obj_t *parent, pad_mark_t mark, pad_family_t family, const char *words,
                          lv_event_cb_t clicked) {
    lv_obj_t *button = row_create(parent);
    lv_obj_add_flag(button, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(button, NUMPAD_KEY_BG, 0);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(button, NUMPAD_KEY_BORDER, LV_STATE_PRESSED);
    lv_obj_set_style_border_color(button, NUMPAD_KEY_BORDER, 0);
    lv_obj_set_style_border_width(button, LV_DPX(1), 0);
    lv_obj_set_style_radius(button, LV_DPX(8), 0);
    lv_obj_set_style_pad_hor(button, LV_DPX(10), 0);
    lv_obj_set_style_pad_ver(button, LV_DPX(4), 0);
    pad_mark_create(button, mark, family, NUMPAD_LEGEND_MARK, NUMPAD_MARK);
    words_create(button, words, lv_theme_get_font_small(button));
    lv_obj_add_event_cb(button, clicked, LV_EVENT_CLICKED, NULL);
}

/* ⭐ THE QUESTION A CLOSE RAISES, at the screen's centre, over the rest: its words are
 * the third of three offered (rhoquinn8217, 2026-11-10: "3"). The line under it in the
 * picture they saw was mine, not the question's ("you don't need to give the
 * instructions again at the bottom of the popup"). ⓘ Clickable all over with nothing to
 * do, so a click on its words reaches nothing behind. */
static lv_obj_t *question_create(pad_family_t family) {
    lv_obj_t *panel = box_create(lv_layer_sys());
    lv_obj_add_flag(panel, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(panel, NUMPAD_PANEL_BG, 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(panel, NUMPAD_MARK, 0);
    lv_obj_set_style_border_width(panel, LV_DPX(1), 0);
    lv_obj_set_style_radius(panel, LV_DPX(10), 0);
    lv_obj_set_style_pad_all(panel, LV_DPX(14), 0);
    lv_obj_set_style_pad_row(panel, LV_DPX(8), 0);
    lv_obj_set_size(panel, NUMPAD_QUESTION_W, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_center(panel);

    /* ⭐ THEIR WORDS (rhoquinn8217, 2026-11-12, on the C3: "Please confirm. Remove User
     * Account Control Rebinds? Ok, Cancel"), in place of "Are you sure? Rebinds will be
     * disabled." from earlier that day. At a lock, the lock list's own name in the same
     * place, as its heading says it. Until then it also said which controls came back:
     * plain ones, or Desktop Mode's. */
    words_create(panel, locstr("Please confirm."), lv_theme_get_font_normal(panel));
    /* ⓘ And in Desktop Mode, which asks too since 2026-11-12 ("keep the question"),
     * its own name, my words. */
    const lock_keys_mode_t mode = lock_keys_mode();
    lv_obj_t *words = words_create(panel,
                                   mode == LOCK_KEYS_LOCK      ? locstr("Remove Windows Sign In Rebinds?")
                                   : mode == LOCK_KEYS_DESKTOP ? locstr("Remove Desktop Mode Rebinds?")
                                                               : locstr("Remove User Account Control Rebinds?"),
                                   lv_theme_get_font_small(panel));
    lv_label_set_long_mode(words, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(words, lv_pct(100));

    lv_obj_t *answers = row_create(panel);
    lv_obj_set_width(answers, lv_pct(100));
    lv_obj_set_flex_align(answers, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(answers, LV_DPX(8), 0);
    lv_obj_set_style_pad_top(answers, LV_DPX(4), 0);
    answer_create(answers, PAD_MARK_SOUTH, family, locstr("OK"), question_ok_clicked);
    answer_create(answers, PAD_MARK_EAST, family, locstr("Cancel"), question_cancel_clicked);
    /* Laid out now: a click can be asked about before the screen is next drawn. */
    lv_obj_update_layout(panel);
    return panel;
}

/* Up while a close's question waits, in the marks of the pad in hand. */
static void question_show(bool on) {
    if (!on) {
        if (s_question != NULL) {
            lv_obj_del(s_question);
            s_question = NULL;
        }
        return;
    }
    const pad_family_t family = pad_family_current();
    if (s_question != NULL && s_question_family != family) {
        lv_obj_del(s_question);
        s_question = NULL;
    }
    if (s_question == NULL) {
        s_question = question_create(family);
        s_question_family = family;
    }
}

static void take_down(void) {
    if (s_up != NULL) {
        lv_obj_del(s_up);
        s_up = NULL;
    }
    s_hide = NULL;
    s_tog_touch = NULL;
    s_tog_stick = NULL;
}

static bool after_sign_in(void) {
#if defined(TARGET_WEBOS)
    return auto_bridge_after_sign_in();
#else
    return false;
#endif
}

static void after_show(bool show) {
    const lv_coord_t y = s_after_y >= 0 ? s_after_y : NUMPAD_AFTER_Y;
    if (show && s_after == NULL) {
        s_after_at = y;
        s_after = dark_panel_create(NUMPAD_MARGIN_X, y);
        words_create(s_after, locstr("Devices will be auto bridged after sign in."), lv_theme_get_font_small(s_after));
    } else if (show && s_after_at != y) {
        s_after_at = y;
        lv_obj_align(s_after, LV_ALIGN_TOP_LEFT, NUMPAD_MARGIN_X, y);
    } else if (!show && s_after != NULL) {
        lv_obj_del(s_after);
        s_after = NULL;
    }
}

void lock_numpad_tick(void) {
    const lock_keys_mode_t mode = lock_keys_mode();
    /* ⭐ Over the screen in all three since 2026-11-12, Desktop Mode's too ("Over the
     * screen like window lock and UAC"; until then in the overlay, and over the screen
     * only by the PS button), unless hidden (lock_keys_list_hidden). The question still
     * comes up with the list hidden: Options held asks it.
     * ⭐ AND DESKTOP MODE'S IN THE OVERLAY, HIDDEN OR NOT (rhoquinn8217, 2026-11-12: "the
     * legend should always be in the overlay when its turned on. Even if it's hidden, it
     * should still be in the overlay when Desktop mode is turned on"), as it was before
     * it went over the screen; not over a panel the overlay opened. */
    if (mode == LOCK_KEYS_OFF) {
        s_stick = false;   /* the touchpad's rows again at the next lock, prompt or Desktop Mode */
    }
    const bool up = lock_keys_active() && !ui_should_block_input();
    const bool in_overlay =
        mode == LOCK_KEYS_DESKTOP && streaming_overlay_shown() && !ctm_panel_shown() && !bt_panel_shown();
    const bool showing = in_overlay || (up && !lock_keys_list_hidden());
    /* ⓘ The line about Auto Bridge is the lock's: a prompt does not hold a bridge
     * back (rhoquinn8217, 2026-11-09: "If the UAC: controller legend is the same, we
     * don't need to have that message"). */
    const bool after = showing && mode == LOCK_KEYS_LOCK && after_sign_in();
    if (!showing || !lock_keys_pad_on_stream()) {
        take_down();
        question_show(up && lock_keys_pad_on_stream() && lock_keys_asking());
        after_show(after);
        return;
    }
    const bool open = lock_keys_numpad_open();
    const pad_family_t family = pad_family_current();
    if (s_up == NULL || s_up_open != open || s_shown_family != family || s_shown_mode != mode ||
        s_shown_stick != s_stick) {
        take_down();
        s_up = open ? numpad_create(family) : list_create(family, mode);
        s_up_open = open;
        s_shown_family = family;
        s_shown_mode = mode;
        s_shown_stick = s_stick;
        if (!open) {
            /* Its rows are laid out now, so its foot is known. */
            lv_obj_update_layout(s_up);
            lv_area_t area;
            lv_obj_get_coords(s_up, &area);
            s_after_y = (lv_coord_t) (area.y2 + 1 + NUMPAD_AFTER_GAP);
        }
        if (s_question != NULL) {
            /* Still over the rest. */
            lv_obj_move_foreground(s_question);
        }
    }
    question_show(lock_keys_asking());
    after_show(after);
}

bool lock_numpad_question_up(void) {
    return s_question != NULL;
}

bool lock_numpad_after_up(void) {
    return s_after != NULL;
}

bool lock_numpad_hide_up(void) {
    return s_hide != NULL;
}

/* What of Aurora's that asks to be clicked is at this point, by name, or NULL. The
 * question first: it is drawn over the rest.
 * ⭐ And the stats' pin while the stats are pinned over the stream (rhoquinn8217,
 * 2026-11-13: "clicking buttons when they are presented over the stream (the stats pin
 * button and the close button) ... stop the click from clicking what's behind the
 * button"): a click on it went to the PC too. Each by its click area, which takes in
 * a widened one, as the pin's is: the button's own box left a ring around it where the
 * PC was clicked as well. */
static const char *button_at(lv_coord_t x, lv_coord_t y) {
    const struct {
        lv_obj_t *obj;
        const char *name;
    } asks[] = {{s_question, "the question"},
                {s_hide, "Hide"},
                {s_tog_touch, "the Touchpad choice"},
                {s_tog_stick, "the Stick choice"},
                {streaming_stats_pin_shown(), "the stats' pin"}};
    for (int i = 0; i < (int) (sizeof asks / sizeof asks[0]); ++i) {
        if (asks[i].obj == NULL) {
            continue;
        }
        lv_area_t area;
        lv_obj_get_click_area(asks[i].obj, &area);
        if (x >= area.x1 && x <= area.x2 && y >= area.y1 && y <= area.y2) {
            return asks[i].name;
        }
    }
    return NULL;
}

bool lock_numpad_button_at(lv_coord_t x, lv_coord_t y) {
    return button_at(x, y) != NULL;
}

bool lock_numpad_mouse_takes(int button, bool down, lv_coord_t x, lv_coord_t y) {
    if (button < 0 || button >= 32) {
        return false;
    }
    const uint32_t bit = 1u << (unsigned) button;
    if (!down) {
        /* Let go: the stream is told only if it was told of the press. */
        if ((s_mouse_taken & bit) == 0) {
            return false;
        }
        s_mouse_taken &= ~bit;
        mouse_note("the mouse let go of button %d at %d,%d: the PC not told", button, (int) x, (int) y);
        return true;
    }
    s_mouse_taken &= ~bit;   /* a press whose release never came */
    const char *on = button_at(x, y);
    if (on == NULL) {
        return false;
    }
    s_mouse_taken |= bit;
    mouse_note("the mouse pressed button %d on %s at %d,%d: Aurora's alone, the PC not told", button, on, (int) x,
               (int) y);
    return true;
}

lv_coord_t lock_numpad_after_at(void) {
    return s_after != NULL ? s_after_at : -1;
}

const char *lock_numpad_state(void) {
    return s_up == NULL ? "off" : (s_up_open ? "open" : "closed");
}

bool lock_numpad_area(lv_area_t *out) {
    if (s_up == NULL) {
        return false;
    }
    lv_obj_get_coords(s_up, out);
    return true;
}
