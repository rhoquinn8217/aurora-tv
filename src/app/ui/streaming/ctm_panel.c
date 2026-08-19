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
#include "input/ctm_bridge_gesture.h"

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
static void ctm_late_refresh_cb(lv_timer_t *t);
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
/* ⛔ ACTIONS LIVE OUTSIDE THE LIST, PINNED BELOW IT.
 *
 * They used to be the last two items in the scrolling container, so with four
 * devices they were pushed below the fold -- and a user who does not think to
 * scroll concludes they are gone. The list is the only thing that should
 * scroll.
 *
 * ⭐ They stay in the SAME focus group, so Down from the last device still
 * reaches them; they simply stay visible while the list moves above. */
static lv_obj_t   *s_ctm_actions    = NULL;
static lv_obj_t   *s_ctm_status_lbl = NULL;   /* "USB Server: <addr> -" */
static lv_obj_t   *s_ctm_state_lbl  = NULL;   /* ONLINE / OFFLINE, the only coloured part */
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
    /* ⛔⛔ PASS THE DEVICE'S index, NOT THE ROW POSITION.
     *
     * ctm_bridge_dev_t.index is an opaque handle into the core's device table;
     * the row position is where the device happens to sit in OUR copy of the
     * list. They agree only while nothing has connected or disconnected --
     * which is exactly when it matters least. Getting this wrong does not
     * fail quietly: it bridges A DIFFERENT DEVICE. */
    const int index = s_ctm_devs[row].index;
    if (s_ctm_devs[row].plugged) {
        ctm_bridge_unplug_index(index);
        ctm_request_refresh();
        return;
    }
    /* ⭐⭐ ASK THE GESTURE TO DO IT, rather than plugging from here.
     *
     * Plugging directly diverged from the chord in ways that were invisible
     * until they bit: the emulated pad was never retired, so the host saw the
     * controller twice; the watcher did not know it owned the bridge, so it
     * never restored anything afterwards; and releasing from the panel then
     * skipped the sequence that ends a bridge properly, which over Bluetooth
     * looked like the controller powering itself off.
     *
     * ⭐ Asking means there is one implementation and the two cannot drift.
     *
     * ⚠️ A keyboard or a mouse is not an SDL controller and has no gesture path
     * to borrow, so the direct plug stays as the fallback -- it is what those
     * devices have always used, and they have none of the problems above
     * because nothing emulates them in the first place. */
    if (!ctm_bridge_gesture_request_bridge(s_ctm_devs[row].node)) {
        ctm_bridge_plug_index(index);
    }
    /* ⛔ ASKING IS NOT BRIDGING. The gesture takes over and the plug happens on
     * a later tick, so a refresh now reads the OLD state and the row still says
     * BASIC. That looked like the press had failed, and pressing again asked
     * for a second bridge on an already-bridged node -- which is what produced
     * a rumble and a tone on the SECOND press.
     *
     * ⭐ So: refresh now for anything that finished immediately, and once more
     * shortly after for the bridge that is still on its way. Two refreshes
     * rather than a timer, because a timer would enumerate on every tick and
     * enumeration is the expensive thing here. */
    ctm_request_refresh();
    lv_timer_t *late = lv_timer_create(ctm_late_refresh_cb, 1200, NULL);
    lv_timer_set_repeat_count(late, 1);
}

/* Release every bridged device, one at a time, the same way a row does.
 *
 * ⛔ NOT ctm_bridge_unplug_all(): that calls release_local_sessions_on_exit(),
 * the APP SHUTDOWN path. It tears down every session at once while holding the
 * device mutex, and with the microphone disarm in the unplug path -- five
 * writes twenty milliseconds apart, per device -- the overlay froze long enough
 * to look like a crash. Measured 2026-08-18.
 *
 * ⚠️ This still runs on the UI thread and still blocks; it just has far less to
 * do, and it does the same thing pressing each row would. Getting these calls
 * off the UI thread is T-062 and is a bigger change than this. */
static void ctm_release_all(void) {
    for (int i = 0; i < s_ctm_ndev; ++i) {
        if (s_ctm_devs[i].plugged) {
            ctm_bridge_unplug_index(s_ctm_devs[i].index);
        }
    }
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
static bool ctm_is(const ctm_bridge_dev_t *d, const char *vid, const char *pid) {
    return strcmp(d->vid, vid) == 0 && strcmp(d->pid, pid) == 0;
}

static const char *ctm_dev_label(const ctm_bridge_dev_t *d) {
    /* ⛔⛔ MATCHED ON VID/PID, NOT ON KIND, AND THAT IS NOT A STYLE CHOICE.
     *
     * ctm_bridge_dev_t declares `char kind[8]`. "ds5e_usb" is eight characters
     * plus a terminator, so it is TRUNCATED to "ds5e_us" on the way in and
     * matches nothing. A wired DualSense Edge therefore fell through to its raw
     * system name -- "Sony Interactive Entertainment DualSense Edge Wireless
     * Controller" -- while a wired DualSense worked, because "ds5_usb" is seven
     * characters and fits exactly.
     *
     * ⭐ vid and pid are what the core matches on anyway, and they cannot be
     * truncated. */
    if (ctm_is(d, "054c", "0ce6")) return "DualSense";
    if (ctm_is(d, "054c", "0df2")) return "DualSense Edge";
    if (ctm_is(d, "054c", "09cc") || ctm_is(d, "054c", "05c4")) return "DualShock 4";
    if (strcmp(d->kind, "puck") == 0) return "Steam Controller";
    if (strcmp(d->kind, "xbox") == 0) return "Xbox Controller";
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
    /* A border, so a row reads as a raised thing rather than a band of colour. */
    lv_obj_set_style_border_width(row, LV_DPX(1), 0);
    lv_obj_set_style_border_color(row, lv_color_hex(0x3a4854), 0);
    lv_obj_set_style_border_color(row, CTM_COL_FOCUS, LV_STATE_FOCUS_KEY);
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

    /* ⭐⭐ THE ROW HAS TO LOOK LIKE A CONTROL, NOT A REPORT.
     *
     * It listed a name and a state and nothing about it suggested it could be
     * pressed -- so the panel read as a status page and the one thing a user
     * needs to do with it was invisible.
     *
     * ⭐ Naming the ACTION is the only treatment that says what pressing it
     * DOES rather than what state the row is in. A chevron was considered and
     * rejected: it conventionally means "goes somewhere", and this goes
     * nowhere. A segmented BASIC|FULL was rejected too -- two segments look
     * like two targets, and on a controller the row is the only target there
     * is.
     *
     * ⚠️ IF THIS MAKES ROWS TOO TALL, the fallback is one line: put the state
     * back beside the name as "DualSense - BASIC" and keep the button. That is
     * a change to this block alone. */
    lv_obj_t *textcol = lv_obj_create(row);
    lv_obj_remove_style_all(textcol);
    lv_obj_set_height(textcol, LV_SIZE_CONTENT);
    lv_obj_set_width(textcol, 1);
    lv_obj_set_flex_grow(textcol, 1);
    lv_obj_set_flex_flow(textcol, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_gap(textcol, LV_DPX(1), 0);
    lv_obj_clear_flag(textcol, LV_OBJ_FLAG_SCROLLABLE);

    /* ⭐ "DualSense · 1st" -- which controller this is, not just what kind.
     *
     * SDL numbers them from zero and the bridge core knows nothing about SDL,
     * so the hidraw node is the join. ⓘ A mouse or a keyboard has no player
     * number and simply shows none. */
    lv_obj_t *name = lv_label_create(textcol);
    {
        static const char *k_ordinal[] = { "1st", "2nd", "3rd", "4th" };
        const int player = ctm_bridge_gesture_player_for_node(d->node);
        if (player >= 0 && player < 4) {
            lv_label_set_text_fmt(name, "%s  %s", ctm_dev_label(d), k_ordinal[player]);
        } else {
            lv_label_set_text(name, ctm_dev_label(d));
        }
    }
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
    /* ⛔ LONG_DOT only truncates a label with a WIDTH. Left to size itself it
     * grows and wraps instead, and an unrecognised device -- "RONGYUAN 2.4G
     * Wireless Device System Control" -- took three lines and a third of the
     * panel. */
    lv_obj_set_width(name, LV_PCT(100));
    lv_obj_set_style_text_color(name, CTM_COL_TXT, 0);

    /* What the device can do, not what we did to it. FULL is a bridged device:
     * speaker, haptics, adaptive triggers, microphone. BASIC is anything
     * reaching the host through the stream's own emulation -- buttons, sticks,
     * gyro, touchpad, rumble.
     *
     * ⛔ "idle" was wrong for an unbridged controller, and for a mouse: the
     * stream carries them, so they are WORKING, just by the other path. */
    lv_obj_t *st = lv_label_create(textcol);
    lv_label_set_text(st, d->plugged ? "FULL" : "BASIC");
    lv_obj_set_style_text_color(st, d->plugged ? CTM_COL_OK : CTM_COL_SUB, 0);
    lv_obj_set_style_text_font(st, lv_theme_get_font_small(textcol), 0);

    /* The action, drawn as a button. ⚠️ NOT actually clickable in its own right:
     * the whole row takes the press, so this is an affordance rather than a
     * second target to aim at. */
    lv_obj_t *act = lv_obj_create(row);
    lv_obj_remove_style_all(act);
    lv_obj_set_size(act, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_hor(act, LV_DPX(10), 0);
    lv_obj_set_style_pad_ver(act, LV_DPX(4), 0);
    lv_obj_set_style_radius(act, LV_DPX(4), 0);
    lv_obj_set_style_bg_color(act, lv_color_hex(0x2f3d49), 0);
    lv_obj_set_style_bg_opa(act, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(act, LV_DPX(1), 0);
    lv_obj_set_style_border_color(act, lv_color_hex(0x4a5866), 0);
    lv_obj_clear_flag(act, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *actlbl = lv_label_create(act);
    lv_label_set_text(actlbl, d->plugged ? "Release" : "Bridge");
    lv_obj_set_style_text_color(actlbl, CTM_COL_TXT, 0);
    lv_obj_set_style_text_font(actlbl, lv_theme_get_font_small(act), 0);
    lv_obj_set_style_text_font(st, lv_theme_get_font_small(row), 0);

    lv_group_add_obj(s_ctm_nav_group, row);
    lv_obj_add_event_cb(row, ctm_nav_key_cb, LV_EVENT_KEY, (void *) (intptr_t) idx);
    lv_obj_add_event_cb(row, ctm_nav_cancel_cb, LV_EVENT_CANCEL, NULL);
    lv_obj_add_event_cb(row, ctm_dev_focus_cb, LV_EVENT_FOCUSED, (void *) (intptr_t) idx);
    lv_obj_add_event_cb(row, ctm_dev_click_cb, LV_EVENT_CLICKED, (void *) (intptr_t) idx);
    return row;
}

static void ctm_act_plugall_cb(lv_event_t *e)   { LV_UNUSED(e); ctm_bridge_plug_all();   ctm_request_refresh(); }
static void ctm_act_unplugall_cb(lv_event_t *e) { LV_UNUSED(e); ctm_release_all(); ctm_request_refresh(); }

static void ctm_make_action(const char *label, lv_event_cb_t cb, lv_color_t bg) {
    lv_obj_t *btn = ctm_nice_btn(s_ctm_actions, label, bg);
    lv_obj_set_flex_grow(btn, 1);
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

    /* ⭐ Two headings, laid out like the rows beneath them -- name on the left,
     * status on the right -- so the FULL/BASIC column reads as something rather
     * than a word floating at the end of a line. */
    lv_obj_t *caprow = lv_obj_create(s_ctm_sidebar);
    lv_obj_remove_style_all(caprow);
    lv_obj_set_size(caprow, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(caprow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(caprow, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(caprow, LV_DPX(10), 0);
    lv_obj_clear_flag(caprow, LV_OBJ_FLAG_SCROLLABLE);

    /* ⛔ "Controllers" was wrong: the list carries mice, keyboards and wireless
     * receivers as readily as pads. */
    lv_obj_t *caption = lv_label_create(caprow);
    lv_label_set_text(caption, "Devices");
    lv_obj_set_style_text_color(caption, CTM_COL_SUB, 0);
    lv_obj_set_style_text_font(caption, lv_theme_get_font_small(caption), 0);

    lv_obj_t *capfeat = lv_label_create(caprow);
    lv_label_set_text(capfeat, "Features");
    lv_obj_set_style_text_color(capfeat, CTM_COL_SUB, 0);
    lv_obj_set_style_text_font(capfeat, lv_theme_get_font_small(capfeat), 0);

    if (s_ctm_status_lbl) {
        int plugged = 0;
        for (int i = 0; i < s_ctm_ndev; ++i) {
            if (s_ctm_devs[i].plugged) plugged++;
        }
        /* ⭐ "Listener", because that is what it is called everywhere else --
         * the docs, the scripts, the log. "Agent" was the odd one out.
         *
         * ⛔ And it reports the LISTENER, not the bridge. A healthy listener
         * with nothing bridged is a normal state; saying "bridge down" there
         * would be wrong and would send someone looking at the TV. This line
         * names the thing to go and check. */
        char listener[64];
        ctm_bridge_agent(listener, sizeof listener);
        /* ⛔ NO COUNT. "0 bridged" sat under a panel called USB Bridge above a
         * button called Bridge all -- the word had stopped carrying meaning,
         * and each row already says what it is.
         *
         * ⭐ "USB Server", not "listener": the Windows side hosts the USB/IP
         * server, and "listener" reads as something eavesdropping to anyone who
         * does not know the networking sense. ⚠️ "Server" alone would be worse
         * -- this setup already has a streaming server, a PC, and a network
         * full of them. The repeated "USB" is the price of being unambiguous. */
        /* ⭐ SAY "online", do not merely imply it with an address. The glue
         * already returns the literal "offline" when the server is down, so the
         * failing case read correctly and the working one just showed an IP --
         * leaving a user to infer that an address means it is up. */
        if (strcmp(listener, "offline") == 0) {
            lv_label_set_text(s_ctm_status_lbl, "USB Server:");
            lv_label_set_text(s_ctm_state_lbl, "- OFFLINE");
            lv_obj_set_style_text_color(s_ctm_state_lbl, lv_palette_main(LV_PALETTE_RED), 0);
        } else {
            lv_label_set_text_fmt(s_ctm_status_lbl, "USB Server: %s", listener);
            lv_label_set_text(s_ctm_state_lbl, "- ONLINE");
            lv_obj_set_style_text_color(s_ctm_state_lbl, CTM_COL_OK, 0);
        }
    }

    if (s_ctm_ndev == 0) {
        lv_obj_t *l = lv_label_create(s_ctm_sidebar);
        lv_label_set_text(l, "No controllers detected.");
        lv_obj_set_style_text_color(l, CTM_COL_SUB, 0);
        lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(l, LV_PCT(100));
    }
    /* ⏸ NO COLLAPSING OF DUPLICATE HID INTERFACES -- tried 2026-08-17 and
     * REVERTED the same evening because it emptied the list entirely.
     *
     * ⚠️ The observation behind it is real: one Razer Orochi appears three
     * times and a wireless receiver adds more, because a modern mouse presents
     * several HID interfaces and each is its own node. ⛔ But we do not yet know
     * what those interfaces ARE, or which one a user would want bridged, and
     * collapsing them was a guess dressed as a fix.
     *
     * ➡️ Understand the interfaces first. Then decide whether to merge, to
     * label, or to leave them alone. */
    for (int i = 0; i < s_ctm_ndev; ++i) {
        s_ctm_dev_rows[i] = ctm_make_dev_row(&s_ctm_devs[i], i);
    }

    /* ⭐ Say so when there is nothing, rather than offering to bridge it.
     * "Bridge all" and "Release all" over an empty list imply devices exist. */
    if (s_ctm_ndev == 0) {
        lv_obj_t *none = lv_label_create(s_ctm_sidebar);
        lv_label_set_text(none, "No devices connected");
        lv_obj_set_style_text_color(none, CTM_COL_SUB, 0);
        lv_obj_set_style_pad_all(none, LV_DPX(8), 0);
    }
    lv_obj_clean(s_ctm_actions);
    if (s_ctm_ndev > 0) {
        ctm_make_action("Bridge All", ctm_act_plugall_cb, lv_palette_darken(LV_PALETTE_GREEN, 2));
        ctm_make_action("Release All", ctm_act_unplugall_cb, lv_palette_darken(LV_PALETTE_BLUE_GREY, 2));
    }

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
    s_ctm_actions = NULL;
    s_ctm_status_lbl = NULL;
    s_ctm_state_lbl  = NULL;
    s_ctm_nav_group = NULL;
    s_ctm_owner = NULL;
    lv_async_call(ctm_teardown_async, NULL);
}

/* Close is already deferred internally, so call it directly (synchronous hide). */
static void ctm_request_close(void) { ctm_close_panel(); }

static void ctm_refresh_async(void *p) { LV_UNUSED(p); ctm_panel_refresh(); }
static void ctm_request_refresh(void)  { lv_async_call(ctm_refresh_async, NULL); }

/* One-shot, for a bridge that completes after the press. See ctm_toggle_device. */
static void ctm_late_refresh_cb(lv_timer_t *t) {
    LV_UNUSED(t);
    if (s_ctm_panel) {
        ctm_request_refresh();
    }
}

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
    /* ⭐ NO DIM. The backdrop used to darken the whole screen, and that is what
     * made opening the panel feel like leaving the game. It is now an invisible
     * layer that exists only to catch a click outside the strip and to hold it
     * above the video. */
    lv_obj_set_style_bg_opa(panel, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(panel, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_move_foreground(panel);

    lv_obj_t *card = lv_obj_create(panel);
    lv_obj_remove_style_all(card);
    /* ⭐ A CORNER STRIP, NOT A DIALOGUE. Narrow, top-right, and only as tall as
     * its contents -- three devices make a short strip and one makes a shorter
     * one, with no dead space either way.
     *
     * ⚠️ Top-right rather than centred on purpose: it is something to glance at
     * while a game is running, not something to stand in front of it. A
     * full-screen card reads as "you have left the game" however little it
     * contains.
     *
     * ⛔ THE CARD ITSELF IS NOT CAPPED. Capping both it and the list clipped
     * the buttons off the bottom: the list filled the card's limit and the
     * actions had nowhere left to go. The list is the only thing here that can
     * grow without bound, so it is the only thing that needs a limit -- and the
     * card is then always exactly as tall as its header, its list and its
     * buttons. */
    lv_obj_set_width(card, LV_PCT(30));
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_align(card, LV_ALIGN_TOP_RIGHT, LV_DPX(-16), LV_DPX(16));
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
    /* The card's own padding already spaces the title from the top edge; the
     * header was adding its own on top of it. */
    lv_obj_set_style_pad_top(header, 0, 0);
    lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *titlerow = lv_obj_create(header);
    lv_obj_remove_style_all(titlerow);
    lv_obj_set_size(titlerow, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(titlerow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(titlerow, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(titlerow, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *title = lv_label_create(titlerow);
    lv_label_set_text(title, "USB Bridge");
    lv_obj_set_style_text_color(title, CTM_COL_TXT, 0);
    lv_obj_set_style_text_font(title, lv_theme_get_font_large(title), 0);

    /* ⛔ NO CLOSE BUTTON. It could only ever be pressed with a pointer -- it was
     * never in the navigation group, so a controller could not reach it at all.
     * A control only a mouse can use has no place in something driven from a
     * sofa, and there are two working ways out already: circle on a controller,
     * back on the remote. */

    /* ⭐ TWO LABELS, because only the STATE should carry colour. One label
     * cannot be part grey and part green, and colouring the whole line makes
     * the address look like it means something. */
    lv_obj_t *statusrow = lv_obj_create(header);
    lv_obj_remove_style_all(statusrow);
    lv_obj_set_size(statusrow, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(statusrow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(statusrow, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(statusrow, LV_DPX(6), 0);
    lv_obj_clear_flag(statusrow, LV_OBJ_FLAG_SCROLLABLE);

    s_ctm_status_lbl = lv_label_create(statusrow);
    lv_obj_set_style_text_color(s_ctm_status_lbl, CTM_COL_SUB, 0);
    lv_obj_set_style_text_font(s_ctm_status_lbl, lv_theme_get_font_small(statusrow), 0);

    s_ctm_state_lbl = lv_label_create(statusrow);
    lv_obj_set_style_text_font(s_ctm_state_lbl, lv_theme_get_font_small(statusrow), 0);

    /* ⭐ ONE COLUMN. The panel was a sidebar and a detail pane side by side; the
     * detail pane is gone, so the list is the whole body and gets the width. */
    s_ctm_sidebar = lv_obj_create(card);
    lv_obj_remove_style_all(s_ctm_sidebar);
    lv_obj_set_width(s_ctm_sidebar, LV_PCT(100));
/* ⛔⛔ SIZE_CONTENT, NOT GROW -- and getting this wrong made the panel LOOK
     * EMPTY with devices connected.
     *
     * flex_grow means "take the leftover space", and a card sized to its own
     * children HAS no leftover space. The list was given zero height, so every
     * row was built correctly and drawn into nothing. So was the "no devices"
     * label, which is how the mistake gave itself away.
     *
     * ⭐ Sized to its content instead, and capped: the card grows with the list
     * until the cap bites, and past that the list scrolls inside the height it
     * has. A short list makes a short strip either way. */
    lv_obj_set_height(s_ctm_sidebar, LV_SIZE_CONTENT);
    lv_obj_set_style_max_height(s_ctm_sidebar, LV_DPX(320), 0);
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

    /* Pinned below the list, so four devices cannot push these off the panel. */
    s_ctm_actions = lv_obj_create(card);
    lv_obj_remove_style_all(s_ctm_actions);
    lv_obj_set_size(s_ctm_actions, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(s_ctm_actions, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_gap(s_ctm_actions, LV_DPX(8), 0);
    lv_obj_clear_flag(s_ctm_actions, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_ctm_actions, ctm_nav_cancel_cb, LV_EVENT_CANCEL, NULL);

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
        s_ctm_actions = NULL;
        s_ctm_status_lbl = NULL;
        s_ctm_state_lbl  = NULL;
        s_ctm_owner = NULL;
    }
    /* Cancel any in-flight panel teardown; the dead panel is freed with the
     * fragment's detached_root, but its groups must be released here. */
    lv_async_call_cancel(ctm_teardown_async, NULL);
    if (s_ctm_dead_nav)    { lv_group_del(s_ctm_dead_nav);    s_ctm_dead_nav = NULL; }
    s_ctm_dead_panel = NULL;
}
