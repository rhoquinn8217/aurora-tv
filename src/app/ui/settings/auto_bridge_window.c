#include "auto_bridge_window.h"

#if defined(TARGET_WEBOS)

#include <stdio.h>
#include <string.h>

#include "app.h"
#include "ui/ui_input.h"
#include "util/i18n.h"
#include "ctm_bridge_glue.h"
#include "input/auto_bridge.h"
#include "input/device_groups.h"
#include "lvgl/font/material_icons_regular_symbols.h"
#include "lvgl/theme/lv_theme_moonlight.h"
#include "lvgl/util/lv_app_utils.h"

/* ⓘ The USB Bridge panel's palette, so the two read as one family. Copied
 * rather than shared: that panel is streaming-only and drags a
 * streaming_controller_t behind it, which this window has no business owning. */
#define ABW_COL_CARD    lv_color_hex(0x12181d)
#define ABW_COL_ROW     lv_color_hex(0x1a232b)
#define ABW_COL_FOCUS   lv_color_hex(0x2563eb)
#define ABW_COL_BORDER  lv_color_hex(0x2a3540)
#define ABW_COL_TXT     lv_color_hex(0xf5f8fa)
#define ABW_COL_SUB     lv_color_hex(0x95a3b0)
#define ABW_COL_PART    lv_color_hex(0xc8d2da)
#define ABW_COL_MARK    lv_palette_main(LV_PALETTE_PURPLE)

#define ABW_MAX DEVICE_GROUPS_MAX
/* ⓘ The row index of "Bridge all devices when the stream starts": one past the
 * device cards, so it shares their click handler. */
#define ABW_ALL ABW_MAX

static lv_obj_t   *s_win;
static lv_group_t *s_group;
static lv_obj_t   *s_box[ABW_MAX + 1];
/* ⭐ The parts listed as the window opened, and the devices they make up. A
 * card is a device, and a mark covers every part of it. */
static ctm_bridge_dev_t s_parts[DEVICE_PARTS_MAX];
static device_group_t   s_groups[ABW_MAX];
static int         s_count;
/* Every device card, markable or not: "Bridge all devices when the stream
 * starts" greys them all, since it bridges them all. */
static lv_obj_t   *s_dev_row[ABW_MAX];
static int         s_dev_count;
static lv_obj_t   *s_foot_btn[2];
static int         s_foot_count;

static void abw_close(void);
static void abw_close_cb(lv_event_t *e);
static void abw_key_cb(lv_event_t *e);
static void abw_apply_all(void);

/* ⭐ EVERY CARD THE MARKS COVER FOLLOWS, not only the one pressed: two devices
 * with one name and no serial share a mark, and a card left ticked would say it
 * still bridges when it no longer does. */
static void abw_refresh_boxes(void) {
    for (int i = 0; i < s_count; ++i) {
        if (s_box[i] == NULL) continue;
        if (auto_bridge_marked(app_configuration->bridge_auto_macs, s_parts, &s_groups[i])) {
            lv_obj_add_state(s_box[i], LV_STATE_CHECKED);
        } else {
            lv_obj_clear_state(s_box[i], LV_STATE_CHECKED);
        }
    }
}

static void abw_mark(int idx, bool on) {
    /* ⛔ Overridden while "Bridge all devices when the stream starts" is on: the
     * cards are greyed, and a press on one must not quietly change the marks
     * beneath. */
    if (app_configuration->bridge_auto_all) {
        return;
    }
    if (idx < 0 || idx >= s_count || s_box[idx] == NULL) {
        return;
    }
    char out[AUTO_BRIDGE_LIST_MAX];
    auto_bridge_mark_set(app_configuration->bridge_auto_macs, s_parts, &s_groups[idx], on, out, sizeof out);
    settings_set_auto_macs(app_configuration, out);
    abw_refresh_boxes();
}

/* ⭐ While "Bridge all devices when the stream starts" is on, the list above it
 * is overridden: its cards and the two buttons are greyed and do nothing, and
 * the marks underneath are kept, so turning it off brings every tick back
 * unchanged (rhoquinn8217, 2026-09-14). */
static void abw_apply_all(void) {
    const bool all = app_configuration->bridge_auto_all;
    if (s_box[ABW_ALL] != NULL) {
        if (all) {
            lv_obj_add_state(s_box[ABW_ALL], LV_STATE_CHECKED);
        } else {
            lv_obj_clear_state(s_box[ABW_ALL], LV_STATE_CHECKED);
        }
    }
    for (int i = 0; i < s_dev_count; ++i) {
        if (s_dev_row[i] == NULL) continue;
        if (all) {
            lv_obj_add_state(s_dev_row[i], LV_STATE_DISABLED);
        } else {
            lv_obj_clear_state(s_dev_row[i], LV_STATE_DISABLED);
        }
        lv_obj_set_style_opa(s_dev_row[i], all ? LV_OPA_40 : LV_OPA_COVER, 0);
    }
    for (int i = 0; i < s_foot_count; ++i) {
        if (all) {
            lv_obj_add_state(s_foot_btn[i], LV_STATE_DISABLED);
        } else {
            lv_obj_clear_state(s_foot_btn[i], LV_STATE_DISABLED);
        }
        lv_obj_set_style_opa(s_foot_btn[i], all ? LV_OPA_40 : LV_OPA_COVER, 0);
    }
}

static void abw_notice_cb(lv_event_t *e) {
    lv_msgbox_close_async(lv_event_get_current_target(e));
}

/* ⭐ SAID AS IT IS TICKED (rhoquinn8217, 2026-09-14). Every device reaching the
 * listener at once opens its config window as they arrive, and a stream that
 * starts before the last one lands looks like a device was missed. ⓘ The
 * theme gives a message box its own focus group and hands the keys back to
 * this window when it closes. */
static void abw_all_notice(void) {
    static const char *btn_texts[] = {translatable("OK"), ""};
    lv_obj_t *box = lv_msgbox_create_i18n(NULL, NULL, locstr(
            "When the stream starts, the DS5-USBIP config window will open when devices "
            "begin connecting. Please allow enough time for all devices to bridge."),
            btn_texts, false);
    lv_obj_add_event_cb(box, abw_notice_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_center(box);
}

static void abw_row_click_cb(lv_event_t *e) {
    const int idx = (int) (intptr_t) lv_event_get_user_data(e);
    if (idx == ABW_ALL) {
        app_configuration->bridge_auto_all = !app_configuration->bridge_auto_all;
        abw_apply_all();
        if (app_configuration->bridge_auto_all) {
            abw_all_notice();
        }
        return;
    }
    if (idx < 0 || idx >= s_count || s_box[idx] == NULL) return;
    abw_mark(idx, !lv_obj_has_state(s_box[idx], LV_STATE_CHECKED));
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
    if (s_foot_count < 2) {
        s_foot_btn[s_foot_count++] = b;   /* greyed while Bridge all is on */
    }
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

/* A card or the Bridge-all row: the frame, focus colours and keys they share. */
static lv_obj_t *abw_make_frame(lv_obj_t *parent) {
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    /* ⓘ The box sits level with the device's name, at the top of a tall card,
     * so it reads as ticking the device rather than one of its parts. */
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
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
    return row;
}

static lv_obj_t *abw_make_column(lv_obj_t *row) {
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
    return col;
}

/* ⭐ A sentence WRAPS, a name or a serial TRUNCATES. A reason cut off at one
 * line would say nothing. */
static lv_obj_t *abw_make_text(lv_obj_t *col, const char *text, lv_color_t color,
                               const lv_font_t *font, bool wrap) {
    lv_obj_t *l = lv_label_create(col);
    lv_label_set_text(l, text);
    lv_label_set_long_mode(l, wrap ? LV_LABEL_LONG_WRAP : LV_LABEL_LONG_DOT);
    lv_obj_set_width(l, LV_PCT(100));
    lv_obj_set_style_text_color(l, color, 0);
    lv_obj_set_style_text_font(l, font, 0);
    return l;
}

/* ⛔ NOT CHECKABLE, and CLICKED rather than VALUE_CHANGED. A checkbox left
 * checkable ticked ITSELF when focus moved onto it -- the bug the overlay panel
 * hit on 2026-09-08. The state is set by hand. */
static void abw_make_box(lv_obj_t *row, int idx) {
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
}

/* ⭐⭐ ONE CARD PER DEVICE, ITS PARTS INSIDE IT (rhoquinn8217, 2026-09-14).
 * - The name, and for a device of one part what that part is: "DualSense
 *   (CONTROLLER)".
 * - What it is remembered by: "MAC: ...", "serial: ..." or "(no serial)".
 * - For a device of several parts, each part as the kernel names it, with what
 *   it is. ⓘ Names repeat when a device reports them twice, and are left that
 *   way: the listener shows each part under that same name, and this list is
 *   what explains why one device arrives there as several.
 * ONE box, which selects every part. */
static lv_obj_t *abw_make_card(lv_obj_t *parent, int idx) {
    const device_group_t *g = &s_groups[idx];
    lv_obj_t *row = abw_make_frame(parent);
    lv_obj_t *col = abw_make_column(row);

    char header[200];
    if (g->part_count == 1) {
        char type[16];
        device_part_type(&s_parts[g->part[0]], type, sizeof type);
        snprintf(header, sizeof header, "%s (%s)", g->name, type);
    } else {
        snprintf(header, sizeof header, "%s", g->name);
    }
    abw_make_text(col, header, ABW_COL_TXT, lv_theme_get_font_normal(parent), false);
    abw_make_text(col, g->shown, ABW_COL_SUB, lv_theme_get_font_small(parent), false);
    if (!g->has_node) {
        abw_make_text(col, locstr("no device node - this device cannot be bridged"), ABW_COL_SUB,
                      lv_theme_get_font_small(parent), true);
    }

    if (g->part_count > 1) {
        lv_obj_t *parts = abw_make_column(col);
        lv_obj_set_width(parts, LV_PCT(100));
        lv_obj_set_flex_grow(parts, 0);
        lv_obj_set_style_pad_left(parts, LV_DPX(10), 0);
        lv_obj_set_style_pad_top(parts, LV_DPX(2), 0);
        lv_obj_set_style_pad_gap(parts, LV_DPX(1), 0);
        for (int p = 0; p < g->part_count; ++p) {
            const ctm_bridge_dev_t *d = &s_parts[g->part[p]];
            char type[16];
            device_part_type(d, type, sizeof type);
            char line[200];
            snprintf(line, sizeof line, "%s (%s)", d->name, type);
            abw_make_text(parts, line, ABW_COL_PART, lv_theme_get_font_small(parent), false);
        }
    }

    if (!g->has_node) {
        /* ⭐ Cannot be marked: shown, and told why. ⛔ Not a blank box -- that
         * reads as "not yet" when the truth is "cannot". */
        lv_obj_t *dash = lv_label_create(row);
        lv_label_set_text(dash, "--");
        lv_obj_set_style_text_color(dash, ABW_COL_SUB, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_CLICKABLE);
        return row;
    }
    abw_make_box(row, idx);
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
    s_dev_count = 0;
}

void auto_bridge_window_open(void) {
    if (s_win != NULL) {
        return;
    }
    s_count = 0;
    s_dev_count = 0;
    s_foot_count = 0;
    memset(s_box, 0, sizeof s_box);
    memset(s_dev_row, 0, sizeof s_dev_row);
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
     * the NEXT stream, so without it a ticked box looks like it did nothing.
     * ⓘ It says what happens to a device without a usable serial, rather than
     * leaving it to be discovered (rhoquinn8217, 2026-09-14). */
    abw_make_text(card, locstr(
            "Selected devices bridge automatically when the stream starts. If you select a "
            "device that doesn't have a serial or if it is all zeros, all devices that share "
            "that device's name will also be auto bridged."),
            ABW_COL_SUB, lv_theme_get_font_small(card), true);
    /* ⭐ BOLD, on a line of its own (rhoquinn8217, 2026-09-14): the one condition
     * that makes every selection above do nothing. */
    abw_make_text(card, locstr(
            "*DS5-USBIP must be running before the stream starts for devices to auto bridge"),
            ABW_COL_SUB, lv_theme_moonlight_get_font_small_bold(card), true);

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
    const int n = ctm_bridge_list_quiet(s_parts, DEVICE_PARTS_MAX);
    /* ⭐ BY NAME (rhoquinn8217, 2026-09-14). Sorting by serial was there to put
     * the parts of one device side by side; a card holds them now. */
    s_count = device_groups_build(s_parts, n, s_groups, ABW_MAX);

    for (int i = 0; i < s_count; ++i) {
        s_dev_row[s_dev_count++] = abw_make_card(list, i);
    }
    if (s_count == 0) {
        lv_obj_t *none = lv_label_create(list);
        lv_label_set_text(none, locstr("No devices are connected."));
        lv_obj_set_style_text_color(none, ABW_COL_SUB, 0);
    }
    abw_refresh_boxes();

    /* ⭐⭐ LAST IN THE LIST, AND IT OVERRIDES THE LIST (rhoquinn8217,
     * 2026-09-13): when a stream starts, bridge every device, with a serial or
     * without. While it is ticked the cards above are greyed and left as they
     * were. */
    {
        lv_obj_t *row = abw_make_frame(list);
        lv_obj_t *col = abw_make_column(row);
        abw_make_text(col, locstr("Bridge all devices when the stream starts"), ABW_COL_TXT,
                      lv_theme_get_font_normal(list), false);
        abw_make_text(col, locstr("Bridges every device when the stream starts. Overrides the "
                                  "selections above."),
                      ABW_COL_SUB, lv_theme_get_font_small(list), true);
        abw_make_box(row, ABW_ALL);
    }

    bool markable = false;
    for (int i = 0; i < s_count; ++i) {
        markable = markable || s_groups[i].has_node;
    }
    if (markable) {
        lv_obj_t *foot = lv_obj_create(card);
        lv_obj_remove_style_all(foot);
        lv_obj_set_size(foot, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(foot, LV_FLEX_FLOW_ROW);
        lv_obj_set_style_pad_gap(foot, LV_DPX(8), 0);
        lv_obj_clear_flag(foot, LV_OBJ_FLAG_SCROLLABLE);
        /* ⓘ Named for what they DO to the list rather than for the two modes:
         * every device, or none. "Manual bridge all" described the state left
         * behind rather than the action taken. */
        abw_make_all_btn(foot, locstr("Select all"), abw_all_auto_cb);
        abw_make_all_btn(foot, locstr("Clear all"), abw_all_manual_cb);
    }

    /* ⛔ THE BUTTONS AT THE FOOT WERE CUT OFF (rhoquinn8217, 2026-09-14, build
     * 327, seven devices on the U5s). The list stops at 60% of the window and
     * the window at 80% of the screen, and at the TV's scale the title, the
     * two lines of text and the buttons need more than the 40% left: the window
     * hit its limit and clipped whatever came last.
     * ➡️ So when the contents run past the window's edge, the window takes its
     * full 80% and the list takes only what everything else leaves, scrolling
     * inside it. ⛔ Only then: a list that fits keeps the window as short as its
     * contents, and a grow inside a window sized to its contents gets no room at
     * all (the USB Bridge panel's empty-list bug). */
    lv_obj_update_layout(card);
    lv_obj_t *last = lv_obj_get_child(card, -1);
    if (last != NULL) {
        lv_area_t card_area;
        lv_area_t last_area;
        lv_obj_get_coords(card, &card_area);
        lv_obj_get_coords(last, &last_area);
        const lv_coord_t inner_bottom = card_area.y2 - lv_obj_get_style_pad_bottom(card, 0) -
                                        lv_obj_get_style_border_width(card, 0);
        if (last_area.y2 > inner_bottom) {
            lv_obj_set_height(card, LV_PCT(80));
            lv_obj_set_style_max_height(list, LV_COORD_MAX, 0);
            lv_obj_set_flex_grow(list, 1);
        }
    }

    /* ⛔⛔ PUSH A MODAL GROUP. app_input_set_group() sets the BASE group, and
     * app_input_get_group() returns the modal stack's tail in preference to it
     * -- so on 2026-09-08 this window drew, took pointer clicks, and never saw
     * a single key: the settings screen's own group was still on top and every
     * press went to the pane behind. ➡️ A window over another screen is a
     * MODAL, and the stack is what the launcher's popup and the settings detail
     * pane already use. */
    app_input_push_modal_group(&global->ui.input, s_group);
    abw_apply_all();
    /* ⓘ There is always a row to land on: Bridge all is one, device or not. */
    lv_group_focus_next(s_group);
}

#endif /* TARGET_WEBOS */
