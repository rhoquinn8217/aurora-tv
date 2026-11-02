/* The numpad over a locked Windows. The why is in the header. */

#include "lock_numpad.h"

#include "stream/input/lock_keys.h"
#include "ui/common/pad_marks.h"
#include "ui/root.h"
#include "util/i18n.h"

/* The keyboard's own colours (soft_keyboard.c), as the numpad gives way to it. */
#define NUMPAD_PANEL_BG lv_color_hex(0x1B1B1B)
#define NUMPAD_KEY_BG lv_color_hex(0x2C2C2C)
#define NUMPAD_KEY_BORDER lv_color_hex(0x3A3A3A)
#define NUMPAD_TEXT lv_color_hex(0xFFFFFF)
#define NUMPAD_MARK lv_color_hex(0xE8E8E8)

/* ⓘ LV_DPX(n) is 2n pixels on a 1920-wide interface. */
#define NUMPAD_MARGIN_X LV_DPX(30)
#define NUMPAD_MARGIN_Y LV_DPX(20)
#define NUMPAD_PAD LV_DPX(12)
#define NUMPAD_GAP LV_DPX(6)
#define NUMPAD_KEY_W LV_DPX(75)
#define NUMPAD_KEY_H LV_DPX(50)
#define NUMPAD_KEY_MARK LV_DPX(22)
#define NUMPAD_KEY_INSET LV_DPX(3)
#define NUMPAD_LEGEND_MARK LV_DPX(18)
/* A digit sits a little left of the middle and below it: the mark has the corner.
 * A word is wider, so it sits along the foot of the key, under the mark. */
#define NUMPAD_DIGIT_LEFT LV_DPX(12)
#define NUMPAD_DIGIT_DROP LV_DPX(6)
#define NUMPAD_WORD_LIFT LV_DPX(4)

static lv_obj_t *s_panel;
static lv_obj_t *s_message;
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
    return row;
}

static void legend_create(lv_obj_t *parent, pad_mark_t mark, pad_family_t family, const char *words) {
    lv_obj_t *entry = row_create(parent);
    lv_obj_set_style_pad_column(entry, LV_DPX(5), 0);
    pad_mark_create(entry, mark, family, NUMPAD_LEGEND_MARK, NUMPAD_MARK);
    words_create(entry, words, lv_theme_get_font_small(parent));
}

static lv_obj_t *dark_panel_create(void) {
    lv_obj_t *panel = box_create(lv_layer_sys());
    lv_obj_set_style_bg_color(panel, NUMPAD_PANEL_BG, 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_90, 0);
    lv_obj_set_style_radius(panel, LV_DPX(10), 0);
    lv_obj_set_style_pad_all(panel, NUMPAD_PAD, 0);
    lv_obj_set_size(panel, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_align(panel, LV_ALIGN_TOP_LEFT, NUMPAD_MARGIN_X, NUMPAD_MARGIN_Y);
    return panel;
}

static void numpad_create(pad_family_t family) {
    lv_obj_t *panel = dark_panel_create();
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(panel, LV_DPX(8), 0);

    words_create(panel, locstr("Windows is locked. Type your PIN."), lv_theme_get_font_small(panel));

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

    lv_obj_t *legend = row_create(panel);
    lv_obj_set_style_pad_column(legend, LV_DPX(16), 0);
    legend_create(legend, PAD_MARK_MENU, family, locstr("Keyboard"));
    legend_create(legend, PAD_MARK_EAST, family, locstr("Hide"));

    s_panel = panel;
}

static void message_create(pad_family_t family) {
    lv_obj_t *panel = dark_panel_create();
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(panel, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(panel, LV_DPX(6), 0);
    pad_mark_create(panel, PAD_MARK_EAST, family, NUMPAD_LEGEND_MARK, NUMPAD_MARK);
    words_create(panel, locstr("brings back the numpad"), lv_theme_get_font_small(panel));
    s_message = panel;
}

static void remove_obj(lv_obj_t **obj) {
    if (*obj != NULL) {
        lv_obj_del(*obj);
        *obj = NULL;
    }
}

void lock_numpad_tick(void) {
    if (!lock_keys_active()) {
        remove_obj(&s_panel);
        remove_obj(&s_message);
        return;
    }
    if (ui_should_block_input() || !lock_keys_pad_on_stream()) {
        remove_obj(&s_panel);
        remove_obj(&s_message);
        return;
    }
    const pad_family_t family = pad_family_current();
    if (family != s_shown_family) {
        s_shown_family = family;
        remove_obj(&s_panel);
        remove_obj(&s_message);
    }
    if (lock_keys_numpad_hidden()) {
        remove_obj(&s_panel);
        if (s_message == NULL) {
            message_create(family);
        }
    } else {
        remove_obj(&s_message);
        if (s_panel == NULL) {
            numpad_create(family);
        }
    }
}

const char *lock_numpad_state(void) {
    return s_panel != NULL ? "shown" : (s_message != NULL ? "hidden" : "off");
}

bool lock_numpad_area(lv_area_t *out) {
    const lv_obj_t *up = s_panel != NULL ? s_panel : s_message;
    if (up == NULL) {
        return false;
    }
    lv_obj_get_coords(up, out);
    return true;
}
