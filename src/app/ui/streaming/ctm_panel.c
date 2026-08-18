/* The CTM bridge panel: a device list, per-device settings, and bridge actions.
 *
 * ⭐ LIFTED OUT OF streaming.controller.c ON 2026-08-17, unchanged. That file is
 * GuiDev1994's, and this block was ~780 of the ~790 lines this fork had added to
 * it -- every one of them a place his next release could conflict. Moving it
 * here leaves a handful of lines at the seam and costs him nothing.
 *
 * ⚠️ NOTHING WAS REWRITTEN IN THE MOVE, deliberately. The lift is verified by
 * NOTHING CHANGING; the panel is stripped down afterwards, in this file, where a
 * mistake does not touch his code and the diff against him does not move.
 *
 * ⛔ WHAT STAYS BEHIND, and why it cannot come here: the teardown. It runs
 * inside his on_delete_obj, so this file exposes ctm_panel_on_owner_deleted()
 * and he calls it.
 *
 * ⓘ The one real coupling is streaming_controller_t -- the panel reads the
 * owner's input group to hand focus back and forth.
 */

/* ⚠️ app.h first, and it is NOT optional. The panel reads the owner's
 * global->ui.input to move focus between its groups, so it needs the COMPLETE
 * app_t -- a forward declaration compiles the header and fails on every use.
 * ui_input.h declares app_input_set_group() itself. */
#include "app.h"
#include "ui/ui_input.h"
#include "streaming.controller.h"
#include "ctm_bridge_glue.h"
#include "ctm_panel.h"

#include <string.h>

/* Which setting a detail row edits. ⚠️ This lived one line above the block that
 * was lifted and was missed on the first pass -- the whole build failed on it. */


/* Forward declarations, moved with the block rather than left behind: several of
 * these functions call each other in both directions. */
static void ctm_close_panel(void);
static void ctm_request_close(void);
static void ctm_teardown_async(void *p);
static void ctm_panel_refresh(void);
static void ctm_request_refresh(void);
static void open_ctm_panel(lv_event_t *event);

#define CTM_COL_CARD     lv_color_hex(0x12181d)
#define CTM_COL_SIDEBAR  lv_color_hex(0x1a232b)
#define CTM_COL_ROW      lv_color_hex(0x232f39)
#define CTM_COL_FOCUS    lv_color_hex(0x2563eb)
#define CTM_COL_BORDER   lv_color_hex(0x2a3540)
#define CTM_COL_TXT      lv_color_hex(0xf5f8fa)
#define CTM_COL_SUB      lv_color_hex(0x95a3b0)
#define CTM_COL_OK       lv_color_hex(0x35c46a)


static lv_obj_t   *s_ctm_panel      = NULL;   /* full-screen backdrop */
static lv_obj_t   *s_ctm_sidebar    = NULL;   /* the device list */
static lv_obj_t   *s_ctm_status_lbl = NULL;
static lv_group_t *s_ctm_nav_group    = NULL;
static streaming_controller_t *s_ctm_owner = NULL;

static ctm_bridge_dev_t s_ctm_devs[16];
static lv_obj_t        *s_ctm_dev_rows[16];
static int  s_ctm_ndev = 0;
static int  s_ctm_sel  = 0;

/* ⭐ THE DETAIL PANE IS GONE, AND THE CRASH IT CAUSED WITH IT.
 *
 * A guard used to live here: leaving the pane focused a sidebar row, focusing
 * rebuilt the pane, and the rebuild freed the object LVGL was still dispatching
 * on. Fixed on 2026-08-17 by not rebuilding from inside a leave.
 *
 * ⭐⭐ Removing the pane removes the whole shape of that fault. Recorded because
 * the same trap waits for anything that rebuilds a container from inside an
 * event raised by one of its children. */


/* Deferred-teardown holders: the panel is hidden synchronously on close (so the
 * remote Back produces a same-frame UI change and webOS doesn't background the
 * app), then these are freed on the next loop (can't delete the focused row from
 * inside its own Back event). */
static lv_obj_t   *s_ctm_dead_panel  = NULL;
static lv_group_t *s_ctm_dead_nav    = NULL;


/* Bridge or release the device on this row.
 *
 * ⭐ Activating a row IS the action now. There is no detail pane to enter, so a
 * press does the one thing a press could mean. */
static void ctm_toggle_device(int row) {
    if (row < 0 || row >= s_ctm_ndev) {
        return;
    }
    if (s_ctm_devs[row].plugged) {
        ctm_bridge_unplug_index(row);
    } else {
        ctm_bridge_plug_index(row);
    }
    ctm_request_refresh();
}


/* A native-looking button: reuses the streaming action-bar style (rounded,
 * shadow, blue focus outline) so panel buttons match the rest of the app
 * instead of looking improvised. */
static lv_obj_t *ctm_nice_btn(lv_obj_t *parent, const char *text, lv_color_t bg) {
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_EVENT_BUBBLE);   /* Back bubbles to the container */
    lv_obj_add_style(btn, &s_ctm_owner->overlay_button_style, 0);
    lv_obj_add_style(btn, &s_ctm_owner->overlay_button_style_focused, LV_STATE_FOCUS_KEY);
    lv_obj_set_style_bg_color(btn, bg, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_t *l = lv_label_create(btn);
    lv_obj_add_style(l, &s_ctm_owner->overlay_button_label_style, 0);
    lv_label_set_text(l, text);
    lv_obj_center(l);
    return btn;
}


/* A row was focused: move the selection highlight. Nothing else.
 *
 * ⭐ This used to rebuild a detail pane on every focus change, which is what
 * made the crash possible. */
static void ctm_dev_focus_cb(lv_event_t *e) {
    int row = (int) (intptr_t) lv_event_get_user_data(e);
    if (row < 0 || row >= s_ctm_ndev) {
        return;
    }
    s_ctm_sel = row;
    for (int i = 0; i < s_ctm_ndev; ++i) {
        if (!s_ctm_dev_rows[i]) continue;
        if (i == row) lv_obj_add_state(s_ctm_dev_rows[i], LV_STATE_CHECKED);
        else          lv_obj_clear_state(s_ctm_dev_rows[i], LV_STATE_CHECKED);
    }
}

/* A row was activated -> bridge or release it. */
static void ctm_dev_click_cb(lv_event_t *e) {
    int row = (int) (intptr_t) lv_event_get_user_data(e);
    s_ctm_sel = row;
    ctm_toggle_device(row);
}

static void ctm_nav_key_cb(lv_event_t *e) {
    int row = (int) (intptr_t) lv_event_get_user_data(e);
    switch (lv_event_get_key(e)) {
        case LV_KEY_UP:
            lv_group_focus_prev(s_ctm_nav_group);
            lv_obj_scroll_to_view(lv_group_get_focused(s_ctm_nav_group), LV_ANIM_ON);
            break;
        case LV_KEY_DOWN:
            lv_group_focus_next(s_ctm_nav_group);
            lv_obj_scroll_to_view(lv_group_get_focused(s_ctm_nav_group), LV_ANIM_ON);
            break;
        case LV_KEY_RIGHT:
            /* ⭐ Right used to enter the detail pane. With the pane gone it does
             * the same thing as Select, so a user who reaches for either gets
             * the action rather than nothing. */
            ctm_toggle_device(row);
            break;
        case LV_KEY_ESC:   ctm_request_close(); break;
        default: break;
    }
}

static void ctm_nav_cancel_cb(lv_event_t *e) {
    LV_UNUSED(e);
    ctm_request_close();
}

/* Short sidebar label: kind badge for known controllers, device name for HID. */
static const char *ctm_dev_label(const ctm_bridge_dev_t *d) {
    if (strcmp(d->kind, "ds5") == 0 || strcmp(d->kind, "ds5_usb") == 0)  return "DS5";
    if (strcmp(d->kind, "ds5e") == 0 || strcmp(d->kind, "ds5e_usb") == 0) return "DS5 Edge";
    if (strcmp(d->kind, "ds4") == 0)  return "DS4";
    if (strcmp(d->kind, "puck") == 0) return "Steam Puck";
    if (strcmp(d->kind, "xbox") == 0) return "Xbox";
    return d->name;
}

static lv_obj_t *ctm_make_dev_row(const ctm_bridge_dev_t *d, int idx) {
    lv_obj_t *row = lv_obj_create(s_ctm_sidebar);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(row, LV_DPX(10), 0);
    lv_obj_set_style_pad_ver(row, LV_DPX(9), 0);
    lv_obj_set_style_radius(row, LV_DPX(6), 0);
    lv_obj_set_style_bg_color(row, CTM_COL_ROW, 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(row, CTM_COL_FOCUS, LV_STATE_FOCUS_KEY);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_STATE_FOCUS_KEY);
    /* Persistent selection: stays lit while focus is in the detail pane. */
    lv_obj_set_style_bg_color(row, lv_color_hex(0x223046), LV_STATE_CHECKED);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_STATE_CHECKED);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(row, LV_OBJ_FLAG_EVENT_BUBBLE);   /* Back bubbles to the sidebar */

    lv_obj_t *name = lv_label_create(row);
    lv_label_set_text(name, ctm_dev_label(d));
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
    lv_obj_set_flex_grow(name, 1);
    lv_obj_set_style_text_color(name, CTM_COL_TXT, 0);

    lv_obj_t *st = lv_label_create(row);
    if (strcmp(d->kind, "hid") == 0) {
        lv_label_set_text(st, "(hid)");
        lv_obj_set_style_text_color(st, CTM_COL_SUB, 0);
    } else if (d->plugged) {
        lv_label_set_text(st, "plugged");
        lv_obj_set_style_text_color(st, CTM_COL_OK, 0);
    } else {
        lv_label_set_text(st, "idle");
        lv_obj_set_style_text_color(st, CTM_COL_SUB, 0);
    }
    lv_obj_set_style_text_font(st, lv_theme_get_font_small(row), 0);

    lv_group_add_obj(s_ctm_nav_group, row);
    lv_obj_add_event_cb(row, ctm_nav_key_cb, LV_EVENT_KEY, (void *) (intptr_t) idx);
    lv_obj_add_event_cb(row, ctm_nav_cancel_cb, LV_EVENT_CANCEL, NULL);
    lv_obj_add_event_cb(row, ctm_dev_focus_cb, LV_EVENT_FOCUSED, (void *) (intptr_t) idx);
    lv_obj_add_event_cb(row, ctm_dev_click_cb, LV_EVENT_CLICKED, (void *) (intptr_t) idx);
    return row;
}

static void ctm_act_plugall_cb(lv_event_t *e)   { LV_UNUSED(e); ctm_bridge_plug_all();   ctm_request_refresh(); }
static void ctm_act_unplugall_cb(lv_event_t *e) { LV_UNUSED(e); ctm_bridge_unplug_all(); ctm_request_refresh(); }
static void ctm_act_close_cb(lv_event_t *e)     { LV_UNUSED(e); ctm_request_close(); }

static void ctm_make_action(const char *label, lv_event_cb_t cb, lv_color_t bg) {
    lv_obj_t *btn = ctm_nice_btn(s_ctm_sidebar, label, bg);
    lv_obj_set_width(btn, LV_PCT(100));
    lv_group_add_obj(s_ctm_nav_group, btn);
    lv_obj_add_event_cb(btn, ctm_nav_key_cb, LV_EVENT_KEY, (void *) (intptr_t) -1);
    lv_obj_add_event_cb(btn, ctm_nav_cancel_cb, LV_EVENT_CANCEL, NULL);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
}

static void ctm_panel_refresh(void) {
    if (!s_ctm_panel) {
        return;
    }
    s_ctm_ndev = ctm_bridge_list(s_ctm_devs, 16);

    lv_group_remove_all_objs(s_ctm_nav_group);
    lv_obj_clean(s_ctm_sidebar);
    for (int i = 0; i < 16; ++i) s_ctm_dev_rows[i] = NULL;

    lv_obj_t *caption = lv_label_create(s_ctm_sidebar);
    lv_label_set_text(caption, "CONTROLLERS");
    lv_obj_set_style_text_color(caption, CTM_COL_SUB, 0);
    lv_obj_set_style_text_font(caption, lv_theme_get_font_small(caption), 0);

    if (s_ctm_status_lbl) {
        int plugged = 0;
        for (int i = 0; i < s_ctm_ndev; ++i) {
            if (s_ctm_devs[i].plugged) plugged++;
        }
        char agent[64];
        ctm_bridge_agent(agent, sizeof agent);
        lv_label_set_text_fmt(s_ctm_status_lbl, "Agent %s  -  %d bridged", agent, plugged);
    }

    if (s_ctm_ndev == 0) {
        lv_obj_t *l = lv_label_create(s_ctm_sidebar);
        lv_label_set_text(l, "No controllers detected.");
        lv_obj_set_style_text_color(l, CTM_COL_SUB, 0);
        lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(l, LV_PCT(100));
    }
    for (int i = 0; i < s_ctm_ndev; ++i) {
        s_ctm_dev_rows[i] = ctm_make_dev_row(&s_ctm_devs[i], i);
    }
    lv_obj_t *divider = lv_obj_create(s_ctm_sidebar);
    lv_obj_remove_style_all(divider);
    lv_obj_set_size(divider, LV_PCT(100), LV_DPX(1));
    lv_obj_set_style_bg_color(divider, CTM_COL_BORDER, 0);
    lv_obj_set_style_bg_opa(divider, LV_OPA_COVER, 0);
    ctm_make_action("Plug all", ctm_act_plugall_cb, lv_palette_darken(LV_PALETTE_GREEN, 2));
    ctm_make_action("Unplug all", ctm_act_unplugall_cb, lv_palette_darken(LV_PALETTE_BLUE_GREY, 2));

    if (s_ctm_sel >= s_ctm_ndev) {
        s_ctm_sel = s_ctm_ndev > 0 ? s_ctm_ndev - 1 : 0;
    }

    /* ⭐ ONE GROUP NOW. There is no second pane to hand focus to, so a refresh
     * cannot leave focus somewhere that no longer exists -- which is what the
     * branch here used to guard against. */
    {
        app_input_set_group(&s_ctm_owner->global->ui.input, s_ctm_nav_group);
        if (s_ctm_ndev > 0 && s_ctm_dev_rows[s_ctm_sel]) {
            lv_group_focus_obj(s_ctm_dev_rows[s_ctm_sel]);
            lv_obj_add_state(s_ctm_dev_rows[s_ctm_sel], LV_STATE_FOCUS_KEY);
        }
    }
}

static void ctm_teardown_async(void *p) {
    LV_UNUSED(p);
    if (s_ctm_dead_panel)  { lv_obj_del(s_ctm_dead_panel);    s_ctm_dead_panel = NULL; }
    if (s_ctm_dead_nav)    { lv_group_del(s_ctm_dead_nav);    s_ctm_dead_nav = NULL; }
}

static void ctm_close_panel(void) {
    if (!s_ctm_panel) {
        return;
    }
    /* Hide NOW (same-frame UI change) and move input back to the overlay; the old
     * group stays alive until the async teardown, so the in-flight keypad event
     * that triggered this close keeps a valid group pointer. */
    lv_obj_add_flag(s_ctm_panel, LV_OBJ_FLAG_HIDDEN);
    if (s_ctm_owner) {
        app_input_set_group(&s_ctm_owner->global->ui.input, s_ctm_owner->group);
    }
    s_ctm_dead_panel = s_ctm_panel;
    s_ctm_dead_nav = s_ctm_nav_group;
    s_ctm_panel = NULL;
    s_ctm_sidebar = NULL;
    s_ctm_status_lbl = NULL;
    s_ctm_nav_group = NULL;
    s_ctm_owner = NULL;
    lv_async_call(ctm_teardown_async, NULL);
}

/* Close is already deferred internally, so call it directly (synchronous hide). */
static void ctm_request_close(void) { ctm_close_panel(); }

static void ctm_refresh_async(void *p) { LV_UNUSED(p); ctm_panel_refresh(); }
static void ctm_request_refresh(void)  { lv_async_call(ctm_refresh_async, NULL); }

static void open_ctm_panel(lv_event_t *event) {
    /* The CTM button has LV_OBJ_FLAG_EVENT_BUBBLE; stop the CLICKED here so it
     * never reaches the overlay root's hide_overlay handler. hide_overlay calls
     * app_set_mouse_grab(true) -> SDL_ShowCursor(FALSE) (that hid the webOS
     * pointer) and hides the action bar. THIS is the real "pointer disappears". */
    lv_event_stop_bubbling(event);
    streaming_controller_t *controller = lv_event_get_user_data(event);
    if (s_ctm_panel || !controller) {
        return;
    }
    s_ctm_owner = controller;
    s_ctm_sel = 0;

    s_ctm_nav_group = lv_group_create();
    lv_group_set_wrap(s_ctm_nav_group, false);

    /* Full-screen dim backdrop on the act screen (detached_root) so it does NOT
     * flip the UI into key/gamepad mode the way a modal does (that hid the webOS
     * cursor). Clickable so stray clicks don't dismiss the streaming overlay. */
    lv_obj_t *panel = lv_obj_create(controller->detached_root);
    s_ctm_panel = panel;
    lv_obj_remove_style_all(panel);
    lv_obj_set_size(panel, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(panel, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_60, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(panel, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_move_foreground(panel);

    lv_obj_t *card = lv_obj_create(panel);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, LV_PCT(82), LV_PCT(84));
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, CTM_COL_CARD, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, LV_DPX(12), 0);
    lv_obj_set_style_border_width(card, LV_DPX(1), 0);
    lv_obj_set_style_border_color(card, CTM_COL_BORDER, 0);
    lv_obj_set_style_pad_all(card, LV_DPX(16), 0);
    lv_obj_set_style_pad_gap(card, LV_DPX(12), 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *header = lv_obj_create(card);
    lv_obj_remove_style_all(header);
    lv_obj_set_size(header, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(header, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_gap(header, LV_DPX(2), 0);
    lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *titlerow = lv_obj_create(header);
    lv_obj_remove_style_all(titlerow);
    lv_obj_set_size(titlerow, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(titlerow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(titlerow, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(titlerow, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *title = lv_label_create(titlerow);
    lv_label_set_text(title, "CTM Bridge");
    lv_obj_set_style_text_color(title, CTM_COL_TXT, 0);
    lv_obj_set_style_text_font(title, lv_theme_get_font_large(title), 0);

    /* Real close button (pointer); keyboard/gamepad close with Back (B). */
    lv_obj_t *closebtn = ctm_nice_btn(titlerow, "Close", lv_palette_darken(LV_PALETTE_RED, 3));
    lv_obj_add_event_cb(closebtn, ctm_act_close_cb, LV_EVENT_CLICKED, NULL);

    s_ctm_status_lbl = lv_label_create(header);
    lv_obj_set_style_text_color(s_ctm_status_lbl, CTM_COL_SUB, 0);
    lv_obj_set_style_text_font(s_ctm_status_lbl, lv_theme_get_font_small(header), 0);

    /* ⭐ ONE COLUMN. The panel was a sidebar and a detail pane side by side; the
     * detail pane is gone, so the list is the whole body and gets the width. */
    s_ctm_sidebar = lv_obj_create(card);
    lv_obj_remove_style_all(s_ctm_sidebar);
    lv_obj_set_width(s_ctm_sidebar, LV_PCT(100));
    lv_obj_set_flex_grow(s_ctm_sidebar, 1);
    lv_obj_set_flex_flow(s_ctm_sidebar, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(s_ctm_sidebar, LV_DPX(8), 0);
    lv_obj_set_style_pad_gap(s_ctm_sidebar, LV_DPX(8), 0);
    lv_obj_set_style_bg_color(s_ctm_sidebar, CTM_COL_SIDEBAR, 0);
    lv_obj_set_style_bg_opa(s_ctm_sidebar, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_ctm_sidebar, LV_DPX(8), 0);
    lv_obj_set_style_border_width(s_ctm_sidebar, LV_DPX(1), 0);
    lv_obj_set_style_border_color(s_ctm_sidebar, CTM_COL_BORDER, 0);
    lv_obj_set_scrollbar_mode(s_ctm_sidebar, LV_SCROLLBAR_MODE_AUTO);
    /* Back backstop: any focusable child bubbles CANCEL up here -> close panel. */
    lv_obj_add_event_cb(s_ctm_sidebar, ctm_nav_cancel_cb, LV_EVENT_CANCEL, NULL);

    app_input_set_group(&controller->global->ui.input, s_ctm_nav_group);
    ctm_panel_refresh();
}

/* ---- the seam ---------------------------------------------------------- */

void ctm_panel_open(lv_event_t *event) {
    open_ctm_panel(event);
}

/* Called from the owner fragment's teardown, which is not ours to move.
 *
 * ⚠️ Everything here was previously written inline in on_delete_obj. It has to
 * happen there rather than on the panel's own close: the fragment can be torn
 * down with the panel still open, and the groups would otherwise outlive the
 * indev that points at them. */
void ctm_panel_on_owner_deleted(streaming_controller_t *controller) {
    if (s_ctm_owner == controller) {
        if (s_ctm_nav_group)    { lv_group_del(s_ctm_nav_group);    s_ctm_nav_group = NULL; }
        s_ctm_panel = NULL;
        s_ctm_sidebar = NULL;
        s_ctm_status_lbl = NULL;
        s_ctm_owner = NULL;
    }
    /* Cancel any in-flight panel teardown; the dead panel is freed with the
     * fragment's detached_root, but its groups must be released here. */
    lv_async_call_cancel(ctm_teardown_async, NULL);
    if (s_ctm_dead_nav)    { lv_group_del(s_ctm_dead_nav);    s_ctm_dead_nav = NULL; }
    s_ctm_dead_panel = NULL;
}
