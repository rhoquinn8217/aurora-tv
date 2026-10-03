/* The streaming overlay's two buttons of this fork's. The why is in the header. */

#include "bridge_overlay.h"

#include "app.h"
#include "logging.h"
#include "util/i18n.h"

#include "ctm_bridge_glue.h"
#include "ctm_panel.h"
#include "bridge_prompt.h"

static void open_listener_config(lv_event_t *event);

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
}

/* ⭐ THE DS5-USBIP BUTTON (rhoquinn8217, 2026-09-28: "I want the DS5-USBIP
 * overlay button to work for any pad"). The USB server on the host opens its
 * settings window by itself when a device is bridged. This asks it to do the
 * same again for a device that already is (ctm_bridge_open_config), and the
 * overlay closes behind the press like any other button's, so the window is
 * what the person sees next.
 * ⛔ With nothing bridged there is nobody to ask. The overlay stays open and
 * says so, rather than closing on a press that did nothing: at the bottom
 * left, where the pop-up a bridge raises leaves its message (rhoquinn8217,
 * 2026-10-02). */
static void open_listener_config(lv_event_t *event) {
    char name[128];
    if (ctm_bridge_open_config(name, sizeof(name))) {
        commons_log_info("Streaming", "DS5-USBIP: asked the host for its settings window on %s", name);
        /* ⓘ The click bubbles on to the view, which hides the overlay. */
        return;
    }
    lv_event_stop_bubbling(event);
    commons_log_info("Streaming", "DS5-USBIP: nothing is bridged, so nothing was asked");
    bridge_prompt_note(locstr("DS5-USBIP opens for a bridged device, and nothing is bridged.\n"
                              "Bridge one from USB Bridge first."));
}
