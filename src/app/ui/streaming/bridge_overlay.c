/* The streaming overlay's buttons of this fork's. The why is in the header. */

#include "bridge_overlay.h"

#include <string.h>

#include "app.h"
#include "logging.h"
#include "util/i18n.h"

#include "ctm_bridge_glue.h"
#include "ctm_panel.h"
#include "bridge_prompt.h"
#include "stream/bridge_session.h"   /* is Virtual Mouse on */
#include "ui/common/bt_panel.h"

/* The points of the Bluetooth button's mark, which the line goes on reading. */
static lv_point_t s_bt_mark[6];

static void open_listener_config(lv_event_t *event);
static void open_bt_devices(lv_event_t *event);
static void vmouse_badge_look(void);

/* ⭐ DS5-USBIP IS GREYED OUT WHILE NOTHING IS BRIDGED, AND STILL SELECTABLE
 * (rhoquinn8217, 2026-10-02). Grey and faded, as the USB Bridge panel greys a
 * button it cannot use, but with no disabled state: it keeps its place in the
 * focus order, its focus outline is drawn at full strength, and a press still
 * says what it needs. ⓘ Decided by the same test a press makes
 * (bridge_open_config_ready), asked four times a second, so the look and the
 * press cannot disagree for longer than that. */
#define DS5USBIP_CHECK_MS 250
#define DS5USBIP_GREY lv_color_hex(0x3a4552)
#define DS5USBIP_FADE LV_OPA_40

static lv_obj_t *s_ds5usbip = NULL;
static lv_timer_t *s_ds5usbip_timer = NULL;
/* 1 greyed, 0 not, -1 not yet decided. */
static int s_ds5usbip_greyed = -1;

static void ds5usbip_look(bool greyed) {
    if (s_ds5usbip == NULL || (int) greyed == s_ds5usbip_greyed) {
        return;
    }
    s_ds5usbip_greyed = greyed;
    lv_obj_t *label = lv_obj_get_child(s_ds5usbip, 0);
    if (greyed) {
        lv_obj_set_style_bg_color(s_ds5usbip, DS5USBIP_GREY, 0);
        lv_obj_set_style_bg_opa(s_ds5usbip, DS5USBIP_FADE, 0);
        if (label != NULL) {
            lv_obj_set_style_text_opa(label, DS5USBIP_FADE, 0);
        }
    } else {
        lv_obj_set_style_bg_color(s_ds5usbip, lv_palette_main(LV_PALETTE_DEEP_PURPLE), 0);
        lv_obj_remove_local_style_prop(s_ds5usbip, LV_STYLE_BG_OPA, 0);
        if (label != NULL) {
            lv_obj_remove_local_style_prop(label, LV_STYLE_TEXT_OPA, 0);
        }
    }
}

static void ds5usbip_tick(lv_timer_t *timer) {
    (void) timer;
    const int ready = bridge_open_config_ready();
    /* ⓘ -1 is "busy, ask again": the look stays as it was until the next tick. */
    if (ready >= 0) {
        ds5usbip_look(ready == 0);
    }
    /* And Virtual Mouse's check, on the same timer (below). */
    vmouse_badge_look();
}

/* ⭐ A CHECK ON VIRTUAL MOUSE WHILE IT IS ON (rhoquinn8217, 2026-10-28: "Lets
 * add a check in the mouse mode button in the overlay when it is on", and of
 * three pictures, B: a small white circle with a green check on the button's
 * top right corner, and nothing while it is off). Virtual Mouse is kept from
 * one stream to the next now (input/bridge_override.h), so the button says
 * whether it is on.
 * ⓘ Asked with DS5-USBIP's look, four times a second, so every way Virtual
 * Mouse is switched (its button, the remote, the control port, the pop-up)
 * shows, with no hook in each.
 * ⓘ The button is upstream's, made just before this file's two
 * (streaming.view.c; the merge guard keeps that order). The badge is a child
 * of it that takes no layout, no focus and no click. */
#define VMOUSE_TICK_COLOUR lv_color_hex(0x2e7d32)

static lv_obj_t *s_vmouse_badge = NULL;
/* The check's three points: lv_line keeps a pointer to them, not a copy. */
static lv_point_t s_vmouse_tick[3];
/* 1 shown, 0 hidden, -1 not yet decided. */
static int s_vmouse_shown = -1;

static void vmouse_badge_look(void) {
    if (s_vmouse_badge == NULL) {
        return;
    }
    const bool on = global != NULL && bridge_session_vmouse_active(global->session);
    if ((int) on == s_vmouse_shown) {
        return;
    }
    s_vmouse_shown = on;
    if (on) {
        lv_obj_clear_flag(s_vmouse_badge, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_vmouse_badge, LV_OBJ_FLAG_HIDDEN);
    }
}

static void vmouse_badge_deleted(lv_event_t *event) {
    if (lv_event_get_target(event) == s_vmouse_badge) {
        s_vmouse_badge = NULL;
        s_vmouse_shown = -1;
    }
}

static void vmouse_badge_create(lv_obj_t *vmouse_btn) {
    lv_obj_t *label = vmouse_btn != NULL ? lv_obj_get_child(vmouse_btn, 0) : NULL;
    if (label == NULL || !lv_obj_check_type(label, &lv_label_class) ||
        strcmp(lv_label_get_text(label), locstr("Virtual Mouse")) != 0) {
        commons_log_warn("Streaming", "the overlay's Virtual Mouse button was not found: no check on it");
        return;
    }
    /* As in the picture: 1.35 times the height of the button's words. */
    const lv_font_t *font = lv_obj_get_style_text_font(label, 0);
    const lv_coord_t text_h = font != NULL ? lv_font_get_line_height(font) : LV_DPX(20);
    const lv_coord_t size = text_h * 135 / 100;
    const lv_coord_t ring = LV_MAX(1, size / 14);
    lv_obj_t *badge = lv_obj_create(vmouse_btn);
    lv_obj_remove_style_all(badge);
    lv_obj_add_flag(badge, LV_OBJ_FLAG_FLOATING | LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(badge, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(badge, size, size);
    lv_obj_set_style_radius(badge, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(badge, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(badge, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(badge, lv_color_black(), 0);
    lv_obj_set_style_border_width(badge, ring, 0);
    /* On the button's corner, a third of it outside. ⓘ Counted from the
     * button's own edge: an aligned child starts inside its padding and
     * border. The gap between buttons is wider than the third, so the next
     * button does not cover it. */
    const lv_coord_t edge = lv_obj_get_style_border_width(vmouse_btn, 0);
    lv_obj_align(badge, LV_ALIGN_TOP_RIGHT,
                 lv_obj_get_style_pad_right(vmouse_btn, 0) + edge + size / 3,
                 -(lv_obj_get_style_pad_top(vmouse_btn, 0) + edge + size / 3));
    lv_obj_add_flag(vmouse_btn, LV_OBJ_FLAG_OVERFLOW_VISIBLE);

    /* The check: the picture's path (5,12.5) (9.4,16.9) (19,7.4) on a 24 grid,
     * in the middle 70% of the circle. ⓘ The points start inside its ring. */
    const lv_coord_t box = size * 70 / 100;
    const lv_coord_t off = (size - box) / 2 - ring;
    s_vmouse_tick[0] = (lv_point_t) {(lv_coord_t) (off + box * 50 / 240), (lv_coord_t) (off + box * 125 / 240)};
    s_vmouse_tick[1] = (lv_point_t) {(lv_coord_t) (off + box * 94 / 240), (lv_coord_t) (off + box * 169 / 240)};
    s_vmouse_tick[2] = (lv_point_t) {(lv_coord_t) (off + box * 190 / 240), (lv_coord_t) (off + box * 74 / 240)};
    lv_obj_t *tick = lv_line_create(badge);
    lv_line_set_points(tick, s_vmouse_tick, 3);
    lv_obj_set_style_line_width(tick, LV_MAX(2, box * 34 / 240), 0);
    lv_obj_set_style_line_color(tick, VMOUSE_TICK_COLOUR, 0);
    lv_obj_set_style_line_rounded(tick, true, 0);

    s_vmouse_badge = badge;
    s_vmouse_shown = -1;
    lv_obj_add_event_cb(badge, vmouse_badge_deleted, LV_EVENT_DELETE, NULL);
    vmouse_badge_look();
}

/* The timer goes with the button, or it would fire at a freed one. */
static void ds5usbip_deleted(lv_event_t *event) {
    if (lv_event_get_target(event) != s_ds5usbip) {
        return;
    }
    if (s_ds5usbip_timer != NULL) {
        lv_timer_del(s_ds5usbip_timer);
        s_ds5usbip_timer = NULL;
    }
    s_ds5usbip = NULL;
    s_ds5usbip_greyed = -1;
}

/* One button in the overlay's row, styled as the overlay's own are. */
static lv_obj_t *overlay_button(streaming_controller_t *controller, lv_obj_t *actions, const char *text,
                                lv_palette_t colour) {
    lv_obj_t *btn = lv_btn_create(actions);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_add_style(btn, &controller->overlay_button_style, 0);
    lv_obj_add_style(btn, &controller->overlay_button_style_focused, LV_STATE_FOCUS_KEY);
    lv_obj_set_style_bg_color(btn, lv_palette_main(colour), 0);
    lv_obj_t *label = lv_label_create(btn);
    lv_obj_add_style(label, &controller->overlay_button_label_style, 0);
    lv_label_set_text(label, text);
    if (app_configuration && !app_configuration->bridge.enable) {
        lv_obj_add_flag(btn, LV_OBJ_FLAG_HIDDEN);
    }
    return btn;
}

void bridge_overlay_buttons_create(streaming_controller_t *controller, lv_obj_t *actions) {
    /* ⓘ Upstream's Virtual Mouse button is the row's last child at this
     * point, for its check (below). */
    lv_obj_t *vmouse_btn = lv_obj_get_child(actions, -1);

    /* ⓘ The panel takes the streaming screen from the click, to know whose
     * overlay it opens over. */
    lv_obj_t *usb_bridge = overlay_button(controller, actions, locstr("USB Bridge"), LV_PALETTE_PURPLE);
    lv_obj_add_event_cb(usb_bridge, ctm_panel_open, LV_EVENT_CLICKED, controller);

    /* ⭐ DS5-USBIP, beside the USB Bridge button and hidden with it, for the
     * same focus-order reason. ⓘ It stands where the Bridge Override switch
     * stood until 2026-10-01, at rhoquinn8217's word; the switch itself went
     * on 2026-10-28 (input/bridge_override.h). */
    lv_obj_t *ds5usbip = overlay_button(controller, actions, locstr("DS5-USBIP"), LV_PALETTE_DEEP_PURPLE);
    lv_obj_add_event_cb(ds5usbip, open_listener_config, LV_EVENT_CLICKED, NULL);

    /* ⭐ The Bluetooth mark, after DS5-USBIP: the Bluetooth Devices panel
     * (ui/common/bt_panel.h). ⓘ Shown whether device bridging is on or not:
     * pairing a controller to the TV has nothing to do with the bridge.
     * ⛔ It read "Controllers" until 2026-10-30, and rhoquinn8217 called that
     * misleading: the panel has only Bluetooth devices, not those on a cable.
     * So it has no words now ("replace the button name with just the bluetooth
     * symbol"): the mark at the height of the row's text, in its colour. The
     * label stays, empty, so the button keeps its neighbours' height. */
    lv_obj_t *bt_devices = overlay_button(controller, actions, "", LV_PALETTE_TEAL);
    lv_obj_t *bt_label = lv_obj_get_child(bt_devices, 0);
    bt_panel_mark(bt_devices, s_bt_mark,
                  lv_font_get_line_height(lv_obj_get_style_text_font(bt_label, LV_PART_MAIN)),
                  lv_obj_get_style_text_color(bt_label, LV_PART_MAIN));
    lv_obj_clear_flag(bt_devices, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(bt_devices, open_bt_devices, LV_EVENT_CLICKED, NULL);

    /* Its look follows whether anything is bridged. ⓘ A timer left by a screen
     * that went without its delete event is replaced rather than trusted. */
    if (s_ds5usbip_timer != NULL) {
        lv_timer_del(s_ds5usbip_timer);
        s_ds5usbip_timer = NULL;
    }
    s_ds5usbip = ds5usbip;
    s_ds5usbip_greyed = -1;
    lv_obj_add_event_cb(ds5usbip, ds5usbip_deleted, LV_EVENT_DELETE, NULL);
    ds5usbip_tick(NULL);
    s_ds5usbip_timer = lv_timer_create(ds5usbip_tick, DS5USBIP_CHECK_MS, NULL);

    vmouse_badge_create(vmouse_btn);
}

/* ⭐ THE DS5-USBIP BUTTON (rhoquinn8217, 2026-09-28: "I want the DS5-USBIP
 * overlay button to work for any pad"). The USB server on the host opens its
 * settings window by itself when a device is bridged. This asks it to do the
 * same again for a device that already is (ctm_bridge_open_config), and the
 * overlay closes behind the press like any other button's, so the window is
 * what the person sees next.
 * ⛔ With nothing bridged there is nobody to ask. The button is grey then
 * (above), and a press keeps the overlay open and says what it needs, rather
 * than closing on a press that did nothing: at the bottom left, where the
 * pop-up a bridge raises leaves its message, in rhoquinn8217's words
 * (2026-10-02). */
static void open_listener_config(lv_event_t *event) {
    char name[128];
    if (ctm_bridge_open_config(name, sizeof(name))) {
        commons_log_info("Streaming", "DS5-USBIP: asked the host for its settings window on %s", name);
        /* ⓘ The click bubbles on to the view, which hides the overlay. */
        return;
    }
    lv_event_stop_bubbling(event);
    commons_log_info("Streaming", "DS5-USBIP: nothing is bridged, so nothing was asked");
    bridge_prompt_note(locstr("Bridge a device to use DS5-USBIP"));
}

/* ⭐ THE OVERLAY STAYS OPEN BEHIND THE PANEL (rhoquinn8217, 2026-10-29: "keep
 * it open"): the click goes no further, so the view does not hide the overlay,
 * and while the overlay is up a bridged controller's presses stay out of the
 * game. Closing the panel lands back on this button. */
static void open_bt_devices(lv_event_t *event) {
    lv_event_stop_bubbling(event);
    commons_log_info("Streaming", "The Bluetooth button: the Bluetooth Devices panel");
    bt_panel_open(true);
}

/* ⭐⭐ THE STATS' PIN BY KEYS (rhoquinn8217, 2026-10-20: "in the streaming
 * overlay, you can navigate to the (x) close button for the performance stats
 * to turn it off or on", and of upstream's pin at the end of the stats' title:
 * "The pin is what I mean"). Pinned, the stats stay on the screen after the
 * overlay closes; unpinned, they show only with it. Upstream takes the pin out
 * of every focus group, and the overlay moves only left and right along its
 * row, so only a pointer reached it.
 *
 * ➡️ Up from the row selects the pin, Enter toggles it (LVGL's own toggle of a
 * checkable button on the release, which upstream's pin_toggle hears), and
 * Down, or Back, goes back to the button Up left. The pin joins the overlay's
 * group only while it is selected, so Left and Right along the row never land
 * on it. ⚠️ Its own keys are caught before LVGL's: a checkable button takes
 * Up and Right as "check" and Left and Down as "uncheck", which would pin or
 * unpin the stats as the person moved away. */
static lv_obj_t *s_pin_wired = NULL;   /* the pin these handlers are on */
static lv_obj_t *s_pin_from = NULL;    /* the row's button Up left */

static void pin_leave(streaming_controller_t *controller) {
    lv_obj_t *pin = controller->stats_pin;
    if (s_pin_from != NULL) {
        lv_group_focus_obj(s_pin_from);
    }
    lv_group_remove_obj(pin);
    s_pin_from = NULL;
}

static void pin_key(lv_event_t *event) {
    streaming_controller_t *controller = lv_event_get_user_data(event);
    switch (lv_event_get_key(event)) {
        case LV_KEY_DOWN:
            pin_leave(controller);
            lv_event_stop_processing(event);
            break;
        case LV_KEY_UP:
        case LV_KEY_LEFT:
        case LV_KEY_RIGHT:
            lv_event_stop_processing(event);
            break;
        default:
            break;
    }
}

/* Back on the pin: the pin keeps the event (it does not bubble), so it goes
 * back to the row and hands Back on to the overlay, which closes as always. */
static void pin_cancel(lv_event_t *event) {
    streaming_controller_t *controller = lv_event_get_user_data(event);
    pin_leave(controller);
    lv_event_send(controller->base.obj, LV_EVENT_CANCEL, NULL);
}

static void pin_deleted(lv_event_t *event) {
    if (lv_event_get_target(event) == s_pin_wired) {
        s_pin_wired = NULL;
        s_pin_from = NULL;
    }
}

bool stats_pin_overlay_key(streaming_controller_t *controller, uint32_t key) {
    if (key != LV_KEY_UP || controller == NULL || controller->group == NULL) {
        return false;
    }
    lv_obj_t *pin = controller->stats_pin;
    if (pin == NULL || controller->stats == NULL || lv_obj_has_flag(controller->stats, LV_OBJ_FLAG_HIDDEN)) {
        return false;
    }
    lv_obj_t *from = lv_group_get_focused(controller->group);
    if (from == pin) {
        return true;
    }
    if (s_pin_wired != pin) {
        lv_obj_add_event_cb(pin, pin_key, LV_EVENT_KEY | LV_EVENT_PREPROCESS, controller);
        lv_obj_add_event_cb(pin, pin_cancel, LV_EVENT_CANCEL, controller);
        lv_obj_add_event_cb(pin, pin_deleted, LV_EVENT_DELETE, NULL);
        s_pin_wired = pin;
    }
    lv_group_add_obj(controller->group, pin);
    lv_group_focus_obj(pin);
    s_pin_from = from;
    return true;
}
