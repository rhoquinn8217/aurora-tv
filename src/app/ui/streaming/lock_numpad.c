/* What a controller does over a locked Windows, on the screen. The why is in the
 * header. */

#include "lock_numpad.h"

#include "stream/input/lock_keys.h"
#include "ui/common/pad_marks.h"
#include "ui/root.h"
#include "util/i18n.h"
#if defined(TARGET_WEBOS)
#include "input/auto_bridge.h"
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
/* Where the line about Auto Bridge goes: below the list, with the numpad open as well,
 * so that it does not move. */
#define NUMPAD_AFTER_Y (NUMPAD_MARGIN_Y + LV_DPX(285))

static lv_obj_t *s_after;
static lv_obj_t *s_up;
static bool s_up_open;
static pad_family_t s_shown_family;

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

/* A row of the list: a button's mark, in a cell as wide as a shoulder button's so
 * the words line up, then what it does. */
static void mark_row_create(lv_obj_t *parent, pad_mark_t mark, pad_family_t family, const char *words) {
    lv_obj_t *row = row_create(parent);
    lv_obj_t *cell = box_create(row);
    lv_obj_set_size(cell, NUMPAD_LEGEND_MARK * 6 / 5, NUMPAD_LEGEND_MARK);
    lv_obj_t *drawn = pad_mark_create(cell, mark, family, NUMPAD_LEGEND_MARK, NUMPAD_MARK);
    lv_obj_center(drawn);
    words_create(row, words, lv_theme_get_font_small(parent));
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
    return panel;
}

/* The numpad closed: every button that does something, as the configs window's rows
 * read: a mark, a plain hyphen, what it does. */
static lv_obj_t *list_create(pad_family_t family) {
    lv_obj_t *panel = dark_panel_create(NUMPAD_MARGIN_X, NUMPAD_MARGIN_Y);
    lv_obj_set_style_pad_row(panel, NUMPAD_LIST_GAP, 0);
    const lv_font_t *font = lv_theme_get_font_small(panel);

    /* A line under the heading, to set it off from the rebinds. ⓘ The heading's own
     * border: it is the list's widest line, so the rule runs the list's width. A box of
     * 100% would come out with no width at all in a panel sized by its content. */
    lv_obj_t *title = words_create(panel, locstr("Windows Sign In: Controller"), font);
    lv_obj_set_style_pad_bottom(title, LV_DPX(4), 0);
    lv_obj_set_style_border_side(title, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(title, LV_DPX(1), 0);
    lv_obj_set_style_border_color(title, NUMPAD_MARK, 0);
    lv_obj_set_style_border_opa(title, LV_OPA_COVER, 0);
    lv_obj_t *gap = box_create(panel);
    lv_obj_set_size(gap, 1, LV_DPX(2));
    words_create(panel, locstr("Right stick - mouse"), font);
    words_create(panel, locstr("Touchpad - mouse"), font);
    words_create(panel, locstr("1 finger tap - left click"), font);
    words_create(panel, locstr("2 finger tap - right click"), font);
    mark_row_create(panel, PAD_MARK_R2, family, locstr("- left click"));
    mark_row_create(panel, PAD_MARK_L2, family, locstr("- right click"));
    mark_row_create(panel, PAD_MARK_DPAD, family, locstr("- arrow keys"));
    mark_row_create(panel, PAD_MARK_SOUTH, family, locstr("- Enter"));
    mark_row_create(panel, PAD_MARK_EAST, family, locstr("- Esc"));
    mark_row_create(panel, PAD_MARK_WEST, family, locstr("- keyboard"));
    mark_row_create(panel, PAD_MARK_NORTH, family, locstr("- numpad"));
    return panel;
}

static void take_down(void) {
    if (s_up != NULL) {
        lv_obj_del(s_up);
        s_up = NULL;
    }
}

static bool after_sign_in(void) {
#if defined(TARGET_WEBOS)
    return auto_bridge_after_sign_in();
#else
    return false;
#endif
}

static void after_show(bool show) {
    if (show && s_after == NULL) {
        s_after = dark_panel_create(NUMPAD_MARGIN_X, NUMPAD_AFTER_Y);
        words_create(s_after, locstr("Devices will be auto bridged after sign in."), lv_theme_get_font_small(s_after));
    } else if (!show && s_after != NULL) {
        lv_obj_del(s_after);
        s_after = NULL;
    }
}

void lock_numpad_tick(void) {
    const bool showing = lock_keys_active() && !ui_should_block_input();
    after_show(showing && after_sign_in());
    if (!showing || !lock_keys_pad_on_stream()) {
        take_down();
        return;
    }
    const bool open = lock_keys_numpad_open();
    const pad_family_t family = pad_family_current();
    if (s_up != NULL && s_up_open == open && s_shown_family == family) {
        return;
    }
    take_down();
    s_up = open ? numpad_create(family) : list_create(family);
    s_up_open = open;
    s_shown_family = family;
}

bool lock_numpad_after_up(void) {
    return s_after != NULL;
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
