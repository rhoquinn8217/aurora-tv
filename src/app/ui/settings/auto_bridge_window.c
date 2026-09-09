#include "auto_bridge_window.h"

#if defined(TARGET_WEBOS)

#include <stdio.h>
#include <string.h>

#include "app.h"
#include "ui/ui_input.h"
#include "util/i18n.h"
#include "ctm_bridge_glue.h"
#include "input/ctm_bridge_gesture.h"
#include "input/auto_bridge.h"
#include "lvgl/font/material_icons_regular_symbols.h"
#include "lvgl/theme/lv_theme_moonlight.h"

/* ⓘ The USB Bridge panel's palette, so the two read as one family. Copied
 * rather than shared: that panel is streaming-only and drags a
 * streaming_controller_t behind it, which this window has no business owning. */
#define ABW_COL_CARD    lv_color_hex(0x12181d)
#define ABW_COL_ROW     lv_color_hex(0x1a232b)
#define ABW_COL_FOCUS   lv_color_hex(0x2563eb)
#define ABW_COL_BORDER  lv_color_hex(0x2a3540)
#define ABW_COL_TXT     lv_color_hex(0xf5f8fa)
#define ABW_COL_SUB     lv_color_hex(0x95a3b0)
#define ABW_COL_MARK    lv_palette_main(LV_PALETTE_PURPLE)

#define ABW_MAX 16

static lv_obj_t   *s_win;
static lv_group_t *s_group;
static lv_group_t *s_prev_group;
static lv_obj_t   *s_box[ABW_MAX];
static char        s_mac[ABW_MAX][64];
static int         s_count;

static void abw_close(void);

/* ⭐ A short name: the reported one is unusable in a row -- "Sony Interactive
 * Entertainment DualSense Edge Wireless Controller" is most of a screen. */
static const char *abw_short_name(const ctm_bridge_dev_t *d) {
    if (strcmp(d->vid, "054c") == 0 && strcmp(d->pid, "0ce6") == 0) return "DualSense";
    if (strcmp(d->vid, "054c") == 0 && strcmp(d->pid, "0df2") == 0) return "DualSense Edge";
    return d->name;
}

static void abw_mark(int row, bool on) {
    char out[512];
    auto_bridge_list_set(app_configuration->bridge_auto_macs, s_mac[row], on, out, sizeof out);
    settings_set_auto_macs(app_configuration, out);
    if (on) {
        lv_obj_add_state(s_box[row], LV_STATE_CHECKED);
    } else {
        lv_obj_clear_state(s_box[row], LV_STATE_CHECKED);
    }
}

static void abw_row_click_cb(lv_event_t *e) {
    const int row = (int) (intptr_t) lv_event_get_user_data(e);
    if (row < 0 || row >= s_count) return;
    abw_mark(row, !lv_obj_has_state(s_box[row], LV_STATE_CHECKED));
}

/* ⭐ Mark everything; when everything already is, clear it. One control for
 * both directions, rather than a second button that only says "none". */
static void abw_all_click_cb(lv_event_t *e) {
    LV_UNUSED(e);
    bool all = true;
    for (int i = 0; i < s_count; ++i) {
        if (!auto_bridge_list_has(app_configuration->bridge_auto_macs, s_mac[i])) {
            all = false;
            break;
        }
    }
    for (int i = 0; i < s_count; ++i) {
        abw_mark(i, !all);
    }
}

static void abw_close_cb(lv_event_t *e) {
    LV_UNUSED(e);
    abw_close();
}

static void abw_key_cb(lv_event_t *e) {
    /* ⓘ Up and Down only. Left and Right mean bridge and release in the panel
     * this is cut down from, and there is nothing here for them to do. */
    switch (lv_event_get_key(e)) {
        case LV_KEY_UP:
            lv_group_focus_prev(s_group);
            lv_obj_scroll_to_view(lv_group_get_focused(s_group), LV_ANIM_ON);
            break;
        case LV_KEY_DOWN:
            lv_group_focus_next(s_group);
            lv_obj_scroll_to_view(lv_group_get_focused(s_group), LV_ANIM_ON);
            break;
        default:
            break;
    }
}

static lv_obj_t *abw_make_row(lv_obj_t *parent, const char *name, const char *mac, int idx) {
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(row, LV_DPX(8), 0);
    lv_obj_set_style_radius(row, LV_DPX(6), 0);
    lv_obj_set_style_bg_color(row, ABW_COL_ROW, 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(row, ABW_COL_FOCUS, LV_STATE_FOCUS_KEY);
    lv_obj_set_style_border_width(row, LV_DPX(1), 0);
    lv_obj_set_style_border_color(row, ABW_COL_BORDER, 0);
    lv_obj_set_style_border_color(row, ABW_COL_FOCUS, LV_STATE_FOCUS_KEY);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);

    /* Device over its address, the same shape the panel's rows use. */
    lv_obj_t *col = lv_obj_create(row);
    lv_obj_remove_style_all(col);
    lv_obj_set_width(col, 1);
    lv_obj_set_flex_grow(col, 1);
    lv_obj_set_height(col, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_all(col, 0, 0);
    lv_obj_set_style_pad_gap(col, LV_DPX(2), 0);
    lv_obj_clear_flag(col, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *nm = lv_label_create(col);
    lv_label_set_text(nm, name);
    lv_label_set_long_mode(nm, LV_LABEL_LONG_DOT);
    lv_obj_set_width(nm, LV_PCT(100));
    lv_obj_set_style_text_color(nm, ABW_COL_TXT, 0);
    lv_obj_set_style_text_font(nm, lv_theme_get_font_normal(parent), 0);

    lv_obj_t *ad = lv_label_create(col);
    lv_label_set_text(ad, mac);
    lv_label_set_long_mode(ad, LV_LABEL_LONG_DOT);
    lv_obj_set_width(ad, LV_PCT(100));
    lv_obj_set_style_text_color(ad, ABW_COL_SUB, 0);
    lv_obj_set_style_text_font(ad, lv_theme_get_font_small(parent), 0);

    if (idx < 0) {
        /* ⭐ No address: shown, and told why. ⛔ Not a blank box -- that reads
         * as "not yet" when the truth is "cannot". Anything SDL does not open
         * as a controller has no address to key a mark on. */
        lv_obj_t *dash = lv_label_create(row);
        lv_label_set_text(dash, "--");
        lv_obj_set_style_text_color(dash, ABW_COL_SUB, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_CLICKABLE);
        return row;
    }

    /* ⛔ NOT CHECKABLE, and CLICKED rather than VALUE_CHANGED. A checkbox left
     * checkable ticked ITSELF when focus moved onto it -- the bug the overlay
     * panel hit on 2026-09-08. The state is set by hand in abw_mark(). */
    lv_obj_t *box = lv_checkbox_create(row);
    lv_checkbox_set_text(box, "");
    lv_obj_clear_flag(box, LV_OBJ_FLAG_CHECKABLE);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(box, ABW_COL_MARK, LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_style_border_color(box, ABW_COL_SUB, LV_PART_INDICATOR);
    s_box[idx] = box;

    lv_obj_add_event_cb(row, abw_row_click_cb, LV_EVENT_CLICKED, (void *) (intptr_t) idx);
    lv_obj_add_event_cb(row, abw_key_cb, LV_EVENT_KEY, NULL);
    lv_obj_add_event_cb(row, abw_close_cb, LV_EVENT_CANCEL, NULL);
    lv_group_add_obj(s_group, row);
    return row;
}

static void abw_close(void) {
    if (s_win == NULL) {
        return;
    }
    app_input_set_group(&global->ui.input, s_prev_group);
    lv_obj_del(s_win);
    s_win = NULL;
    if (s_group) {
        lv_group_del(s_group);
        s_group = NULL;
    }
    s_prev_group = NULL;
    s_count = 0;
}

void auto_bridge_window_open(void) {
    if (s_win != NULL) {
        return;
    }
    s_count = 0;
    s_group = lv_group_create();
    s_prev_group = app_input_get_group(&global->ui.input);

    /* ⓘ On the top layer, so it sits over the settings screen without being
     * part of it and cannot be disturbed by the pane rebuilding beneath. */
    s_win = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_win);
    lv_obj_set_size(s_win, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_win, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_win, LV_OPA_50, 0);
    lv_obj_clear_flag(s_win, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_win, LV_OBJ_FLAG_CLICKABLE);   /* swallow clicks behind it */

    lv_obj_t *card = lv_obj_create(s_win);
    lv_obj_remove_style_all(card);
    lv_obj_set_width(card, LV_PCT(56));
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_set_style_max_height(card, LV_PCT(80), 0);
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, ABW_COL_CARD, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, LV_DPX(10), 0);
    lv_obj_set_style_border_width(card, LV_DPX(1), 0);
    lv_obj_set_style_border_color(card, ABW_COL_BORDER, 0);
    lv_obj_set_style_pad_all(card, LV_DPX(16), 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_gap(card, LV_DPX(8), 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    /* Title centred, close corner pinned out of the flow -- as the panel does. */
    lv_obj_t *titlerow = lv_obj_create(card);
    lv_obj_remove_style_all(titlerow);
    lv_obj_set_size(titlerow, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(titlerow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(titlerow, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(titlerow, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(titlerow);
    lv_label_set_text(title, locstr("Auto Bridge"));
    lv_obj_set_style_text_color(title, ABW_COL_TXT, 0);
    lv_obj_set_style_text_font(title, lv_theme_get_font_large(titlerow), 0);

    lv_obj_t *x = lv_btn_create(titlerow);
    lv_obj_remove_style_all(x);
    lv_obj_set_size(x, LV_DPX(30), LV_DPX(30));
    lv_obj_set_style_radius(x, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(x, ABW_COL_ROW, 0);
    lv_obj_set_style_bg_opa(x, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(x, ABW_COL_FOCUS, LV_STATE_FOCUS_KEY);
    lv_obj_add_flag(x, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(x, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_align(x, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_t *xl = lv_label_create(x);
    lv_label_set_text_static(xl, MAT_SYMBOL_CLOSE);
    lv_obj_set_style_text_font(xl, lv_theme_moonlight_get_iconfont_small(titlerow), 0);
    lv_obj_set_style_text_color(xl, ABW_COL_TXT, 0);
    lv_obj_center(xl);
    lv_obj_clear_flag(xl, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(x, abw_close_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(x, abw_key_cb, LV_EVENT_KEY, NULL);
    lv_obj_add_event_cb(x, abw_close_cb, LV_EVENT_CANCEL, NULL);
    lv_group_add_obj(s_group, x);

    /* ⭐ The sentence that makes a tick mean something. The effect arrives at
     * the NEXT stream, so without it a ticked box looks like it did nothing. */
    lv_obj_t *sub = lv_label_create(card);
    lv_label_set_text(sub, locstr(
            "These devices bridge themselves to your gaming PC when a stream starts."));
    lv_label_set_long_mode(sub, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(sub, LV_PCT(100));
    lv_obj_set_style_text_color(sub, ABW_COL_SUB, 0);
    lv_obj_set_style_text_font(sub, lv_theme_get_font_small(card), 0);

    lv_obj_t *list = lv_obj_create(card);
    lv_obj_remove_style_all(list);
    lv_obj_set_width(list, LV_PCT(100));
    lv_obj_set_height(list, LV_SIZE_CONTENT);
    lv_obj_set_style_max_height(list, LV_PCT(60), 0);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_gap(list, LV_DPX(6), 0);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_AUTO);

    /* ⛔ The QUIET list: the ordinary one brings the bridge core up, which
     * broadcasts for an agent that cannot exist before a host is chosen.
     * Drawing a list is not a reason to wake the bridge. */
    ctm_bridge_dev_t devs[ABW_MAX];
    const int n = ctm_bridge_list_quiet(devs, ABW_MAX);
    int shown = 0;
    for (int i = 0; i < n && s_count < ABW_MAX; ++i) {
        char mac[64];
        const bool has = devs[i].node[0] != '\0' &&
                         ctm_bridge_gesture_mac_for_node(devs[i].node, mac, sizeof mac);
        if (!has) {
            abw_make_row(list, abw_short_name(&devs[i]), locstr("no address"), -1);
            shown++;
            continue;
        }
        const int idx = s_count;
        snprintf(s_mac[idx], sizeof s_mac[idx], "%s", mac);
        s_count++;
        lv_obj_t *row = abw_make_row(list, abw_short_name(&devs[i]), mac, idx);
        LV_UNUSED(row);
        if (auto_bridge_list_has(app_configuration->bridge_auto_macs, mac)) {
            lv_obj_add_state(s_box[idx], LV_STATE_CHECKED);
        }
        shown++;
    }
    if (shown == 0) {
        lv_obj_t *none = lv_label_create(list);
        lv_label_set_text(none, locstr("No devices are connected."));
        lv_obj_set_style_text_color(none, ABW_COL_SUB, 0);
    }

    if (s_count > 1) {
        lv_obj_t *all = lv_btn_create(card);
        lv_obj_remove_style_all(all);
        lv_obj_set_size(all, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_style_pad_all(all, LV_DPX(10), 0);
        lv_obj_set_style_radius(all, LV_DPX(6), 0);
        lv_obj_set_style_bg_color(all, ABW_COL_ROW, 0);
        lv_obj_set_style_bg_opa(all, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(all, ABW_COL_FOCUS, LV_STATE_FOCUS_KEY);
        lv_obj_set_style_border_width(all, LV_DPX(1), 0);
        lv_obj_set_style_border_color(all, ABW_COL_BORDER, 0);
        lv_obj_t *al = lv_label_create(all);
        lv_label_set_text(al, locstr("Auto bridge all"));
        lv_obj_set_style_text_color(al, ABW_COL_TXT, 0);
        lv_obj_center(al);
        lv_obj_add_event_cb(all, abw_all_click_cb, LV_EVENT_CLICKED, NULL);
        lv_obj_add_event_cb(all, abw_key_cb, LV_EVENT_KEY, NULL);
        lv_obj_add_event_cb(all, abw_close_cb, LV_EVENT_CANCEL, NULL);
        lv_group_add_obj(s_group, all);
    }

    app_input_set_group(&global->ui.input, s_group);
    if (s_count > 0) {
        lv_group_focus_next(s_group);
    }
}

#endif /* TARGET_WEBOS */
