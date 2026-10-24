/* The on-screen keyboard's Close tab. The why is in the header. */

#include "soft_keyboard_close.h"

#include "lvgl/font/material_icons_regular_symbols.h"
#include "util/font.h"   /* app_fonts_t, which the theme's header names without declaring */
#include "lvgl/theme/lv_theme_moonlight.h"
#include "util/bus.h"
#include "util/i18n.h"
#include "util/user_event.h"

static void tab_clicked(lv_event_t *event) {
    LV_UNUSED(event);
    /* As the remote's Back (lvgl/input/lv_drv_sdl_key.c): the stream screen
     * closes the keyboard and tells the session it has gone. */
    bus_pushevent(USER_CLOSE_SOFT_KEYBOARD, NULL, NULL);
}

/* On the panel's top edge, overlapping its top padding but not the keys, the
 * tab's right end over the keys' right end. */
static void place(lv_obj_t *tab, lv_obj_t *panel) {
    lv_obj_align_to(tab, panel, LV_ALIGN_OUT_TOP_RIGHT, -lv_obj_get_style_pad_right(panel, LV_PART_MAIN),
                    LV_DPX(6));
}

static void panel_resized(lv_event_t *event) {
    place(lv_event_get_user_data(event), lv_event_get_current_target(event));
}

void soft_keyboard_close_tab(lv_obj_t *keyboard, lv_obj_t *panel) {
    if (keyboard == NULL || panel == NULL) {
        return;
    }
    /* A plain object, not a button, and in no group: the pad and the arrows
     * keep walking the keys alone; this one is for a pointer. */
    lv_obj_t *tab = lv_obj_create(keyboard);
    lv_obj_remove_style_all(tab);
    lv_obj_add_flag(tab, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(tab, LV_OBJ_FLAG_CLICK_FOCUSABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(tab, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_layout(tab, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(tab, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(tab, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(tab, LV_DPX(12), 0);
    lv_obj_set_style_pad_ver(tab, LV_DPX(6), 0);
    lv_obj_set_style_pad_column(tab, LV_DPX(6), 0);
    lv_obj_set_style_radius(tab, LV_DPX(8), 0);
    /* The keys' own colours (soft_keyboard.c: KBD_KEY_BG, KBD_KEY_PRESS). */
    lv_obj_set_style_bg_color(tab, lv_color_hex(0x2C2C2C), 0);
    lv_obj_set_style_bg_color(tab, lv_color_hex(0x3A3A3A), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(tab, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(tab, lv_color_hex(0x3A3A3A), 0);
    lv_obj_set_style_border_width(tab, 1, 0);
    lv_obj_set_style_text_color(tab, lv_color_hex(0xEBEBEB), 0);

    lv_obj_t *mark = lv_label_create(tab);
    lv_obj_set_style_text_font(mark, lv_theme_moonlight_get_iconfont_small(keyboard), 0);
    lv_label_set_text_static(mark, MAT_SYMBOL_CLOSE);
    lv_obj_t *word = lv_label_create(tab);
    lv_obj_set_style_text_font(word, lv_theme_get_font_small(keyboard), 0);
    lv_label_set_text_static(word, locstr("Close"));

    lv_obj_add_event_cb(tab, tab_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(panel, panel_resized, LV_EVENT_SIZE_CHANGED, tab);
    /* The panel is placed by layout, so lay out first, then place the tab. */
    lv_obj_update_layout(keyboard);
    place(tab, panel);
}
