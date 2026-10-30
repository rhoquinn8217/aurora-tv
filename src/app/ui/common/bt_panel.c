/* The Bluetooth controllers panel. The why and where are in the header. */

#include "bt_panel.h"

#if defined(TARGET_WEBOS)

#include <stdio.h>
#include <string.h>

#include "app.h"
#include "app_settings.h"   /* the check box's setting, saved at once */
#include "ui/ui_input.h"
#include "util/i18n.h"
#include "input/bt_pairing.h"
#include "lvgl/font/material_icons_regular_symbols.h"
#include "lvgl/theme/lv_theme_moonlight.h"
#include "lvgl/theme/lv_theme_moonlight_colors.h"

/* ⓘ The USB Bridge panel's palette, as the Auto Bridge window has it, so the
 * three read as one family; and red for what forgets. */
#define BTP_COL_CARD    lv_color_hex(0x12181d)
#define BTP_COL_ROW     lv_color_hex(0x1a232b)
#define BTP_COL_FOCUS   lv_color_hex(0x2563eb)
#define BTP_COL_BORDER  lv_color_hex(0x2a3540)
#define BTP_COL_TXT     lv_color_hex(0xf5f8fa)
#define BTP_COL_SUB     lv_color_hex(0x95a3b0)
#define BTP_COL_ON      lv_color_hex(0x4ade80)
#define BTP_COL_FORGET  lv_color_hex(0x3a2326)
#define BTP_COL_FORGET_TXT lv_color_hex(0xfca5a5)
#define BTP_COL_ALL     lv_palette_main(LV_PALETTE_RED)
#define BTP_COL_PAIR    lv_color_hex(0x1f3a2b)
#define BTP_COL_PAIR_TXT lv_color_hex(0x86efac)

/* The top bar's icon size, as launcher.view.c and stream_actions.c have it. */
#define TOPBAR_BTN_SIZE 40

static lv_obj_t   *s_win;
static lv_obj_t   *s_card;
static lv_group_t *s_group;
static lv_obj_t   *s_close;
static lv_obj_t   *s_count;
static lv_obj_t   *s_list;
static lv_obj_t   *s_all;
static lv_obj_t   *s_search;
static lv_obj_t   *s_switch;
static lv_obj_t   *s_knob;
static lv_obj_t   *s_status;
static lv_obj_t   *s_auto_row;
static lv_obj_t   *s_auto_box;
/* The registered devices' Forget buttons, and the found ones' Pair buttons. */
static lv_obj_t   *s_forget[BT_PAIRING_MAX];
static char        s_addr[BT_PAIRING_MAX][24];
static int         s_rows;
static lv_obj_t   *s_pair[BT_PAIRING_MAX];
static char        s_found_addr[BT_PAIRING_MAX][24];
static int         s_found_rows;

static void btp_close_cb(lv_event_t *e) {
    LV_UNUSED(e);
    bt_panel_close();
}

/* ⭐ LEFT AND RIGHT MOVE TOO, as in the Auto Bridge window: Forget all and the
 * Search switch sit side by side, and the key that points that way should go
 * there. */
static void btp_key_cb(lv_event_t *e) {
    switch (lv_event_get_key(e)) {
        case LV_KEY_UP:
        case LV_KEY_LEFT:
            lv_group_focus_prev(s_group);
            lv_obj_scroll_to_view(lv_group_get_focused(s_group), LV_ANIM_ON);
            break;
        case LV_KEY_DOWN:
        case LV_KEY_RIGHT:
            lv_group_focus_next(s_group);
            lv_obj_scroll_to_view(lv_group_get_focused(s_group), LV_ANIM_ON);
            break;
        default:
            break;
    }
}

static void btp_keys(lv_obj_t *obj) {
    lv_obj_add_event_cb(obj, btp_key_cb, LV_EVENT_KEY, NULL);
    lv_obj_add_event_cb(obj, btp_close_cb, LV_EVENT_CANCEL, NULL);
}

static void btp_forget_cb(lv_event_t *e) {
    const int i = (int) (intptr_t) lv_event_get_user_data(e);
    if (i >= 0 && i < s_rows) {
        bt_pairing_forget(s_addr[i]);
    }
}

static void btp_forget_all_cb(lv_event_t *e) {
    LV_UNUSED(e);
    if (s_rows > 0) {
        bt_pairing_forget(NULL);
    }
}

static void btp_search_cb(lv_event_t *e) {
    LV_UNUSED(e);
    bt_pairing_search(!lv_obj_has_state(s_switch, LV_STATE_CHECKED));
}

static void btp_auto_look(void) {
    if (app_configuration->bridge.bt_auto_pair) {
        lv_obj_add_state(s_auto_box, LV_STATE_CHECKED);
    } else {
        lv_obj_clear_state(s_auto_box, LV_STATE_CHECKED);
    }
}

/* Saved at once, as Bridge Override's answer was: a TV that closes the app
 * without leaving Settings would otherwise forget it. */
static void btp_auto_cb(lv_event_t *e) {
    LV_UNUSED(e);
    app_configuration->bridge.bt_auto_pair = !app_configuration->bridge.bt_auto_pair;
    settings_save(app_configuration);
    btp_auto_look();
}

static lv_obj_t *btp_text(lv_obj_t *parent, const char *text, lv_color_t color, const lv_font_t *font, bool wrap) {
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, text);
    lv_label_set_long_mode(l, wrap ? LV_LABEL_LONG_WRAP : LV_LABEL_LONG_DOT);
    lv_obj_set_width(l, LV_PCT(100));
    lv_obj_set_style_text_color(l, color, 0);
    lv_obj_set_style_text_font(l, font, 0);
    return l;
}

/* A button of the panel's: the colour it rests in, blue when selected. */
static lv_obj_t *btp_button(lv_obj_t *parent, lv_color_t bg, lv_color_t fg, const char *text) {
    lv_obj_t *b = lv_btn_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_hor(b, LV_DPX(14), 0);
    lv_obj_set_style_pad_ver(b, LV_DPX(8), 0);
    lv_obj_set_style_radius(b, LV_DPX(6), 0);
    lv_obj_set_style_bg_color(b, bg, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(b, BTP_COL_FOCUS, LV_STATE_FOCUS_KEY);
    lv_obj_set_style_text_color(b, fg, 0);
    lv_obj_set_style_text_color(b, BTP_COL_TXT, LV_STATE_FOCUS_KEY);
    if (text != NULL) {
        lv_obj_t *l = lv_label_create(b);
        lv_label_set_text(l, text);
        lv_obj_center(l);
    }
    btp_keys(b);
    return b;
}

static void btp_pair_cb(lv_event_t *e) {
    const int i = (int) (intptr_t) lv_event_get_user_data(e);
    if (i >= 0 && i < s_found_rows) {
        bt_pairing_pair(s_found_addr[i]);
    }
}

/* One device: the controller icon or its space, its name, a line under it,
 * and one button, which this returns. */
static lv_obj_t *btp_row(const char *name, bool gamepad, const char *line, lv_color_t line_color,
                         lv_color_t bg, lv_color_t fg, const char *button) {
    lv_obj_t *row = lv_obj_create(s_list);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(row, LV_DPX(8), 0);
    lv_obj_set_style_pad_gap(row, LV_DPX(10), 0);
    lv_obj_set_style_radius(row, LV_DPX(6), 0);
    lv_obj_set_style_bg_color(row, BTP_COL_ROW, 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(row, LV_DPX(1), 0);
    lv_obj_set_style_border_color(row, BTP_COL_BORDER, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    /* A controller shows the controller icon; anything else, the same space. */
    lv_obj_t *icon = lv_label_create(row);
    lv_obj_set_width(icon, LV_DPX(24));
    lv_obj_set_style_text_font(icon, lv_theme_moonlight_get_iconfont_small(row), 0);
    lv_obj_set_style_text_color(icon, BTP_COL_TXT, 0);
    lv_label_set_text_static(icon, gamepad ? MAT_SYMBOL_SPORTS_ESPORTS : "");

    lv_obj_t *col = lv_obj_create(row);
    lv_obj_remove_style_all(col);
    lv_obj_set_width(col, 1);
    lv_obj_set_flex_grow(col, 1);
    lv_obj_set_height(col, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_gap(col, LV_DPX(2), 0);
    lv_obj_clear_flag(col, LV_OBJ_FLAG_SCROLLABLE);
    btp_text(col, name, BTP_COL_TXT, lv_theme_get_font_normal(col), false);
    btp_text(col, line, line_color, lv_theme_get_font_small(col), false);

    return btp_button(row, bg, fg, button);
}

/* A registered device: whether it is connected, and Forget. */
static void btp_device_row(int i, const bt_pairing_device_t *dev) {
    lv_obj_t *forget = btp_row(dev->name, dev->gamepad, dev->connected ? locstr("Connected") : locstr("Not connected"),
                               dev->connected ? BTP_COL_ON : BTP_COL_SUB, BTP_COL_FORGET, BTP_COL_FORGET_TXT,
                               locstr("Forget"));
    lv_obj_add_event_cb(forget, btp_forget_cb, LV_EVENT_CLICKED, (void *) (intptr_t) i);
    snprintf(s_addr[i], sizeof s_addr[i], "%s", dev->addr);
    s_forget[i] = forget;
}

/* ⭐ A device the search found and did not pair by itself: Pair (rhoquinn8217,
 * 2026-10-29). Once it has paired, it is a registered device with Forget. */
static void btp_found_row(int i, const bt_pairing_found_t *f) {
    lv_obj_t *pair = btp_row(f->name, f->gamepad, f->pairing ? locstr("Pairing...") : locstr("Found, not paired"),
                             f->pairing ? BTP_COL_PAIR_TXT : BTP_COL_SUB, BTP_COL_PAIR, BTP_COL_PAIR_TXT,
                             locstr("Pair"));
    lv_obj_add_event_cb(pair, btp_pair_cb, LV_EVENT_CLICKED, (void *) (intptr_t) i);
    snprintf(s_found_addr[i], sizeof s_found_addr[i], "%s", f->addr);
    s_pair[i] = pair;
}

/* ⭐ THE LIST TAKES ITS ROWS' ROOM, and scrolls only when the window would run
 * off the screen: then the window takes its full height and the list what the
 * rest leaves. The Auto Bridge window's answer to its cut-off buttons; a fixed
 * share for the list cut the C1's three rows at two and a half (2026-10-29). */
static void btp_fit(void) {
    lv_obj_set_height(s_card, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(s_list, 0);
    lv_obj_set_height(s_list, LV_SIZE_CONTENT);
    lv_obj_update_layout(s_card);
    lv_obj_t *last = lv_obj_get_child(s_card, -1);
    if (last == NULL) {
        return;
    }
    lv_area_t card_area;
    lv_area_t last_area;
    lv_obj_get_coords(s_card, &card_area);
    lv_obj_get_coords(last, &last_area);
    const lv_coord_t inner_bottom = card_area.y2 - lv_obj_get_style_pad_bottom(s_card, 0) -
                                    lv_obj_get_style_border_width(s_card, 0);
    if (last_area.y2 > inner_bottom) {
        lv_obj_set_height(s_card, lv_obj_get_style_max_height(s_card, 0));
        lv_obj_set_height(s_list, 1);
        lv_obj_set_flex_grow(s_list, 1);
    }
}

/* ⭐ Built again from the state at every change, with the selection kept on
 * what it was on: the same device's button (a found device that has paired
 * lands on its Forget), the row that took its place, or the same button. The
 * group is refilled in the order the panel reads, top to bottom, so Up and Down
 * walk it as it is drawn. */
static void btp_refresh(void) {
    if (s_win == NULL) {
        return;
    }
    bt_pairing_state_t st;
    bt_pairing_state(&st);

    lv_obj_t *focused = lv_group_get_focused(s_group);
    char was_addr[24] = "";
    int was_row = -1;
    bool was_found = false;
    for (int i = 0; i < s_rows; i++) {
        if (focused == s_forget[i]) {
            was_row = i;
            snprintf(was_addr, sizeof was_addr, "%s", s_addr[i]);
        }
    }
    for (int i = 0; i < s_found_rows; i++) {
        if (focused == s_pair[i]) {
            was_row = i;
            was_found = true;
            snprintf(was_addr, sizeof was_addr, "%s", s_found_addr[i]);
        }
    }

    lv_group_remove_all_objs(s_group);
    lv_obj_clean(s_list);
    memset(s_forget, 0, sizeof s_forget);
    memset(s_pair, 0, sizeof s_pair);
    s_rows = 0;
    s_found_rows = 0;
    for (int i = 0; i < st.count && i < BT_PAIRING_MAX; i++) {
        btp_device_row(i, &st.devices[i]);
        s_rows++;
    }
    if (st.count == 0) {
        btp_text(s_list, st.listed ? locstr("Nothing is registered on this TV.")
                                   : locstr("Asking the TV for its devices..."),
                 BTP_COL_SUB, lv_theme_get_font_small(s_list), true);
    }
    if (st.found_count > 0) {
        btp_text(s_list, locstr("Found by the search"), BTP_COL_SUB, lv_theme_get_font_small(s_list), false);
        for (int i = 0; i < st.found_count && i < BT_PAIRING_MAX; i++) {
            btp_found_row(i, &st.found[i]);
            s_found_rows++;
        }
    }

    char count[80];
    snprintf(count, sizeof count, "%s %d", locstr("Registered on this TV:"), st.count);
    lv_label_set_text(s_count, count);
    /* ⓘ Hidden while there is nothing to say, so it leaves no empty line. No
     * standing hint in its place (rhoquinn8217, 2026-10-29: "Remove the
     * 'Forget a controller that will not pair...' sentences"). */
    lv_label_set_text(s_status, st.status);
    if (st.status[0] == '\0') {
        lv_obj_add_flag(s_status, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_clear_flag(s_status, LV_OBJ_FLAG_HIDDEN);
    }
    btp_fit();

    if (st.searching) {
        lv_obj_add_state(s_switch, LV_STATE_CHECKED);
        lv_obj_align(s_knob, LV_ALIGN_RIGHT_MID, -LV_DPX(3), 0);
    } else {
        lv_obj_clear_state(s_switch, LV_STATE_CHECKED);
        lv_obj_align(s_knob, LV_ALIGN_LEFT_MID, LV_DPX(3), 0);
    }
    const bool can_forget = st.count > 0 && !st.searching;
    lv_obj_set_style_opa(s_all, can_forget ? LV_OPA_COVER : LV_OPA_40, 0);
    if (can_forget) {
        lv_obj_clear_state(s_all, LV_STATE_DISABLED);
    } else {
        lv_obj_add_state(s_all, LV_STATE_DISABLED);
    }

    lv_group_add_obj(s_group, s_close);
    for (int i = 0; i < s_rows; i++) {
        lv_group_add_obj(s_group, s_forget[i]);
    }
    for (int i = 0; i < s_found_rows; i++) {
        lv_group_add_obj(s_group, s_pair[i]);
    }
    lv_group_add_obj(s_group, s_all);
    lv_group_add_obj(s_group, s_search);
    lv_group_add_obj(s_group, s_auto_row);
    btp_auto_look();

    lv_obj_t *target = NULL;
    if (was_row >= 0) {
        /* The same device, wherever it is now: a found one that paired is a
         * registered one. */
        for (int i = 0; i < s_rows && target == NULL; i++) {
            if (strcmp(s_addr[i], was_addr) == 0) {
                target = s_forget[i];
            }
        }
        for (int i = 0; i < s_found_rows && target == NULL; i++) {
            if (strcmp(s_found_addr[i], was_addr) == 0) {
                target = s_pair[i];
            }
        }
        if (target == NULL && was_found) {
            target = was_row < s_found_rows ? s_pair[was_row]
                                            : (s_found_rows > 0 ? s_pair[s_found_rows - 1] : s_search);
        } else if (target == NULL) {
            target = was_row < s_rows ? s_forget[was_row] : (s_rows > 0 ? s_forget[s_rows - 1] : s_search);
        }
    } else if (focused == s_close || focused == s_all || focused == s_search || focused == s_auto_row) {
        target = focused;
    } else {
        target = s_search;
    }
    lv_group_focus_obj(target);
}

static void btp_changed(void) {
    btp_refresh();
}

bool bt_panel_shown(void) {
    return s_win != NULL;
}

void bt_panel_close(void) {
    if (s_win == NULL) {
        return;
    }
    bt_pairing_set_listener(NULL);
    bt_pairing_state_t st;
    bt_pairing_state(&st);
    if (st.searching) {
        bt_pairing_search(false);
    }
    /* ⛔ Remove the modal group rather than restoring another: the stack owns
     * what is focused, and popping it hands the keys back (auto_bridge_window.c
     * says why). */
    app_input_remove_modal_group(&global->ui.input, s_group);
    lv_obj_del(s_win);
    s_win = NULL;
    if (s_group != NULL) {
        lv_group_del(s_group);
        s_group = NULL;
    }
    s_rows = 0;
    s_found_rows = 0;
}

void bt_panel_open(bool over_stream) {
    if (s_win != NULL) {
        return;
    }
    s_rows = 0;
    s_found_rows = 0;
    memset(s_forget, 0, sizeof s_forget);
    memset(s_pair, 0, sizeof s_pair);
    s_group = lv_group_create();

    /* ⓘ On the top layer, so it sits over the dashboard or the overlay without
     * being part of either. Over a stream the backdrop is clear: the picture
     * stays as it is, and the panel stands where the USB Bridge panel does. */
    s_win = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_win);
    lv_obj_set_size(s_win, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_win, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_win, over_stream ? LV_OPA_TRANSP : LV_OPA_50, 0);
    lv_obj_clear_flag(s_win, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_win, LV_OBJ_FLAG_CLICKABLE);   /* swallow clicks behind it */

    lv_obj_t *card = lv_obj_create(s_win);
    s_card = card;
    lv_obj_remove_style_all(card);
    /* ⓘ Wide enough for "DualSense Edge Wireless Controller" beside its Forget
     * (rhoquinn8217, 2026-10-29: "widen the panel for longer controller names"). */
    lv_obj_set_width(card, over_stream ? LV_PCT(52) : LV_PCT(62));
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_set_style_max_height(card, over_stream ? LV_PCT(82) : LV_PCT(80), 0);
    if (over_stream) {
        lv_obj_align(card, LV_ALIGN_TOP_RIGHT, -LV_DPX(16), LV_DPX(24));
    } else {
        lv_obj_center(card);
    }
    lv_obj_set_style_bg_color(card, BTP_COL_CARD, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, LV_DPX(10), 0);
    lv_obj_set_style_border_width(card, LV_DPX(1), 0);
    lv_obj_set_style_border_color(card, BTP_COL_BORDER, 0);
    lv_obj_set_style_pad_all(card, LV_DPX(16), 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_gap(card, LV_DPX(8), 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    /* Title, and the close corner pinned out of the flow, as the Auto Bridge
     * window has them. */
    lv_obj_t *titlerow = lv_obj_create(card);
    lv_obj_remove_style_all(titlerow);
    lv_obj_set_size(titlerow, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(titlerow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(titlerow, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(titlerow, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *title = lv_label_create(titlerow);
    lv_label_set_text(title, locstr("Bluetooth Devices"));
    lv_obj_set_style_text_color(title, BTP_COL_TXT, 0);
    lv_obj_set_style_text_font(title, lv_theme_get_font_large(titlerow), 0);

    s_close = lv_btn_create(titlerow);
    lv_obj_remove_style_all(s_close);
    lv_obj_set_size(s_close, LV_DPX(30), LV_DPX(30));
    lv_obj_set_style_radius(s_close, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_close, BTP_COL_ROW, 0);
    lv_obj_set_style_bg_opa(s_close, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_close, BTP_COL_FOCUS, LV_STATE_FOCUS_KEY);
    lv_obj_add_flag(s_close, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_align(s_close, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_t *xl = lv_label_create(s_close);
    lv_label_set_text_static(xl, MAT_SYMBOL_CLOSE);
    lv_obj_set_style_text_font(xl, lv_theme_moonlight_get_iconfont_small(titlerow), 0);
    lv_obj_set_style_text_color(xl, BTP_COL_TXT, 0);
    lv_obj_center(xl);
    lv_obj_add_event_cb(s_close, btp_close_cb, LV_EVENT_CLICKED, NULL);
    btp_keys(s_close);

    s_count = btp_text(card, "", BTP_COL_SUB, lv_theme_get_font_small(card), false);

    s_list = lv_obj_create(card);
    lv_obj_remove_style_all(s_list);
    lv_obj_set_width(s_list, LV_PCT(100));
    lv_obj_set_height(s_list, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_gap(s_list, LV_DPX(6), 0);
    lv_obj_set_scrollbar_mode(s_list, LV_SCROLLBAR_MODE_AUTO);

    /* The foot: Forget all on the left, the Search switch on the right. */
    lv_obj_t *foot = lv_obj_create(card);
    lv_obj_remove_style_all(foot);
    lv_obj_set_size(foot, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(foot, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(foot, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(foot, LV_OBJ_FLAG_SCROLLABLE);

    s_all = btp_button(foot, BTP_COL_ALL, BTP_COL_TXT, locstr("Forget all"));
    lv_obj_add_event_cb(s_all, btp_forget_all_cb, LV_EVENT_CLICKED, NULL);

    /* ⭐ Over a stream, the note stands between the two buttons (rhoquinn8217,
     * 2026-10-29). Nothing is bridged on the dashboard, so nothing stands there. */
    if (over_stream) {
        lv_obj_t *note = lv_label_create(foot);
        lv_label_set_text(note, locstr("Forgetting a bridged device releases it."));
        lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(note, 1);
        lv_obj_set_flex_grow(note, 1);
        lv_obj_set_style_text_align(note, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(note, BTP_COL_SUB, 0);
        lv_obj_set_style_text_font(note, lv_theme_get_font_small(foot), 0);
        lv_obj_set_style_pad_hor(note, LV_DPX(8), 0);
    }

    s_search = btp_button(foot, BTP_COL_ROW, BTP_COL_TXT, NULL);
    lv_obj_set_style_border_width(s_search, LV_DPX(1), 0);
    lv_obj_set_style_border_color(s_search, BTP_COL_BORDER, 0);
    lv_obj_set_flex_flow(s_search, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_search, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(s_search, LV_DPX(10), 0);
    lv_obj_t *sl = lv_label_create(s_search);
    lv_label_set_text(sl, over_stream ? locstr("Search") : locstr("Search for controllers"));
    /* ⓘ Drawn by hand, a track and a knob: this LVGL is built without its switch
     * widget. ⛔ Not clickable: its state is set from the search's, as the Auto
     * Bridge window's boxes are (a checkable one ticked itself as the selection
     * moved onto it). Green when on, so it still reads on the blue of a
     * selected button. */
    s_switch = lv_obj_create(s_search);
    lv_obj_remove_style_all(s_switch);
    lv_obj_set_size(s_switch, LV_DPX(40), LV_DPX(22));
    lv_obj_set_style_radius(s_switch, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(s_switch, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_switch, lv_color_hex(0x4b5563), 0);
    lv_obj_set_style_bg_color(s_switch, BTP_COL_ON, LV_STATE_CHECKED);
    lv_obj_clear_flag(s_switch, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    s_knob = lv_obj_create(s_switch);
    lv_obj_remove_style_all(s_knob);
    lv_obj_set_size(s_knob, LV_DPX(16), LV_DPX(16));
    lv_obj_set_style_radius(s_knob, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(s_knob, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_knob, lv_color_white(), 0);
    lv_obj_clear_flag(s_knob, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(s_knob, LV_ALIGN_LEFT_MID, LV_DPX(3), 0);
    lv_obj_add_event_cb(s_search, btp_search_cb, LV_EVENT_CLICKED, NULL);

    s_status = btp_text(card, "", BTP_COL_SUB, lv_theme_get_font_small(card), true);

    /* ⭐ THE CHECK BOX AT THE FOOT (rhoquinn8217, 2026-10-29): "Automatically pair
     * DualSense/DualSense Edge Controllers", on by default. A row with the box,
     * as the Auto Bridge window's are: ⛔ not checkable, its state set by hand,
     * since a checkable one ticks itself as the selection moves onto it. */
    s_auto_row = lv_obj_create(card);
    lv_obj_remove_style_all(s_auto_row);
    lv_obj_set_size(s_auto_row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(s_auto_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_auto_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(s_auto_row, LV_DPX(8), 0);
    lv_obj_set_style_pad_gap(s_auto_row, LV_DPX(8), 0);
    lv_obj_set_style_radius(s_auto_row, LV_DPX(6), 0);
    lv_obj_set_style_bg_color(s_auto_row, BTP_COL_ROW, 0);
    lv_obj_set_style_bg_opa(s_auto_row, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_auto_row, BTP_COL_FOCUS, LV_STATE_FOCUS_KEY);
    lv_obj_set_style_border_width(s_auto_row, LV_DPX(1), 0);
    lv_obj_set_style_border_color(s_auto_row, BTP_COL_BORDER, 0);
    lv_obj_clear_flag(s_auto_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_auto_row, LV_OBJ_FLAG_CLICKABLE);
    s_auto_box = lv_checkbox_create(s_auto_row);
    lv_checkbox_set_text(s_auto_box, "");
    lv_obj_clear_flag(s_auto_box, LV_OBJ_FLAG_CHECKABLE);
    lv_obj_clear_flag(s_auto_box, LV_OBJ_FLAG_CLICKABLE);
    /* ⓘ Out of every group: a check box joins the screen's default one as it is
     * made, and over the overlay that drew its focus ring (2026-10-29, the C1).
     * The row is what the keys reach. */
    lv_group_remove_obj(s_auto_box);
    lv_obj_set_style_bg_color(s_auto_box, lv_palette_main(LV_PALETTE_PURPLE), LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_style_border_color(s_auto_box, BTP_COL_SUB, LV_PART_INDICATOR);
    lv_obj_t *auto_text = lv_label_create(s_auto_row);
    lv_label_set_text(auto_text, locstr("Automatically pair DualSense/DualSense Edge Controllers"));
    lv_label_set_long_mode(auto_text, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(auto_text, 1);
    lv_obj_set_flex_grow(auto_text, 1);
    lv_obj_set_style_text_color(auto_text, BTP_COL_TXT, 0);
    lv_obj_add_event_cb(s_auto_row, btp_auto_cb, LV_EVENT_CLICKED, NULL);
    btp_keys(s_auto_row);

    /* ⛔⛔ A MODAL GROUP, pushed, as the Auto Bridge window's is: the stack is
     * what decides where the keys go, over the dashboard and over the overlay. */
    app_input_push_modal_group(&global->ui.input, s_group);
    bt_pairing_set_listener(btp_changed);
    btp_refresh();
    /* ⛔ OPENS ON SEARCH, never on a Forget (2026-10-29, the C1): over the
     * overlay it opened on the second row's Forget, one Enter from forgetting a
     * controller. Forget all is one step away. */
    lv_group_focus_obj(s_search);
    bt_pairing_refresh();
}

/* ---------------------------------------------------------------------------
 * The dashboard's button.
 * ------------------------------------------------------------------------- */

static void btp_topbar_clicked(lv_event_t *e) {
    LV_UNUSED(e);
    bt_panel_open(false);
}

void bt_panel_topbar_button(launcher_fragment_t *launcher, lv_obj_t *topbar) {
    /* Styled as the icons after it (launcher.view.c, create_topbar_icon_btn). A
     * child of the top bar joins its focus group, and its keys bubble to the
     * bar. */
    lv_obj_t *btn = lv_btn_create(topbar);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_set_size(btn, LV_DPX(TOPBAR_BTN_SIZE), LV_DPX(TOPBAR_BTN_SIZE));
    lv_obj_add_style(btn, &launcher->topbar_btn_style, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_70, 0);
    lv_obj_t *lbl = lv_label_create(btn);
    lv_obj_set_style_text_font(lbl, lv_theme_moonlight_get_iconfont_small(topbar), 0);
    lv_obj_set_style_text_color(lbl, ml_color_hex(ML_COLOR_TEXT), 0);
    lv_label_set_text_static(lbl, MAT_SYMBOL_SPORTS_ESPORTS);
    lv_obj_center(lbl);
    lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(btn, btp_topbar_clicked, LV_EVENT_CLICKED, NULL);
}

#else

void bt_panel_topbar_button(launcher_fragment_t *launcher, lv_obj_t *topbar) {
    (void) launcher;
    (void) topbar;
}

void bt_panel_open(bool over_stream) {
    (void) over_stream;
}

void bt_panel_close(void) {
}

bool bt_panel_shown(void) {
    return false;
}

#endif
