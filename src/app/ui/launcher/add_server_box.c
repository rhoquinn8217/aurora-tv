/* The add-a-computer box's start, its clear button, and a controller's number
 * pad. The why is in the header. */

#include "add_server_box.h"

#include "app.h"
#include "lvgl/font/material_icons_regular_symbols.h"
#include "util/font.h"   /* app_fonts_t, which the theme's header names without declaring */
#include "lvgl/theme/lv_theme_moonlight.h"
#include "ui/common/keyboard_text.h"
#include "ui/common/pad_marks.h"
#include "ui/ui_input.h"
#include "util/i18n.h"

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>

/* The stream keyboard's keys and its focus (soft_keyboard.c: KBD_KEY_BG,
 * KBD_KEY_PRESS, KBD_FOCUS_PURPLE), so the two read as one family. */
#define KEY_BG      0x2C2C2C
#define KEY_PRESS   0x3A3A3A
#define KEY_TEXT    0xEBEBEB
#define FOCUS       0x8B5CF6
#define LEGEND_TEXT 0xBDBDC2

/* The number pad: three across, Bksp beside 0, and the keyboard a row of its own.
 * Its keys by the button matrix's count, which skips the row breaks. */
#define PAD_BKSP     11
#define PAD_KEYBOARD 12
#define PAD_ROWS     5
static const char *s_pad_map[] = {"1", "2", "3", "\n", "4", "5", "6", "\n", "7", "8", "9", "\n",
                                  ".", "0", "Bksp", "\n", "Keyboard", ""};
#define PAD_MAP_KEYBOARD 16

typedef struct {
    lv_obj_t *window;
    lv_obj_t *box;
    lv_obj_t *clear;
    lv_obj_t *buttons;
    uint16_t ok;
    /* A controller's number pad and the line under it; NULL for a window opened
     * without one. */
    lv_obj_t *pad;
    lv_obj_t *legend;
    /* The key the pad selects when the d-pad comes back to it. */
    uint16_t back_to;
} box_parts_t;

/* The TV's own IPv4 address without its last number ("192.168.1."): a cable's
 * first, then Wi-Fi's, then any other. Not loopback, and not Wi-Fi Direct
 * (p2p0), which holds an address of its own for screen sharing. */
static bool own_network_start(char *out, size_t len) {
    struct ifaddrs *list = NULL;
    if (getifaddrs(&list) != 0) {
        return false;
    }
    static const char *const kinds[] = {"eth", "wlan", ""};
    bool found = false;
    for (size_t k = 0; k < sizeof(kinds) / sizeof(kinds[0]) && !found; k++) {
        for (const struct ifaddrs *it = list; it != NULL; it = it->ifa_next) {
            if (it->ifa_addr == NULL || it->ifa_addr->sa_family != AF_INET || it->ifa_name == NULL ||
                (it->ifa_flags & IFF_UP) == 0 || (it->ifa_flags & IFF_LOOPBACK) != 0 ||
                strncmp(it->ifa_name, "p2p", 3) == 0 || strncmp(it->ifa_name, kinds[k], strlen(kinds[k])) != 0) {
                continue;
            }
            /* In network order, so its bytes read as written. */
            const uint8_t *b = (const uint8_t *) &((const struct sockaddr_in *) it->ifa_addr)->sin_addr.s_addr;
            snprintf(out, len, "%u.%u.%u.", b[0], b[1], b[2]);
            found = true;
            break;
        }
    }
    freeifaddrs(list);
    return found;
}

/* After upstream's own check (add.dialog.c, input_changed_cb), which ran first. */
static void box_changed(lv_event_t *event) {
    lv_obj_t *box = lv_event_get_current_target(event);
    const box_parts_t *parts = lv_event_get_user_data(event);
    const char *text = lv_textarea_get_text(box);
    const size_t len = text != NULL ? strlen(text) : 0;
    if (len > 0) {
        lv_obj_clear_flag(parts->clear, LV_OBJ_FLAG_HIDDEN);
    } else {
        /* The d-pad had it: the pad takes the focus as it goes. */
        if (parts->pad != NULL && lv_obj_has_state(parts->clear, LV_STATE_FOCUSED)) {
            lv_group_focus_obj(parts->pad);
        }
        lv_obj_add_flag(parts->clear, LV_OBJ_FLAG_HIDDEN);
    }
    if (len == 0 || text[len - 1] == '.') {
        lv_btnmatrix_set_btn_ctrl(parts->buttons, parts->ok, LV_BTNMATRIX_CTRL_DISABLED);
    }
}

static void clear_clicked(lv_event_t *event) {
    /* Its box sends value-changed, which hides the button and turns OK off. */
    const box_parts_t *parts = lv_event_get_user_data(event);
    lv_textarea_set_text(parts->box, "");
}

/* Circle, as Cancel: the window closes and adds nothing. */
static void window_cancel(lv_event_t *event) {
    const box_parts_t *parts = lv_event_get_user_data(event);
    lv_msgbox_close_async(parts->window);
}

static void clear_key(lv_event_t *event) {
    box_parts_t *parts = lv_event_get_user_data(event);
    if (lv_event_get_key(event) == LV_KEY_DOWN && parts->pad != NULL) {
        parts->back_to = 2;   /* the 3, under it */
        lv_group_focus_obj(parts->pad);
    }
}

static void keyboard_closed(bool enter, void *user) {
    box_parts_t *parts = user;
    lv_obj_clear_flag(parts->pad, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(parts->legend, LV_OBJ_FLAG_HIDDEN);
    lv_obj_center(parts->window);
    /* Back on the Keyboard key, now or from Cancel and OK. */
    parts->back_to = PAD_KEYBOARD;
    if (enter) {
        lv_group_focus_obj(parts->buttons);
        lv_btnmatrix_set_selected_btn(parts->buttons, parts->ok);
    } else {
        lv_group_focus_obj(parts->pad);
    }
}

static void pad_pressed(lv_event_t *event) {
    box_parts_t *parts = lv_event_get_user_data(event);
    const uint16_t id = lv_btnmatrix_get_selected_btn(parts->pad);
    if (id == PAD_KEYBOARD) {
        if (!keyboard_text_shown()) {
            /* Out of the way, so the window fits above or below the keyboard. Not
             * focused-looking first: LVGL hands a hidden object's focus on, and
             * Cancel lit up under the keyboard (Build 653). The window's focus
             * stays on the pad, for when the keyboard goes. */
            lv_obj_clear_state(parts->pad, LV_STATE_FOCUSED | LV_STATE_FOCUS_KEY);
            lv_obj_add_flag(parts->pad, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(parts->legend, LV_OBJ_FLAG_HIDDEN);
            keyboard_text_open(parts->box, parts->window, keyboard_closed, parts);
        }
    } else if (id == PAD_BKSP) {
        lv_textarea_del_char(parts->box);
    } else if (id != LV_BTNMATRIX_BTN_NONE) {
        const char *text = lv_btnmatrix_get_btn_text(parts->pad, id);
        if (text != NULL) {
            lv_textarea_add_text(parts->box, text);
        }
    }
}

/* Before the pad moves its own selection: from its top row Up is the ×, and
 * from the Keyboard key Down is Cancel and OK. A key used here goes no further,
 * so the pad it left does not move a selection it no longer shows. */
static void pad_key(lv_event_t *event) {
    box_parts_t *parts = lv_event_get_user_data(event);
    const uint32_t key = lv_event_get_key(event);
    const uint16_t id = lv_btnmatrix_get_selected_btn(parts->pad);
    if (key == LV_KEY_UP && id != LV_BTNMATRIX_BTN_NONE && id < 3) {
        if (!lv_obj_has_flag(parts->clear, LV_OBJ_FLAG_HIDDEN)) {
            lv_group_focus_obj(parts->clear);
            lv_event_stop_processing(event);
        }
    } else if (key == LV_KEY_DOWN && id == PAD_KEYBOARD) {
        parts->back_to = PAD_KEYBOARD;
        lv_group_focus_obj(parts->buttons);
        lv_event_stop_processing(event);
    } else if (key == LV_KEY_DEL || key == LV_KEY_BACKSPACE) {
        /* Square, or a keyboard's Backspace: one character back. */
        lv_textarea_del_char(parts->box);
        lv_event_stop_processing(event);
    }
}

/* Back on the pad from the d-pad: on the key nearest where it came from. */
static void pad_focused(lv_event_t *event) {
    box_parts_t *parts = lv_event_get_user_data(event);
    lv_indev_t *indev = lv_event_get_param(event);
    if (indev == NULL || lv_indev_get_type(indev) == LV_INDEV_TYPE_KEYPAD) {
        lv_btnmatrix_set_selected_btn(parts->pad, parts->back_to);
    }
    parts->back_to = 0;
}

/* A pointer on the box of a window opened with a controller: webOS's keyboard,
 * as in a window opened without one. */
static void box_clicked(lv_event_t *event) {
    const box_parts_t *parts = lv_event_get_user_data(event);
    lv_group_t *group = lv_obj_get_group(parts->buttons);
    if (parts->pad != NULL && group != NULL && lv_obj_get_group(parts->box) == NULL) {
        lv_group_add_obj(group, parts->box);
        lv_group_focus_obj(parts->box);
    }
}

static void legend_item(lv_obj_t *legend, pad_mark_t mark, const char *words) {
    lv_obj_t *item = lv_obj_create(legend);
    lv_obj_remove_style_all(item);
    lv_obj_clear_flag(item, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(item, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(item, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(item, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(item, LV_DPX(4), 0);
    pad_mark_create(item, mark, pad_family_current(), LV_DPX(14), lv_color_hex(LEGEND_TEXT));
    lv_obj_t *label = lv_label_create(item);
    lv_obj_set_style_text_font(label, lv_theme_get_font_small(legend), 0);
    lv_obj_set_style_text_color(label, lv_color_hex(LEGEND_TEXT), 0);
    lv_label_set_text_static(label, words);
}

static void pad_style(lv_obj_t *pad) {
    lv_obj_set_style_bg_opa(pad, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(pad, 0, 0);
    lv_obj_set_style_outline_width(pad, 0, LV_STATE_FOCUSED);
    lv_obj_set_style_outline_width(pad, 0, LV_STATE_FOCUS_KEY);
    lv_obj_set_style_pad_all(pad, 0, 0);
    lv_obj_set_style_pad_gap(pad, LV_DPX(6), 0);
    lv_obj_set_style_radius(pad, LV_DPX(8), LV_PART_ITEMS);
    lv_obj_set_style_bg_color(pad, lv_color_hex(KEY_BG), LV_PART_ITEMS);
    lv_obj_set_style_bg_opa(pad, LV_OPA_COVER, LV_PART_ITEMS);
    lv_obj_set_style_border_color(pad, lv_color_hex(KEY_PRESS), LV_PART_ITEMS);
    lv_obj_set_style_border_width(pad, LV_DPX(1), LV_PART_ITEMS);
    lv_obj_set_style_text_color(pad, lv_color_hex(KEY_TEXT), LV_PART_ITEMS);
    lv_obj_set_style_bg_color(pad, lv_color_hex(KEY_PRESS), LV_PART_ITEMS | LV_STATE_PRESSED);
    /* The selected key: a purple edge on its own fill, as the stream keyboard's. */
    lv_obj_set_style_bg_color(pad, lv_color_hex(KEY_BG), LV_PART_ITEMS | LV_STATE_FOCUSED);
    lv_obj_set_style_bg_color(pad, lv_color_hex(KEY_BG), LV_PART_ITEMS | LV_STATE_FOCUS_KEY);
    lv_obj_set_style_border_color(pad, lv_color_hex(FOCUS), LV_PART_ITEMS | LV_STATE_FOCUSED);
    lv_obj_set_style_border_color(pad, lv_color_hex(FOCUS), LV_PART_ITEMS | LV_STATE_FOCUS_KEY);
    lv_obj_set_style_border_width(pad, LV_DPX(3), LV_PART_ITEMS | LV_STATE_FOCUSED);
    lv_obj_set_style_border_width(pad, LV_DPX(3), LV_PART_ITEMS | LV_STATE_FOCUS_KEY);
    /* And no more: the theme's blue outline around it too made two halos, the
     * blue one cut off at the pad's edge (rhoquinn8217, 2026-11-06: "Keep the
     * purple one becaue the blue one clips outs"). */
    lv_obj_set_style_outline_width(pad, 0, LV_PART_ITEMS | LV_STATE_FOCUSED);
    lv_obj_set_style_outline_width(pad, 0, LV_PART_ITEMS | LV_STATE_FOCUS_KEY);
}

/* Under the box, in two rows more of upstream's grid: the pad, then what its
 * buttons do. In the window's focus order the box makes way for the ×, the pad,
 * then Cancel and OK, and the pad takes the focus with 1 selected. */
static void pad_create(box_parts_t *parts) {
    lv_obj_t *content = lv_obj_get_parent(parts->box);
    static const lv_coord_t rows[] = {LV_GRID_CONTENT, LV_GRID_CONTENT, LV_GRID_CONTENT, LV_GRID_CONTENT,
                                      LV_GRID_CONTENT, LV_GRID_TEMPLATE_LAST};
    lv_obj_set_grid_dsc_array(content, lv_obj_get_style_grid_column_dsc_array(content, 0), rows);

    s_pad_map[PAD_MAP_KEYBOARD] = locstr("Keyboard");
    lv_obj_t *pad = lv_btnmatrix_create(content);
    lv_btnmatrix_set_map(pad, s_pad_map);
    lv_btnmatrix_set_btn_ctrl(pad, PAD_KEYBOARD, LV_BTNMATRIX_CTRL_NO_REPEAT);
    lv_obj_set_grid_cell(pad, LV_GRID_ALIGN_CENTER, 0, 3, LV_GRID_ALIGN_START, 3, 1);
    lv_obj_set_size(pad, LV_DPX(220), PAD_ROWS * LV_DPX(26) + (PAD_ROWS - 1) * LV_DPX(6));
    pad_style(pad);
    parts->pad = pad;

    lv_obj_t *legend = lv_obj_create(content);
    lv_obj_remove_style_all(legend);
    lv_obj_clear_flag(legend, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(legend, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(legend, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(legend, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(legend, LV_DPX(14), 0);
    lv_obj_set_grid_cell(legend, LV_GRID_ALIGN_CENTER, 0, 3, LV_GRID_ALIGN_START, 4, 1);
    legend_item(legend, PAD_MARK_SOUTH, locstr("type"));
    legend_item(legend, PAD_MARK_WEST, locstr("delete"));
    legend_item(legend, PAD_MARK_EAST, locstr("cancel"));
    parts->legend = legend;

    lv_obj_add_event_cb(pad, pad_pressed, LV_EVENT_VALUE_CHANGED, parts);
    lv_obj_add_event_cb(pad, pad_key, LV_EVENT_KEY | LV_EVENT_PREPROCESS, parts);
    lv_obj_add_event_cb(pad, pad_focused, LV_EVENT_FOCUSED, parts);
    lv_obj_add_event_cb(pad, window_cancel, LV_EVENT_CANCEL, parts);

    /* The × for the d-pad: a purple ring while it has the focus. */
    lv_obj_set_style_outline_color(parts->clear, lv_color_hex(FOCUS), LV_STATE_FOCUSED);
    lv_obj_set_style_outline_width(parts->clear, LV_DPX(3), LV_STATE_FOCUSED);
    lv_obj_set_style_outline_pad(parts->clear, LV_DPX(2), LV_STATE_FOCUSED);
    lv_obj_add_event_cb(parts->clear, clear_key, LV_EVENT_KEY, parts);
    lv_obj_add_event_cb(parts->clear, window_cancel, LV_EVENT_CANCEL, parts);
    lv_obj_add_event_cb(parts->box, box_clicked, LV_EVENT_CLICKED, parts);

    /* The box out of the focus order, or the d-pad would open webOS's keyboard on
     * it; it still looks like where the typing goes, with its cursor. */
    lv_group_t *group = lv_obj_get_group(parts->buttons);
    if (group != NULL) {
        lv_group_remove_obj(parts->box);
        lv_group_remove_obj(parts->buttons);
        lv_group_add_obj(group, parts->clear);
        lv_group_add_obj(group, pad);
        lv_group_add_obj(group, parts->buttons);
        lv_group_focus_obj(pad);
    }
    lv_obj_add_state(parts->box, LV_STATE_FOCUSED);
}

static void box_deleted(lv_event_t *event) {
    box_parts_t *parts = lv_event_get_user_data(event);
    /* The window going while aurora-tv's keyboard is up over it: the keyboard
     * goes too, and nothing is told about it. */
    if (parts->pad != NULL && keyboard_text_shown()) {
        keyboard_text_drop();
    }
    lv_mem_free(parts);
}

void add_server_box(lv_obj_t *box, lv_obj_t *buttons, uint16_t ok) {
    if (box == NULL || buttons == NULL) {
        return;
    }
    box_parts_t *parts = lv_mem_alloc(sizeof(box_parts_t));
    if (parts == NULL) {
        return;
    }
    memset(parts, 0, sizeof(*parts));
    /* The content's parent: the window. */
    parts->window = lv_obj_get_parent(lv_obj_get_parent(box));
    parts->box = box;
    /* A plain object, not a button: the window's focus stays on its buttons,
     * and the keys walk them as before; this one is for a pointer, and for the
     * d-pad in a window opened with a controller. Floating, so it keeps its
     * place while the box's one line scrolls. */
    const lv_coord_t size = LV_DPX(22);
    lv_obj_t *clear = lv_obj_create(box);
    lv_obj_remove_style_all(clear);
    lv_obj_add_flag(clear, LV_OBJ_FLAG_FLOATING | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(clear, LV_OBJ_FLAG_CLICK_FOCUSABLE | LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    lv_obj_set_size(clear, size, size);
    lv_obj_set_style_radius(clear, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(clear, lv_color_hex(0x58585C), 0);
    lv_obj_set_style_bg_color(clear, lv_color_hex(0x707076), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(clear, LV_OPA_COVER, 0);
    lv_obj_t *mark = lv_label_create(clear);
    lv_obj_set_style_text_font(mark, lv_theme_moonlight_get_iconfont_small(box), 0);
    lv_obj_set_style_text_color(mark, lv_color_hex(0xEBEBEB), 0);
    lv_label_set_text_static(mark, MAT_SYMBOL_CLOSE);
    lv_obj_center(mark);
    /* The text stops short of it: the box's right padding grows by the button
     * and a gap, and the button sits in that growth, as far from the box's edge
     * as the text was. Aligned to the content's right end, so moved out by as
     * much as the padding grew. */
    const lv_coord_t gap = LV_DPX(8);
    lv_obj_set_style_pad_right(box, lv_obj_get_style_pad_right(box, LV_PART_MAIN) + size + gap, 0);
    lv_obj_align(clear, LV_ALIGN_RIGHT_MID, size + gap, 0);
    lv_obj_add_event_cb(clear, clear_clicked, LV_EVENT_CLICKED, parts);

    parts->clear = clear;
    parts->buttons = buttons;
    parts->ok = ok;
    lv_obj_add_event_cb(box, box_changed, LV_EVENT_VALUE_CHANGED, parts);
    lv_obj_add_event_cb(box, box_deleted, LV_EVENT_DELETE, parts);

    /* The cursor ends after it. Setting it sends value-changed, so OK and the
     * button are right from the start. */
    char start[20];
    if (own_network_start(start, sizeof(start))) {
        lv_textarea_set_text(box, start);
    } else {
        lv_obj_add_flag(clear, LV_OBJ_FLAG_HIDDEN);
    }

    /* Opened with a controller: its own number pad, which a controller can use. */
    if (global != NULL && app_ui_get_input_mode(&global->ui.input) == UI_INPUT_MODE_GAMEPAD) {
        pad_create(parts);
    }
}
