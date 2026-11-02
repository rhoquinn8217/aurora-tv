/* Where the on-screen keyboard sits. The why is in the header. */

#include "keyboard_place.h"

#include "app.h"
#include "input/input_gamepad.h"
#include "ui/common/pad_marks.h"
#include "util/i18n.h"

#define PLACE_HINT_MARK LV_DPX(18)

static bool s_top;
static lv_obj_t *s_panel;
static lv_obj_t *s_hint;

static void hint_show(void) {
    if (s_hint != NULL) {
        lv_obj_del(s_hint);
        s_hint = NULL;
    }
    if (s_panel == NULL || global == NULL || app_input_get_gamepads_count(&global->input) <= 0) {
        return;
    }
    /* In the strip the key matrix leaves above its keys, at the right. */
    lv_obj_t *hint = lv_obj_create(s_panel);
    lv_obj_remove_style_all(hint);
    lv_obj_clear_flag(hint, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(hint, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(hint, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(hint, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(hint, LV_DPX(5), 0);
    lv_obj_add_flag(hint, LV_OBJ_FLAG_IGNORE_LAYOUT);
    pad_mark_create(hint, PAD_MARK_VIEW, pad_family_current(), PLACE_HINT_MARK, lv_color_hex(0xE8E8E8));
    lv_obj_t *words = lv_label_create(hint);
    lv_obj_remove_style_all(words);
    lv_obj_set_style_text_font(words, lv_theme_get_font_small(s_panel), 0);
    lv_obj_set_style_text_color(words, lv_color_hex(0xFFFFFF), 0);
    lv_label_set_text(words, s_top ? locstr("Move to bottom") : locstr("Move to top"));
    lv_obj_align(hint, LV_ALIGN_TOP_RIGHT, -LV_DPX(10), -LV_DPX(6));
    s_hint = hint;
}

static void place_apply(void) {
    if (s_panel != NULL) {
        lv_obj_align(s_panel, s_top ? LV_ALIGN_TOP_MID : LV_ALIGN_BOTTOM_MID, 0, 0);
    }
    hint_show();
}

static void panel_deleted(lv_event_t *event) {
    (void) event;
    s_panel = NULL;
    s_hint = NULL;
}

void keyboard_place_attach(lv_obj_t *panel) {
    s_panel = panel;
    s_hint = NULL;
    lv_obj_add_event_cb(panel, panel_deleted, LV_EVENT_DELETE, NULL);
    place_apply();
}

bool keyboard_place_button(SDL_JoystickID which, Uint8 button, bool pressed) {
    if (pressed) {
        pad_family_note(which);
    }
    if (button != SDL_CONTROLLER_BUTTON_BACK) {
        return false;
    }
    if (pressed) {
        s_top = !s_top;
        place_apply();
    }
    return true;
}

const char *keyboard_place_name(void) {
    return s_top ? "top" : "bottom";
}
