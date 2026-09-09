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
#include "lvgl/font/material_icons_regular_symbols.h"
#include "lvgl/theme/lv_theme_moonlight.h"

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
/* ⭐ The bridged state and the action that produces it, in one colour.
 *
 * ⛔ Green was tried and rejected 2026-08-20: it is the colour every other part
 * of this interface uses for "fine", so it said nothing about THIS state, and a
 * green Bridge All beside a green FULL made the two look like the same thing.
 * ⓘ THE SAME PURPLE AS THE "USB Bridge" BUTTON IN THE STREAMING OVERLAY -- the
 * one that opens this panel. ⭐ So the colour follows the feature: the button
 * you press to get here, the state it produces, and the action that produces
 * it, all one colour. ⛔ A softer hex was tried first and was not punchy
 * enough beside it. */
#define CTM_COL_FULL     lv_palette_main(LV_PALETTE_PURPLE)


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
static lv_obj_t   *s_ctm_close_btn  = NULL;   /* the X in the title row; in the nav group */
static lv_obj_t   *s_ctm_status_lbl = NULL;   /* "USB Server: <addr> -" */
static lv_obj_t   *s_ctm_state_lbl  = NULL;   /* ONLINE / OFFLINE, the only coloured part */
/* ⭐ The same fact the label shows, kept so the rows and Bridge All can act on
 * it rather than only report it. */
static bool        s_ctm_server_online = true;
/* ⭐⭐ THREE STATES, NOT TWO: not known, online, offline.
 *
 * ⛔ g_agent_online starts false, so the panel used to say OFFLINE the instant
 * it opened -- before a single probe had completed. Usually right, and still a
 * claim we had not earned. ⓘ rhoquinn8217, 2026-08-20.
 *
 * ⭐ While it is not known the rows stay pressable, and a press simply ATTEMPTS
 * the bridge: it either works -- signals, and the state resolves to online -- or
 * it refuses, and the state resolves to offline. ⓘ A command that gets through
 * is proof the agent is there, which is why send_agent_command now sets both
 * flags either way.
 *
 * ⚠️ So a refusal is possible here, deliberately. Greying rows exists to avoid
 * a refusal we KNOW is coming; when nobody knows, finding out is the honest
 * answer and the refusal is real information. */
static bool        s_ctm_server_known = false;

/* ⭐⭐ FLASH THE OFFLINE TAG WHEN SOMEONE TRIES TO BRIDGE ANYWAY.
 *
 * ⛔ Greying alone was not enough -- rhoquinn8217, 2026-08-20: "I couldn't tell." A
 * disabled row says "not now"; it does not say WHY, and the reason is already
 * on screen two lines above. ➡️ Flashing it points at the answer instead of
 * refusing silently.
 *
 * ⓘ An odd count so it ends bright, and it drives itself off a timer rather
 * than the panel's refresh, which is too slow to read as a flash. */
static lv_timer_t *s_ctm_flash_timer = NULL;
static lv_timer_t *s_ctm_online_timer = NULL;
static int         s_ctm_flash_left = 0;

/* ⭐⭐ THE ONLINE LABEL UPDATES ITSELF; THE DEVICE LIST DOES NOT.
 *
 * ⛔ THE PANEL HAD NO TIMER AT ALL. It redrew only when you did something to it
 * -- pressed a row, or Bridge All -- so a listener started while the panel was
 * open went unnoticed until you closed and reopened it. ⓘ rhoquinn8217, 2026-08-20:
 * "why doesn't the panel update live?"
 *
 * ⚠️ AND THE OBVIOUS FIX IS WRONG. A timer calling the full refresh would call
 * ctm_bridge_list, which calls ctm_glue_enumerate EVERY TIME -- and enumeration
 * is the expensive thing here, measured at seconds on the C3. The existing
 * comment on the open path says exactly that, and it is right.
 *
 * ⭐ So this polls the ONE cheap thing: a flag the probe thread already
 * maintains. No enumeration, no list rebuild, just a label. ⓘ The device list
 * still refreshes on action, as before.
 *
 * ⓘ It also removes the case for a refresh button: the only thing that went
 * stale while the panel sat open now does not. */
static void ctm_online_tick(lv_timer_t *t) {
    LV_UNUSED(t);
    if (!s_ctm_state_lbl || s_ctm_flash_left > 0) {
        return;   /* mid-flash: leave the colour alone */
    }
    const bool known = ctm_bridge_agent_probed();
    const bool now = ctm_bridge_agent_online();
    if (known == s_ctm_server_known && now == s_ctm_server_online) {
        return;
    }
    s_ctm_server_known = known;
    s_ctm_server_online = now;
    if (!known) {
        lv_label_set_text(s_ctm_state_lbl, "- N/A");
        lv_obj_set_style_text_color(s_ctm_state_lbl, CTM_COL_SUB, 0);
        ctm_request_refresh();
        return;
    }
    lv_label_set_text(s_ctm_state_lbl, now ? "- ONLINE" : "- OFFLINE");
    lv_obj_set_style_text_color(s_ctm_state_lbl,
                                now ? CTM_COL_OK : lv_palette_main(LV_PALETTE_RED), 0);
    /* ⓘ The rows and Bridge All are drawn from this state, so a change has to
     * rebuild them -- but only on a CHANGE, which is rare. */
    ctm_request_refresh();
}

static void ctm_flash_tick(lv_timer_t *t) {
    if (!s_ctm_state_lbl) {
        lv_timer_del(t);
        s_ctm_flash_timer = NULL;
        return;
    }
    const bool on = (s_ctm_flash_left % 2) == 1;
    lv_obj_set_style_text_color(s_ctm_state_lbl,
                                on ? CTM_COL_TXT : lv_palette_main(LV_PALETTE_RED), 0);
    if (--s_ctm_flash_left <= 0) {
        lv_obj_set_style_text_color(s_ctm_state_lbl, lv_palette_main(LV_PALETTE_RED), 0);
        lv_timer_del(t);
        s_ctm_flash_timer = NULL;
    }
}

static void ctm_flash_offline(void) {
    if (!s_ctm_state_lbl) {
        return;
    }
    s_ctm_flash_left = 7;
    if (!s_ctm_flash_timer) {
        s_ctm_flash_timer = lv_timer_create(ctm_flash_tick, 120, NULL);
    }
}
static lv_group_t *s_ctm_nav_group    = NULL;
static streaming_controller_t *s_ctm_owner = NULL;

static ctm_bridge_dev_t s_ctm_devs[16];
static lv_obj_t        *s_ctm_dev_rows[16];
/* The badge that is NOT lit on each row, and that row's name label. The first
 * is recoloured when the selection moves (LVGL v8 does not give a child the
 * parent's state, so this cannot be a style); the second is measured. */
static lv_obj_t        *s_ctm_dev_offbadge[16];
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
/* ⭐⭐ DIRECTION, NOT TOGGLE. Right hands the device to the PC, left brings it
 * back -- which is what the arrow on the row promises.
 *
 * ⓘ Pressing the direction a device is already in does nothing, deliberately.
 * The row is showing only one arrow, so the other direction has nothing to
 * offer, and doing something anyway would make the arrow a lie.
 *
 * ⓘ The row itself still toggles on Select, for anyone who does not want to
 * think about direction. */
/* ⭐⭐ A ROW WAITING FOR ITS CHANGE TO LAND.
 *
 * ⛔ THE FAULT IT FIXES: pressing bridge or release made the row FLICKER. The
 * panel rebuilds its rows on every refresh, and a refresh that lands before the
 * bridge has finished redraws the row in its OLD state -- so the label flips
 * back, then forward again a moment later.
 *
 * ⭐ Instead the row is greyed from the press until the state it was asked for
 * actually arrives. Nothing flips twice, and the grey says "working on it",
 * which is true: a bridge takes a second or two.
 *
 * ⚠️ Keyed by the device's INDEX, not its row position, because the list can be
 * rebuilt underneath it -- the same trap ctm_toggle_device documents.
 *
 * ⓘ Given a deadline so a bridge that never completes cannot leave a row grey
 * forever. */
#define CTM_PENDING_MS 6000
static int      s_ctm_pending_index = -1;
static bool     s_ctm_pending_want = false;
static uint32_t s_ctm_pending_until = 0;

static void ctm_toggle_device(int row);

static void ctm_bridge_device(int row) {
    if (row < 0 || row >= s_ctm_ndev || s_ctm_devs[row].plugged) {
        return;
    }
    ctm_toggle_device(row);
}

static void ctm_release_device(int row) {
    if (row < 0 || row >= s_ctm_ndev || !s_ctm_devs[row].plugged) {
        return;
    }
    ctm_toggle_device(row);
}

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
    /* ⭐ Remember what we asked for, so the row can grey until it happens. */
    s_ctm_pending_index = index;
    s_ctm_pending_want = !s_ctm_devs[row].plugged;
    s_ctm_pending_until = lv_tick_get() + CTM_PENDING_MS;
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
    /* ⭐ Refuse the press rather than letting it fail. ⛔ With the server
     * offline a bridge cannot work, and attempting it answers with a refusal --
     * red flashes and a buzz, which look exactly like a real failure. ⓘ The row
     * is disabled too; this is the belt to that's braces. */
    if (s_ctm_server_known && !s_ctm_server_online) {
        ctm_flash_offline();
        return;
    }
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
/* Bridge every device that is not already bridged, the same way pressing its
 * row would.
 *
 * ⛔ NOT ctm_bridge_plug_all(): that plugs from the core directly and never
 * tells moonlight, so every controller it bridged stayed in the host's panel
 * as an emulated pad AS WELL -- the host saw each of them twice. The row press
 * goes through the gesture, which retires the emulated pad and records that the
 * bridge is ours.
 *
 * ⚠️ Runs on the UI thread, like the row press, and the same caveat applies:
 * with several devices this is several bridges in a row. Acceptable while a
 * bridge is ~2 seconds; worth revisiting if that changes. */
static void ctm_bridge_all(void) {
    for (int i = 0; i < s_ctm_ndev; ++i) {
        if (!s_ctm_devs[i].plugged) {
            ctm_toggle_device(i);
        }
    }
}

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
        /* ⭐ The unlit badge outlines in grey on the SELECTED row only
         * (rhoquinn8217, 2026-09-08). Its usual CTM_COL_BORDER goes muddy
         * against the selected row's lighter back, and a single brighter grey
         * on every row would make a list of four rows shout. ⛔ This CANNOT be
         * a style state: LVGL v8 does not propagate a parent's state to its
         * children, so the badge never sees LV_STATE_CHECKED and only this
         * loop -- which already runs on every focus change -- can do it. */
        if (s_ctm_dev_offbadge[i]) {
            lv_obj_set_style_border_color(s_ctm_dev_offbadge[i],
                                          i == row ? CTM_COL_SUB : CTM_COL_BORDER, 0);
        }
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
             * the action rather than nothing.
             *
             * ⭐⭐ AND IT TURNED OUT TO BE THE SAFE ONE, by accident. While a
             * bridged controller is MIRRORED on the host (T-116), every press
             * reaches the panel AND whatever has focus behind it -- so X here
             * also activates something in the game or in Steam. Right does too,
             * but "right" in a background app is usually harmless.
             *
             * ⚠️ THAT IS A WORKAROUND WITH A LIFETIME. When the mirror is fixed,
             * this loses its justification and goes back to being a direction
             * key that performs an action -- which is worth removing then, not
             * now. rhoquinn8217, 2026-08-18.
             *
             * ⛔ RIGHT IS NOW BRIDGE-ONLY, not a toggle. It used to release a
             * bridged device too, which contradicted the arrow the row shows --
             * a bridged row offers only the back arrow.
             *
             * ⭐⭐ ON THE ACTION BUTTONS -- row == -1 -- LEFT AND RIGHT MOVE
             * FOCUS INSTEAD. Bridge All and Release All sit SIDE BY SIDE, so
             * reaching for right to get from one to the other is the obvious
             * thing to do, and it did nothing: they are consecutive in the
             * group, so only up and down moved between them. ⓘ rhoquinn8217,
             * 2026-08-20. ➡️ Where two things are drawn beside each other, the
             * key that points that way should go there. */
            if (row < 0) {
                lv_group_focus_next(s_ctm_nav_group);
                lv_obj_scroll_to_view(lv_group_get_focused(s_ctm_nav_group), LV_ANIM_ON);
                break;
            }
            ctm_bridge_device(row);
            break;
        case LV_KEY_LEFT:
            if (row < 0) {
                lv_group_focus_prev(s_ctm_nav_group);
                lv_obj_scroll_to_view(lv_group_get_focused(s_ctm_nav_group), LV_ANIM_ON);
                break;
            }
            /* ⭐ The other half of the same idea: left brings a bridged device
             * back. ⓘ It was not handled at all before, which is why the back
             * arrow did nothing. */
            ctm_release_device(row);
            break;
        case LV_KEY_ESC:   ctm_request_close(); break;
        default: break;
    }
}

static void ctm_nav_cancel_cb(lv_event_t *e) {
    LV_UNUSED(e);
    ctm_request_close();
}

/* The close corner was pressed, by pointer or by Select. */
static void ctm_close_click_cb(lv_event_t *e) {
    LV_UNUSED(e);
    ctm_request_close();
}

/* BASIC clicked means release, FULL clicked means bridge -- the same absolute
 * directions the Left and Right keys give, so a pointer and a d-pad say the
 * same thing. ⓘ Each refuses a press that would not change anything. */
static void ctm_badge_basic_cb(lv_event_t *e) {
    ctm_release_device((int) (intptr_t) lv_event_get_user_data(e));
}

static void ctm_badge_full_cb(lv_event_t *e) {
    ctm_bridge_device((int) (intptr_t) lv_event_get_user_data(e));
}

/* One state badge. Lit, it is the state the row is in; unlit, it is an outline
 * of the other state, so the pair reads as a switch with two positions.
 *
 * ⭐ A POINTER TARGET, BUT NOT A NAVIGATION STOP (rhoquinn8217, 2026-09-08).
 * Clickable and navigable are independent in LVGL, so the badge takes a click
 * without ever joining the group: a remote with a pointer gains two targets,
 * and a remote with only a d-pad -- the worst case, and the one to design for
 * -- gains no extra presses. ⛔ Do NOT lv_group_add_obj() these.
 *
 * Returns the badge so the caller can keep the unlit one for recolouring. */
static lv_obj_t *ctm_make_badge(lv_obj_t *parent, const char *text, bool lit,
                                lv_color_t lit_bg, lv_color_t lit_txt,
                                lv_event_cb_t cb, int idx) {
    lv_obj_t *badge = lv_obj_create(parent);
    lv_obj_remove_style_all(badge);
    lv_obj_set_size(badge, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_hor(badge, LV_DPX(10), 0);
    lv_obj_set_style_pad_ver(badge, LV_DPX(3), 0);
    lv_obj_set_style_radius(badge, LV_DPX(4), 0);
    lv_obj_set_style_border_width(badge, LV_DPX(1), 0);
    lv_obj_set_style_border_color(badge, lit ? lit_bg : CTM_COL_BORDER, 0);
    lv_obj_set_style_bg_color(badge, lit_bg, 0);
    lv_obj_set_style_bg_opa(badge, lit ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(badge, LV_OBJ_FLAG_SCROLLABLE);
    /* ⭐ Darkens under the press, the way the action buttons do, so a click
     * that lands is felt (rhoquinn8217, 2026-09-08). ⓘ The unlit badge has no
     * fill to darken, so it GAINS one for the press instead -- without it a
     * click on BASIC gave no feedback at all. ✅ Unlike the selection grey
     * above, PRESSED is the badge's own state, so this one is a plain style. */
    lv_obj_set_style_bg_color(badge, lv_color_darken(lit ? lit_bg : CTM_COL_ROW, LV_OPA_40),
                              LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(badge, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_add_flag(badge, LV_OBJ_FLAG_CLICKABLE);
    /* ⛔ Not click-focusable: the badge is outside the group, so taking focus
     * on a click would move it nowhere and steal it from the row. */
    lv_obj_clear_flag(badge, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_obj_add_event_cb(badge, cb, LV_EVENT_CLICKED, (void *) (intptr_t) idx);
    lv_obj_t *st = lv_label_create(badge);
    lv_label_set_text(st, text);
    lv_obj_set_style_text_color(st, lit ? lit_txt : CTM_COL_SUB, 0);
    lv_obj_set_style_text_font(st, lv_theme_get_font_normal(parent), 0);
    return badge;
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
    /* ⭐ Name on top, action beneath. ⛔ Side by side left the button competing
     * with a long device name for the same width, and the name is the part that
     * cannot be shortened. ⓘ rhoquinn8217, 2026-08-20. */
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_gap(row, 0, 0);
    lv_obj_set_style_pad_hor(row, LV_DPX(10), 0);
    /* ⭐ Tighter than it was. ⛔ Two lines per row makes a short list long, and
     * the padding was sized for the one-line version. ⓘ The gap between the two
     * lines is cut too -- see the pad_gap below. */
    lv_obj_set_style_pad_ver(row, LV_DPX(4), 0);
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
    /* ⛔⛔ NO flex_grow HERE. The row is a COLUMN now, and in a column grow takes
     * leftover HEIGHT -- so a zero-width box stretched down the whole list and
     * the panel became one blue block. Measured on hardware, 2026-08-20.
     * ⭐ Full width, height from its contents. */
    lv_obj_set_width(textcol, LV_PCT(100));
    lv_obj_set_height(textcol, LV_SIZE_CONTENT);
    /* ⭐ Two columns: what the device IS on the left, what STATE it is in on
     * the right, each stacked under its own heading. ⓘ They are aligned to
     * their TOPS, so the name and the badges share a line and what sits under
     * each is free to differ in height. */
    lv_obj_set_flex_flow(textcol, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(textcol, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_gap(textcol, LV_DPX(8), 0);
    lv_obj_set_style_pad_all(textcol, 0, 0);
    lv_obj_clear_flag(textcol, LV_OBJ_FLAG_SCROLLABLE);

    /* ⭐ "DualSense · 1st" -- which controller this is, not just what kind.
     *
     * SDL numbers them from zero and the bridge core knows nothing about SDL,
     * so the hidraw node is the join. ⓘ A mouse or a keyboard has no player
     * number and simply shows none. */
    /* ⭐ The identity column. ⛔ The grow belongs HERE, not on the name: textcol
     * is a ROW so grow takes leftover WIDTH, which is what the name wants --
     * but the name now lives one level down in a COLUMN, where grow would take
     * HEIGHT and stretch it down the list (the 2026-08-20 bug, one level in). */
    lv_obj_t *namecol = lv_obj_create(textcol);
    lv_obj_remove_style_all(namecol);
    lv_obj_set_width(namecol, 1);
    lv_obj_set_flex_grow(namecol, 1);
    lv_obj_set_height(namecol, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(namecol, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(namecol, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_all(namecol, 0, 0);
    lv_obj_set_style_pad_gap(namecol, LV_DPX(2), 0);
    lv_obj_clear_flag(namecol, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *name = lv_label_create(namecol);
    {
        /* ⭐ "(1) DualSense" rather than "DualSense  1st". Shorter, which the
         * row needs -- and the number reads as an identifier rather than a
         * ranking. ⓘ It also leads, so a column of rows can be scanned by
         * number. ⚠️ Ordinals were also four separate strings to translate. */
        /* ⭐ "(1) DualSense" -- the number prepended rather than given a column
         * of its own. ⛔ A separate column was tried and wasted width the name
         * needed. ⓘ Anything without a player number just shows its name. */
        const int player = ctm_bridge_gesture_player_for_node(d->node);
        if (player >= 0 && player < 4) {
            lv_label_set_text_fmt(name, "(%d) %s", player + 1, ctm_dev_label(d));
        } else {
            lv_label_set_text(name, ctm_dev_label(d));
        }
    }
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
    /* ⛔ LONG_DOT only truncates a label with a WIDTH. Left to size itself it
     * grows and wraps instead, and an unrecognised device -- "RONGYUAN 2.4G
     * Wireless Device System Control" -- took three lines and a third of the
     * panel. */
    /* ⛔ LONG_DOT only truncates a label that HAS a width. Left to size itself
     * it grows and wraps -- measured on hardware: "DualSense Edge Wireless
     * Controller" took three lines. */
    /* ⓘ Takes what the tag leaves, and truncates rather than wrapping -- inside
     * a ROW, grow means leftover WIDTH, which is what is wanted here. */
    /* ⓘ Full width OF THE COLUMN, which is what LONG_DOT needs to truncate
     * against. The column is what grows; this just fills it. */
    lv_obj_set_width(name, LV_PCT(100));
    lv_obj_set_style_text_color(name, CTM_COL_TXT, 0);
    lv_obj_set_style_text_font(name, lv_theme_get_font_normal(textcol), 0);

    /* ⭐ THE ADDRESS, UNDER THE NAME (rhoquinn8217, 2026-09-08). It fills the
     * space the action label left when it moved right, and it earns the room:
     * it is the identity the whole project keys on, and the only thing that
     * tells two identically named controllers apart.
     *
     * ⭐⭐ THE MAC, NOT THE CORE'S `uniq`, AND THAT IS THE WHOLE POINT
     * (rhoquinn8217, 2026-09-08).
     *
     * ⛔⛔ `uniq` DOES NOT IDENTIFY THE CONTROLLER. Ever. Measured on the C1
     * 2026-09-08 with one of each connection:
     *
     *   Edge, direct USB cable   uniq = (empty)            SDL = 14-3a-9a-cb-f6-9d
     *   DualSense, DS5dongle     uniq = 948D3F0AD521619B2  SDL = 7c-66-ef-82-10-ed
     *
     * ➡️ Cabled it is EMPTY, and through a dongle it is the DONGLE'S OWN serial
     * -- the Pico 2 W's, not the pad's. ⚠️ rhoquinn8217 found this: the
     * 17-character strings that looked like controller serials were dongles all
     * along, and they follow the DONGLE across a controller swap.
     * ⭐ The MAC is the pad's own, identical on both paths and over Bluetooth,
     * so it is what belongs here and what a mark must key on. ⛔ Keying on uniq
     * would mark the dongle: move the pad and the mark stays behind.
     *
     * ⓘ It comes from SDL, which reads feature report 0x09 -- available cabled,
     * confirmed on hardware. ⚠️ Only for devices SDL opens as controllers, so a
     * mouse or a headset falls back to whatever the core reported, and anything
     * with nothing at all gets a dash rather than an empty line, so every row
     * keeps the same height. Printed exactly as reported, never prettified. */
    lv_obj_t *mac = lv_label_create(namecol);
    {
        char addr[64];
        if (ctm_bridge_gesture_mac_for_node(d->node, addr, sizeof addr)) {
            lv_label_set_text(mac, addr);
        } else {
            lv_label_set_text(mac, d->mac[0] ? d->mac : "--");
        }
    }
    lv_label_set_long_mode(mac, LV_LABEL_LONG_DOT);
    lv_obj_set_width(mac, LV_PCT(100));
    lv_obj_set_style_text_color(mac, CTM_COL_SUB, 0);
    lv_obj_set_style_text_font(mac, lv_theme_get_font_small(textcol), 0);

    /* What the device can do, not what we did to it. FULL is a bridged device:
     * speaker, haptics, adaptive triggers, microphone. BASIC is anything
     * reaching the host through the stream's own emulation -- buttons, sticks,
     * gyro, touchpad, rumble.
     *
     * ⛔ "idle" was wrong for an unbridged controller, and for a mouse: the
     * stream carries them, so they are WORKING, just by the other path. */

    /* The action, drawn as a button. ⚠️ NOT actually clickable in its own right:
     * the whole row takes the press, so this is an affordance rather than a
     * second target to aim at. */
    /* ⭐⭐ FULL IN A GREEN BOX, BASIC AS PLAIN TEXT.
     *
     * ⛔ Colour used to be on the ACTION button, and it read backwards: a big
     * red block beside a controller that was working looked like an alarm, and
     * a big green one beside a basic controller looked like all was well. ⓘ
     * rhoquinn8217, 2026-08-20. ➡️ The colour belongs on the STATE, which is the thing
     * it describes.
     *
     * ⭐ A box rather than green text, so the good state is the one that stands
     * out -- BASIC is the absence of it rather than a warning of its own. */
    /* ⭐⭐ BOTH BADGES, ALWAYS, THE CURRENT ONE LIT (rhoquinn8217, 2026-09-08).
     * BASIC then FULL, in the direction the arrow key sends a controller. Not
     * controls: the row is the target, and the keys are Left to release, Right
     * to bridge, Select to toggle. The lit badge is the state; the other is an
     * outline, so the pair reads as a switch with two positions rather than a
     * word that changes. ⓘ It used to show one badge, FULL purple or BASIC
     * grey, and the row had to be re-read to know which of the two it was. */
    /* ⭐ The state column. ⓘ Its width comes from the badge pair, which is the
     * widest thing in it, so CENTRE-aligning its children puts the action label
     * under the middle of the badges no matter what either of them says. ⛔ That
     * is why this is a container and not a padding: a padding would have to be
     * re-guessed every time a word changed. */
    lv_obj_t *statecol = lv_obj_create(textcol);
    lv_obj_remove_style_all(statecol);
    lv_obj_set_size(statecol, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(statecol, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(statecol, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(statecol, 0, 0);
    lv_obj_set_style_pad_gap(statecol, LV_DPX(2), 0);
    lv_obj_clear_flag(statecol, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *badgerow = lv_obj_create(statecol);
    lv_obj_remove_style_all(badgerow);
    lv_obj_set_size(badgerow, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(badgerow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(badgerow, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(badgerow, 0, 0);
    lv_obj_set_style_pad_gap(badgerow, LV_DPX(8), 0);
    lv_obj_clear_flag(badgerow, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *b_basic = ctm_make_badge(badgerow, "BASIC", !d->plugged,
                                       lv_color_hex(0x4a5866), CTM_COL_SUB,
                                       ctm_badge_basic_cb, idx);
    lv_obj_t *b_full  = ctm_make_badge(badgerow, "FULL", d->plugged,
                                       CTM_COL_FULL, CTM_COL_TXT,
                                       ctm_badge_full_cb, idx);
    if (idx >= 0 && idx < 16) {
        s_ctm_dev_offbadge[idx] = d->plugged ? b_basic : b_full;
    }

    /* ⭐ CENTRED UNDER THE BADGES (rhoquinn8217, 2026-09-08), by living inside
     * the state column rather than on a full-width line of its own. It began
     * left under the name, where its arrows pointed at nothing, then
     * right-aligned, which lined up its END rather than its middle. ➡️ In the
     * column the centring is structural and needs no number. */
    lv_obj_t *act = lv_obj_create(statecol);
    lv_obj_remove_style_all(act);
    lv_obj_set_size(act, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(act, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(act, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    /* ⛔ THE ROW'S OWN PADDING WAS NEVER THE HEIGHT. Its children carried their
     * own, so trimming the row alone changed nothing visible -- measured
     * 2026-08-20 after a first attempt did exactly that. */
    lv_obj_set_style_pad_hor(act, 0, 0);
    lv_obj_set_style_pad_ver(act, 0, 0);
    lv_obj_set_style_radius(act, LV_DPX(4), 0);
    /* ⭐ NO CHROME AND NO COLOUR ON THE ACTION. ⛔ A coloured button here read as
     * a status light and contradicted the real one: red beside a working
     * controller looked like a fault. ➡️ The action is a quiet line of text; the
     * colour lives on the state above it. */
    lv_obj_set_style_bg_opa(act, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(act, 0, 0);
    lv_obj_clear_flag(act, LV_OBJ_FLAG_SCROLLABLE);

    /* ⭐⭐ THE ARROW SITS ON THE SIDE YOU WOULD PRESS, so the glyph teaches the
     * control: right sends the device to the PC, left brings it back.
     *
     * ⭐ ONLY THE AVAILABLE DIRECTION IS EVER SHOWN. A bridged device offers
     * "release" and nothing else; pressing right does nothing, and showing it
     * would promise otherwise. ⓘ The row itself still takes a press, which
     * toggles -- that is the path for anyone who does not want to think about
     * direction. */
    lv_obj_t *actlbl = lv_label_create(act);
    /* ⛔ PLAIN CHARACTERS, NOT LV_SYMBOL_*. LV_SYMBOL_LEFT was tried on
     * 2026-08-20 and rendered as an empty box -- the small theme font on these
     * rows has no symbol range. ⓘ "<" and ">" are in every font there is. */
    /* ⭐ "Click to ..." says what pressing DOES, which is the whole point of the
     * row being a control rather than a report. ⓘ The state is already on the
     * line above, so the button carries only the action.
     *
     * ⛔ PLAIN CHARACTERS, NOT LV_SYMBOL_*. LV_SYMBOL_LEFT rendered as an empty
     * box -- the small theme font on these rows has no symbol range. */
    /* ⭐ Shorter (rhoquinn8217, 2026-09-08): the badges now say the state and the
     * arrow says the direction, so "Click to" was carrying nothing. */
    lv_label_set_text(actlbl, d->plugged ? "< Release" : "Bridge >");
    lv_obj_set_style_text_color(actlbl, CTM_COL_TXT, 0);
    lv_obj_set_style_text_font(actlbl, lv_theme_get_font_small(act), 0);
    lv_obj_set_style_text_color(actlbl, CTM_COL_SUB, 0);

    /* ⭐ Grey while the change we asked for has not arrived. ⓘ Cleared here
     * rather than on a timer: the row is rebuilt on every refresh, so the first
     * rebuild that shows the wanted state is the moment it landed. */
    /* ⭐ Faded hard while the server is offline -- nothing in the list can be
     * bridged, and a light touch of grey was invisible on a television. ⓘ A
     * BRIDGED row is left alone: releasing it is a teardown on this side and
     * needs no host, so it is still worth pressing. */
    if (s_ctm_server_known && !s_ctm_server_online && !d->plugged) {
        lv_obj_set_style_opa(row, LV_OPA_30, 0);
    }

    if (s_ctm_pending_index == d->index) {
        if (d->plugged == s_ctm_pending_want || lv_tick_get() > s_ctm_pending_until) {
            s_ctm_pending_index = -1;
        } else {
            lv_obj_set_style_opa(row, LV_OPA_50, 0);
        }
    }

    lv_group_add_obj(s_ctm_nav_group, row);
    lv_obj_add_event_cb(row, ctm_nav_key_cb, LV_EVENT_KEY, (void *) (intptr_t) idx);
    lv_obj_add_event_cb(row, ctm_nav_cancel_cb, LV_EVENT_CANCEL, NULL);
    lv_obj_add_event_cb(row, ctm_dev_focus_cb, LV_EVENT_FOCUSED, (void *) (intptr_t) idx);
    lv_obj_add_event_cb(row, ctm_dev_click_cb, LV_EVENT_CLICKED, (void *) (intptr_t) idx);
    return row;
}

/* ⛔ Guarded rather than trusting LV_STATE_DISABLED, which greys a button but
 * does not reliably stop it being activated. ⭐ And it flashes the reason. */
static void ctm_act_plugall_cb(lv_event_t *e) {
    LV_UNUSED(e);
    if (s_ctm_server_known && !s_ctm_server_online) {
        ctm_flash_offline();
        return;
    }
    ctm_bridge_all();
    ctm_request_refresh();
}
static void ctm_act_unplugall_cb(lv_event_t *e) { LV_UNUSED(e); ctm_release_all(); ctm_request_refresh(); }

/* ⓘ Returns the button so a caller can disable it -- see Bridge All while the
 * server is offline. */
static lv_obj_t *ctm_make_action(const char *label, lv_event_cb_t cb, lv_color_t bg) {
    lv_obj_t *btn = ctm_nice_btn(s_ctm_actions, label, bg);
    lv_obj_set_flex_grow(btn, 1);
    lv_group_add_obj(s_ctm_nav_group, btn);
    lv_obj_add_event_cb(btn, ctm_nav_key_cb, LV_EVENT_KEY, (void *) (intptr_t) -1);
    lv_obj_add_event_cb(btn, ctm_nav_cancel_cb, LV_EVENT_CANCEL, NULL);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
    return btn;
}

static void ctm_panel_refresh(void) {
    if (!s_ctm_panel) {
        return;
    }
    s_ctm_ndev = ctm_bridge_list(s_ctm_devs, 16);

    lv_group_remove_all_objs(s_ctm_nav_group);
    /* The close corner lives in the header, which is not rebuilt, so it goes
     * back into the group first: Up from the first row reaches it. */
    if (s_ctm_close_btn) lv_group_add_obj(s_ctm_nav_group, s_ctm_close_btn);
    lv_obj_clean(s_ctm_sidebar);
    for (int i = 0; i < 16; ++i) {
        s_ctm_dev_rows[i] = NULL;
        s_ctm_dev_offbadge[i] = NULL;
    }

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
    lv_label_set_text(caption, "Device");
    lv_obj_set_style_text_color(caption, CTM_COL_SUB, 0);
    lv_obj_set_style_text_font(caption, lv_theme_get_font_small(caption), 0);

    /* ⭐ Back on 2026-08-20 as "Feature Set", and now it sits over something:
     * the tag is right-aligned on the row's first line, directly beneath it.
     * ⛔ As "Features" it sat over the action button and named nothing. */
    lv_obj_t *capfeat = lv_label_create(caprow);
    lv_label_set_text(capfeat, "Feature Set");
    /* ⭐ In from the right by about four characters (rhoquinn8217, 2026-09-08),
     * so it sits over the BASIC/FULL pair instead of past the end of it. The
     * badges stop short of the row's edge; hard right looked hung off it. */
    lv_obj_set_style_pad_right(capfeat, LV_DPX(26), 0);
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
        /* ⭐ The address shows either way -- OFFLINE says nothing is answering
         * there, not that the address has gone. ⓘ rhoquinn8217, 2026-08-20. */
        s_ctm_server_known = ctm_bridge_agent_probed();
        s_ctm_server_online = ctm_bridge_agent_online();
        lv_label_set_text_fmt(s_ctm_status_lbl, "USB Server: %s", listener);
        if (!s_ctm_server_known) {
            lv_label_set_text(s_ctm_state_lbl, "- N/A");
            lv_obj_set_style_text_color(s_ctm_state_lbl, CTM_COL_SUB, 0);
        } else {
            lv_label_set_text(s_ctm_state_lbl, s_ctm_server_online ? "- ONLINE" : "- OFFLINE");
            lv_obj_set_style_text_color(s_ctm_state_lbl,
                                        s_ctm_server_online ? CTM_COL_OK
                                                            : lv_palette_main(LV_PALETTE_RED), 0);
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
        /* ⭐⭐ WITH THE SERVER OFFLINE, RELEASE ALL IS THE ONLY THING THAT HELPS.
         *
         * ⛔ Bridging cannot work, and trying it gives a refusal -- red flashes
         * and a buzz, indistinguishable from a real failure. ⭐ Releasing is a
         * teardown on this side and needs no host at all.
         *
         * ⚠️ AND IT IS WHAT THE TV NEEDS ANYWAY. Losing the listener does not
         * currently tear anything down: the session loops back and retries
         * forever, so a controller stays claimed and bridged, waiting for a host
         * that is not coming. ⓘ rhoquinn8217, 2026-08-20: "when the listener is down,
         * that's what the TV needs to do anyway." ➡️ T-127 makes it automatic;
         * until then this is the way out. */
        lv_obj_t *plug_all =
                ctm_make_action("Bridge All", ctm_act_plugall_cb, CTM_COL_FULL);
        if (plug_all && s_ctm_server_known && !s_ctm_server_online) {
            /* ⛔ LV_STATE_DISABLED alone was barely visible -- it only shifts
             * the theme's own opacity a little. ⭐ Paint it grey and fade it. */
            lv_obj_add_state(plug_all, LV_STATE_DISABLED);
            lv_obj_set_style_bg_color(plug_all, lv_color_hex(0x3a4552), 0);
            lv_obj_set_style_opa(plug_all, LV_OPA_40, 0);
        }
        /* ⭐ RED, because it takes every device back at once. ⓘ It was blue-grey, which
         * read as the neutral of the pair -- but Bridge All affects one thing at a
         * time in practice and this affects all of them, so it is the one worth
         * hesitating over. ⚠️ It stays available while the server is offline: that
         * is exactly when it is needed. */
        ctm_make_action("Release All", ctm_act_unplugall_cb, lv_palette_darken(LV_PALETTE_RED, 2));
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
    /* ⭐ Wider (rhoquinn8217, 2026-09-08): two badges on the first line and an
     * Auto Bridge box on the second, with "(1) DualSense Edge" still fitting
     * beside them. Anything longer than that truncates. */
    /* ⭐ 38%, MEASURED ON THE C1 rather than guessed (2026-09-08). At 1920 that
     * is a 729px card, in which "(1) DualSense Edge" is given 298px and needs
     * 280 -- an 18px margin, enough that a slightly longer name does not
     * truncate and small enough that no blank gap shows. ⛔ The old 36 and the
     * 302 build's 44 are both void: both were measured while the auto-bridge
     * control still sat in the row, and that has gone to the settings pane. */
    lv_obj_set_width(card, LV_PCT(38));
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    /* ⭐ In from the corner (rhoquinn8217, 2026-09-08): the TV's overscan was
     * clipping the right edge of the card off the picture. */
    lv_obj_align(card, LV_ALIGN_TOP_RIGHT, LV_DPX(-48), LV_DPX(24));
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
    /* ⭐ The title centres (rhoquinn8217, 2026-09-08); the close corner is taken
     * out of the flow below and pinned right, so it cannot push it off centre. */
    lv_obj_set_flex_align(titlerow, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(titlerow, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *title = lv_label_create(titlerow);
    lv_label_set_text(title, "USB Bridge");
    lv_obj_set_style_text_color(title, CTM_COL_TXT, 0);
    lv_obj_set_style_text_font(title, lv_theme_get_font_large(title), 0);

    /* ⭐ A CLOSE CORNER (rhoquinn8217, 2026-09-08). ⓘ The objection that kept one
     * out until now was that a close button only a pointer could press had no
     * place in something driven from a sofa. This one is in the navigation
     * group -- Up from the first row reaches it, Select closes -- and the pointer
     * can click it too. Circle and Back still close as before. */
    s_ctm_close_btn = lv_btn_create(titlerow);
    lv_obj_remove_style_all(s_ctm_close_btn);
    lv_obj_set_size(s_ctm_close_btn, LV_DPX(30), LV_DPX(30));
    /* ⭐ A CIRCLE, like the close on the settings window (rhoquinn8217,
     * 2026-09-08), so the two read as the same control. */
    lv_obj_set_style_radius(s_ctm_close_btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_ctm_close_btn, CTM_COL_ROW, 0);
    lv_obj_set_style_bg_opa(s_ctm_close_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_ctm_close_btn, CTM_COL_FOCUS, LV_STATE_FOCUS_KEY);
    lv_obj_set_style_border_width(s_ctm_close_btn, LV_DPX(1), 0);
    lv_obj_set_style_border_color(s_ctm_close_btn, CTM_COL_BORDER, 0);
    lv_obj_set_style_border_color(s_ctm_close_btn, CTM_COL_FOCUS, LV_STATE_FOCUS_KEY);
    lv_obj_add_flag(s_ctm_close_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_ctm_close_btn, LV_OBJ_FLAG_IGNORE_LAYOUT);   /* pinned, not flowed */
    lv_obj_align(s_ctm_close_btn, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_add_event_cb(s_ctm_close_btn, ctm_close_click_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(s_ctm_close_btn, ctm_nav_key_cb, LV_EVENT_KEY, (void *) (intptr_t) -1);
    lv_obj_add_event_cb(s_ctm_close_btn, ctm_nav_cancel_cb, LV_EVENT_CANCEL, NULL);
    {
        /* ⭐⭐ THE REAL GLYPH, AND WHY IT WORKS HERE WHEN THE ROWS CANNOT.
         *
         * ⛔ LV_SYMBOL_CLOSE drew an empty box, which is what put a letter "X"
         * here. The cause was never the symbol, it was the FONT: these rows
         * take the theme's text font, which carries no symbol range at all.
         * ➡️ MAT_SYMBOL_CLOSE paired with the theme's ICON font renders,
         * exactly as the settings window's close does.
         * ⚠️ Only glyphs listed in res/iconfonts/MaterialIcons-Regular.list are
         * compiled in -- `close` is, `menu` is NOT, which is why MAT_SYMBOL_MENU
         * drew as tofu on build 302. Adding one means regenerating the font. */
        lv_obj_t *x = lv_label_create(s_ctm_close_btn);
        lv_label_set_text_static(x, MAT_SYMBOL_CLOSE);
        lv_obj_set_style_text_font(x, lv_theme_moonlight_get_iconfont_small(s_ctm_close_btn), 0);
        lv_obj_set_style_text_color(x, CTM_COL_TXT, 0);
        lv_obj_clear_flag(x, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_center(x);
    }

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
    /* ⭐⭐ ASK FOR A FRESH READING AS THE PANEL OPENS -- the one moment somebody
     * is definitely reading it. ⓘ rhoquinn8217's suggestion, 2026-08-20.
     *
     * ⛔ IT ONLY ASKS. The probe blocks for up to a second against a host that
     * is not answering, and this runs on the interface thread; doing it here
     * would freeze the overlay for that second. ⭐ The answer arrives on the
     * panel's next refresh, a moment later.
     *
     * ⓘ Without this, a listener started mid-stream went unnoticed until the
     * probe's own interval came round. */
    ctm_bridge_agent_recheck();
    open_ctm_panel(event);

    /* ⭐ Watches the online flag while the panel is up. See ctm_online_tick.
     *
     * ⛔ CREATED HERE, ON THE OPEN PATH. A first attempt put it in
     * ctm_toggle_device by matching the wrong ctm_request_refresh() call -- so
     * it only started when a row was pressed, which is precisely when nobody
     * needs it.
     *
     * ⭐ Any previous timer is deleted first rather than guarded against: a
     * stale non-NULL pointer would otherwise mean no timer is ever made
     * again. */
    if (s_ctm_online_timer) {
        lv_timer_del(s_ctm_online_timer);
    }
    s_ctm_online_timer = lv_timer_create(ctm_online_tick, 1000, NULL);
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
        /* ⛔ The online timer must go with the labels it writes to, or it fires
         * against freed objects. ⓘ The flash timer deletes itself when it runs
         * out; this one does not, because it never stops on its own. */
        if (s_ctm_online_timer) { lv_timer_del(s_ctm_online_timer); s_ctm_online_timer = NULL; }
        if (s_ctm_flash_timer)  { lv_timer_del(s_ctm_flash_timer);  s_ctm_flash_timer = NULL; }
        s_ctm_flash_left = 0;
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
