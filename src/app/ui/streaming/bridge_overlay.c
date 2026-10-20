/* The streaming overlay's two buttons of this fork's. The why is in the header. */

#include "bridge_overlay.h"

#include "app.h"
#include "logging.h"
#include "util/i18n.h"

#include "ctm_bridge_glue.h"
#include "ctm_panel.h"
#include "bridge_prompt.h"

static void open_listener_config(lv_event_t *event);

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
    /* ⓘ The panel takes the streaming screen from the click, to know whose
     * overlay it opens over. */
    lv_obj_t *usb_bridge = overlay_button(controller, actions, locstr("USB Bridge"), LV_PALETTE_PURPLE);
    lv_obj_add_event_cb(usb_bridge, ctm_panel_open, LV_EVENT_CLICKED, controller);

    /* ⭐ DS5-USBIP, beside the USB Bridge button and hidden with it, for the
     * same focus-order reason. ⓘ It stands where Bridge Override stood until
     * 2026-10-01, at rhoquinn8217's word. That switch has no button now: the
     * question a bridge raises turns it on (bridge_prompt.h). */
    lv_obj_t *ds5usbip = overlay_button(controller, actions, locstr("DS5-USBIP"), LV_PALETTE_DEEP_PURPLE);
    lv_obj_add_event_cb(ds5usbip, open_listener_config, LV_EVENT_CLICKED, NULL);

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
