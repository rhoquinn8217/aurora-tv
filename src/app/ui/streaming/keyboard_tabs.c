/* The on-screen keyboard's Move and Close tabs. The why is in the header. */

#include "keyboard_tabs.h"

#include "app.h"
#include "input/input_gamepad.h"
#include "keyboard_place.h"
#include "lvgl/font/material_icons_regular_symbols.h"
#include "util/font.h"   /* app_fonts_t, which the theme's header names without declaring */
#include "lvgl/theme/lv_theme_moonlight.h"
#include "ui/common/pad_marks.h"
#include "util/bus.h"
#include "util/i18n.h"
#include "util/user_event.h"

/* The keys' own colours (soft_keyboard.c: KBD_KEY_BG, KBD_KEY_PRESS). */
#define TAB_BG      0x2C2C2C
#define TAB_PRESS   0x3A3A3A
#define TAB_TEXT    0xEBEBEB
/* How far the tabs reach over the panel's padding. */
#define TAB_OVERLAP LV_DPX(6)
#define TAB_MARK    LV_DPX(18)

static lv_obj_t *s_row;
static lv_obj_t *s_panel;
static lv_obj_t *s_move;

static void close_clicked(lv_event_t *event) {
    LV_UNUSED(event);
    /* As the remote's Back (lvgl/input/lv_drv_sdl_key.c): the stream screen
     * closes the keyboard and tells the session it has gone. */
    bus_pushevent(USER_CLOSE_SOFT_KEYBOARD, NULL, NULL);
}

static void move_clicked(lv_event_t *event) {
    LV_UNUSED(event);
    /* As Create: keyboard_place.c moves it, and the tabs with it. */
    keyboard_place_toggle();
}

/* A plain object, not a button, and in no group: the pad and the arrows keep
 * walking the keys alone; a tab is for a pointer. */
static lv_obj_t *tab_create(lv_obj_t *row, lv_event_cb_t clicked) {
    lv_obj_t *tab = lv_obj_create(row);
    lv_obj_remove_style_all(tab);
    lv_obj_add_flag(tab, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(tab, LV_OBJ_FLAG_CLICK_FOCUSABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(tab, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(tab, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(tab, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(tab, LV_DPX(12), 0);
    lv_obj_set_style_pad_ver(tab, LV_DPX(6), 0);
    lv_obj_set_style_pad_column(tab, LV_DPX(6), 0);
    lv_obj_set_style_radius(tab, LV_DPX(8), 0);
    lv_obj_set_style_bg_color(tab, lv_color_hex(TAB_BG), 0);
    lv_obj_set_style_bg_color(tab, lv_color_hex(TAB_PRESS), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(tab, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(tab, lv_color_hex(TAB_PRESS), 0);
    lv_obj_set_style_border_width(tab, 1, 0);
    lv_obj_set_style_text_color(tab, lv_color_hex(TAB_TEXT), 0);
    lv_obj_add_event_cb(tab, clicked, LV_EVENT_CLICKED, NULL);
    return tab;
}

static void tab_words(lv_obj_t *tab, const char *words) {
    lv_obj_t *label = lv_label_create(tab);
    lv_obj_set_style_text_font(label, lv_theme_get_font_small(tab), 0);
    lv_label_set_text_static(label, words);
}

/* Move says where it goes next. Create's mark only while a controller is
 * connected: without one, nothing can press it. */
static void move_fill(void) {
    lv_obj_clean(s_move);
    if (global != NULL && app_input_get_gamepads_count(&global->input) > 0) {
        pad_mark_create(s_move, PAD_MARK_VIEW, pad_family_current(), TAB_MARK, lv_color_hex(TAB_TEXT));
    }
    tab_words(s_move, keyboard_place_at_top() ? locstr("Move to bottom") : locstr("Move to top"));
}

/* Overlapping the panel's padding but not the keys, Close's right end over the
 * keys' right end. The keys sit inside the panel's padding and inside their
 * button matrix's own, which the theme sets: the panel's alone left the Close
 * tab 46 px past F12 on the rooted monitor (Build 583). */
static void row_place(void) {
    if (s_row == NULL || s_panel == NULL) {
        return;
    }
    lv_coord_t inset = lv_obj_get_style_pad_right(s_panel, LV_PART_MAIN);
    const uint32_t children = lv_obj_get_child_cnt(s_panel);
    for (uint32_t i = 0; i < children; ++i) {
        lv_obj_t *child = lv_obj_get_child(s_panel, (int32_t) i);
        if (lv_obj_check_type(child, &lv_btnmatrix_class)) {
            inset += lv_obj_get_style_pad_right(child, LV_PART_MAIN);
            break;
        }
    }
    /* The panel is placed by layout, and Move's words change the row's width: lay
     * out first, then place. */
    lv_obj_update_layout(s_panel);
    if (keyboard_place_at_top()) {
        lv_obj_align_to(s_row, s_panel, LV_ALIGN_OUT_BOTTOM_RIGHT, -inset, -TAB_OVERLAP);
    } else {
        lv_obj_align_to(s_row, s_panel, LV_ALIGN_OUT_TOP_RIGHT, -inset, TAB_OVERLAP);
    }
}

static void panel_resized(lv_event_t *event) {
    LV_UNUSED(event);
    row_place();
}

static void row_deleted(lv_event_t *event) {
    LV_UNUSED(event);
    s_row = NULL;
    s_panel = NULL;
    s_move = NULL;
}

void keyboard_tabs_place(void) {
    if (s_row == NULL) {
        return;
    }
    move_fill();
    row_place();
}

void keyboard_tabs_create(lv_obj_t *keyboard, lv_obj_t *panel) {
    if (keyboard == NULL || panel == NULL) {
        return;
    }
    /* Not clickable itself: a click between the tabs lands where it would without them.
     * ⛔ START, not END: a row as wide as its content counts only how far its tabs
     * reach to the right, so END packed Move past its left edge and cut it to "top"
     * (Build 645 on the rooted monitor). Where the row sits is row_place()'s job. */
    lv_obj_t *row = lv_obj_create(keyboard);
    lv_obj_remove_style_all(row);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, LV_DPX(6), 0);
    s_row = row;
    s_panel = panel;

    s_move = tab_create(row, move_clicked);
    move_fill();

    lv_obj_t *close = tab_create(row, close_clicked);
    lv_obj_t *mark = lv_label_create(close);
    lv_obj_set_style_text_font(mark, lv_theme_moonlight_get_iconfont_small(keyboard), 0);
    lv_label_set_text_static(mark, MAT_SYMBOL_CLOSE);
    tab_words(close, locstr("Close"));

    lv_obj_add_event_cb(row, row_deleted, LV_EVENT_DELETE, NULL);
    lv_obj_add_event_cb(panel, panel_resized, LV_EVENT_SIZE_CHANGED, NULL);
    row_place();
}
