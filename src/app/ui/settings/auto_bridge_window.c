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
static lv_obj_t   *s_box[ABW_MAX];
static char        s_mac[ABW_MAX][64];
static int         s_count;

static void abw_close(void);
static void abw_close_cb(lv_event_t *e);
static void abw_key_cb(lv_event_t *e);

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

/* ⭐ TWO BUTTONS, EACH DOING ONE THING (rhoquinn8217, 2026-09-08). ⛔ One
 * button that marked everything and then un-marked everything on the next press
 * was worse: from across a room you cannot see which way it will go, so the
 * safe move was always to press it and watch. Naming both directions means
 * neither has to be discovered. */
static void abw_all_auto_cb(lv_event_t *e) {
    LV_UNUSED(e);
    for (int i = 0; i < s_count; ++i) {
        abw_mark(i, true);
    }
}

static void abw_all_manual_cb(lv_event_t *e) {
    LV_UNUSED(e);
    for (int i = 0; i < s_count; ++i) {
        abw_mark(i, false);
    }
}

/* One of the pair at the foot of the window. */
static void abw_make_all_btn(lv_obj_t *parent, const char *text, lv_event_cb_t cb) {
    lv_obj_t *b = lv_btn_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_set_width(b, 1);
    lv_obj_set_flex_grow(b, 1);
    lv_obj_set_height(b, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(b, LV_DPX(10), 0);
    lv_obj_set_style_radius(b, LV_DPX(6), 0);
    lv_obj_set_style_bg_color(b, ABW_COL_ROW, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(b, ABW_COL_FOCUS, LV_STATE_FOCUS_KEY);
    lv_obj_set_style_border_width(b, LV_DPX(1), 0);
    lv_obj_set_style_border_color(b, ABW_COL_BORDER, 0);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_color(l, ABW_COL_TXT, 0);
    lv_obj_center(l);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(b, abw_key_cb, LV_EVENT_KEY, NULL);
    lv_obj_add_event_cb(b, abw_close_cb, LV_EVENT_CANCEL, NULL);
    lv_group_add_obj(s_group, b);
}

static void abw_close_cb(lv_event_t *e) {
    LV_UNUSED(e);
    abw_close();
}

static void abw_key_cb(lv_event_t *e) {
    /* ⭐ LEFT AND RIGHT MOVE TOO, and they have to (rhoquinn8217, 2026-09-08).
     * The two buttons at the foot sit SIDE BY SIDE, so reaching for right to
     * get from one to the other is the obvious thing to do -- and it did
     * nothing, because they are merely consecutive in the group and only up and
     * down walked it. ➡️ Where two things are drawn beside each other, the key
     * that points that way should go there. ⓘ The USB Bridge panel reached the
     * same conclusion for Bridge All and Release All on 2026-08-20.
     *
     * ⛔ Left and right cannot mean bridge and release here, as they do in the
     * panel this is cut down from: there is no host to bridge to outside a
     * stream, so they are free to navigate. */
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
    /* ⭐ The reason WRAPS, an address TRUNCATES. A device that cannot be marked
     * gets a sentence rather than a shrug, and a sentence cut off at one line
     * would be the shrug again. */
    lv_label_set_long_mode(ad, idx < 0 ? LV_LABEL_LONG_WRAP : LV_LABEL_LONG_DOT);
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
    /* ⛔⛔ REMOVE THE MODAL GROUP, do not "restore" the old one. See the push
     * in open() -- the stack owns what is focused, and popping is what hands
     * the settings screen its keys back. */
    app_input_remove_modal_group(&global->ui.input, s_group);
    lv_obj_del(s_win);
    s_win = NULL;
    if (s_group) {
        lv_group_del(s_group);
        s_group = NULL;
    }
    s_count = 0;
}

void auto_bridge_window_open(void) {
    if (s_win != NULL) {
        return;
    }
    s_count = 0;
    s_group = lv_group_create();

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
            "Selected devices that will automatically bridge when the stream starts. "
            "Requires a device with a mac address and the CTM-USBIP running before "
            "the stream starts."));
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
            /* ⭐ Say WHY, not just that (rhoquinn8217, 2026-09-08). "no address"
             * states a fact and leaves the reader to guess whether it is a
             * fault, a wait, or a rule. */
            abw_make_row(list, abw_short_name(&devs[i]),
                         locstr("no mac address - auto bridge can only target "
                                "devices with mac addresses"), -1);
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

    if (s_count > 0) {
        lv_obj_t *foot = lv_obj_create(card);
        lv_obj_remove_style_all(foot);
        lv_obj_set_size(foot, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(foot, LV_FLEX_FLOW_ROW);
        lv_obj_set_style_pad_gap(foot, LV_DPX(8), 0);
        lv_obj_clear_flag(foot, LV_OBJ_FLAG_SCROLLABLE);
        /* ⓘ Named for what they DO to the list rather than for the two modes:
         * every address, or none. "Manual bridge all" described the state left
         * behind rather than the action taken. */
        abw_make_all_btn(foot, locstr("All MAC addresses"), abw_all_auto_cb);
        abw_make_all_btn(foot, locstr("Clear all"), abw_all_manual_cb);
    }

    /* ⛔⛔ PUSH A MODAL GROUP. app_input_set_group() sets the BASE group, and
     * app_input_get_group() returns the modal stack's tail in preference to it
     * -- so on 2026-09-08 this window drew, took pointer clicks, and never saw
     * a single key: the settings screen's own group was still on top and every
     * press went to the pane behind. ➡️ A window over another screen is a
     * MODAL, and the stack is what the launcher's popup and the settings detail
     * pane already use. */
    app_input_push_modal_group(&global->ui.input, s_group);
    if (s_count > 0) {
        lv_group_focus_next(s_group);
    }
}

#endif /* TARGET_WEBOS */
