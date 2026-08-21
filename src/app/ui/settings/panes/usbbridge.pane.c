/* The USB Bridge settings pane.
 *
 * ⭐ Its own section rather than three switches loose in Input: the feature has
 * more than one setting and the settings only make sense together.
 *
 * ⓘ Bridging is not a mode. A stream behaves exactly as GuiDev1994's does --
 * keyboards, mice and controllers all go to the PC automatically -- and
 * bridging is something done ON TOP of that, to one device at a time. The
 * switch here does not change how a stream works; it decides whether a device
 * can be handed over at all.
 */

#include "app.h"
#include "config.h"

#include "pref_obj.h"

#include "util/i18n.h"
#include "ui/settings/settings.controller.h"

typedef struct {
    lv_fragment_t base;
    /* ⭐ Everything that depends on bridging being enabled, so one callback can
     * grey the lot. Add to this rather than to a second mechanism. */
    lv_obj_t *dependent[5];
    int dependent_count;
} usbbridge_pane_t;

static void pane_ctor(lv_fragment_t *self, void *args);

static void enable_state_update_cb(lv_event_t *e);

/* ⭐⭐ Grey out everything below the master switch when it is off.
 *
 * ⓘ Disabled rather than hidden: the settings stay visible, so it is obvious
 * they exist and obvious why they cannot be changed. Hiding them would look
 * like they had gone. ⓘ Same pattern basic.pane.c uses for HDR and AV1. */
static void usbbridge_apply_enabled(usbbridge_pane_t *pane) {
    if (!pane) return;
    const bool on = app_configuration->bridge_enable;
    for (int i = 0; i < pane->dependent_count; ++i) {
        if (!pane->dependent[i]) continue;
        if (on) {
            lv_obj_clear_state(pane->dependent[i], LV_STATE_DISABLED);
        } else {
            lv_obj_add_state(pane->dependent[i], LV_STATE_DISABLED);
        }
    }
}

/* Add a switch that only means anything while bridging is on. */
static lv_obj_t *dependent_checkbox(usbbridge_pane_t *pane, lv_obj_t *view,
                                    const char *title, bool *value)
{
    lv_obj_t *cb = pref_checkbox(view, title, value, false);
    if (pane->dependent_count < (int) (sizeof(pane->dependent) / sizeof(pane->dependent[0]))) {
        pane->dependent[pane->dependent_count++] = cb;
    }
    return cb;
}

static void enable_state_update_cb(lv_event_t *e) {
    usbbridge_apply_enabled((usbbridge_pane_t *) lv_event_get_user_data(e));
}

static lv_obj_t *create_obj(lv_fragment_t *self, lv_obj_t *container);

const lv_fragment_class_t settings_pane_usbbridge_cls = {
        .constructor_cb = pane_ctor,
        .create_obj_cb = create_obj,
        .instance_size = sizeof(usbbridge_pane_t),
};

static void pane_ctor(lv_fragment_t *self, void *args) {
    (void) self;
    (void) args;
}

static lv_obj_t *create_obj(lv_fragment_t *self, lv_obj_t *container) {
    usbbridge_pane_t *pane = (usbbridge_pane_t *) self;
    lv_obj_t *view = pref_pane_container(container);
    lv_obj_set_layout(view, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(view, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(view, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    /* ⭐ What the section is, before any switch in it.
     *
     * ⓘ "as if it were plugged in there" is the plain-language version of
     * "natively" -- the same meaning without needing to be a programmer. And
     * "device" rather than "controller" on purpose: the bridge is not
     * controller-only, and naming controllers would describe our use of it
     * rather than the feature. */
    pref_title_label(view, locstr("USB Bridge"));
    pref_desc_label(view, locstr(
            "Connect a device on the TV to the PC as if it were plugged in there. "
            "Requires the CTM-USBIP relay running on the host."), false);

    /* ⭐⭐ THE ONLY WAYS TO BRIDGE ARE THE GESTURE AND THE PANEL, so switching
     * this off switches both and nothing can be handed over.
     *
     * ⛔ IT IS NOT THE OLD "use CTM Bridge" SWITCH, which was removed on
     * 2026-08-19. That one stopped Moonlight announcing any gamepad to the host
     * for the whole session, so an ordinary stream behaved differently and a
     * second controller had no route to the PC at all. This one only decides
     * whether a device can be handed over; a stream is unchanged either way. */
    lv_obj_t *enable_checkbox =
            pref_checkbox(view, locstr("Enable bridging"), &app_configuration->bridge_enable, false);
    pref_desc_label(view, locstr(
            "Allows devices to be handed to the PC during a stream. "
            "With this off, nothing is bridged and nothing about a stream changes."), false);
    /* ⛔ Worth saying once, about the set rather than any one switch. */
    pref_desc_label(view, locstr(
            "With every signal below switched off, a refused bridge looks the same as a "
            "gesture that was not recognised."), false);
    lv_obj_add_event_cb(enable_checkbox, enable_state_update_cb, LV_EVENT_VALUE_CHANGED, pane);

    /* ⭐ The gesture is the only way in that has a setting of its own. The USB
     * Bridge panel deliberately does NOT, and that is a decision worth keeping:
     * a panel switched off while gestures still work would look like the
     * feature had broken, with no way to find out why. ⓘ rhoquinn8217, 2026-08-20.
     *
     * ⓘ Defaults ON. */
    dependent_checkbox(pane, view, locstr("Bridge by gesture"),
                       &app_configuration->bridge_gesture);
    pref_desc_label(view, locstr(
            "Hold two fingers on the touchpad and press for one second to hand a controller "
            "over, or four seconds to take it back."), false);

    /* ⭐ NO DESCRIPTIONS ON THESE. The titles say what they are, and four
     * paragraphs of explanation made the section harder to read rather than
     * easier. ⓘ rhoquinn8217, 2026-08-20: only describe what cannot be guessed.
     *
     * ⓘ These cover a handover, a handback AND a refusal -- every signal this
     * side makes, not just the successful ones. */
    dependent_checkbox(pane, view, locstr("Lightbar signals"),
                       &app_configuration->bridge_signal_light);
    dependent_checkbox(pane, view, locstr("Rumble signals"),
                       &app_configuration->bridge_signal_rumble);
    dependent_checkbox(pane, view, locstr("Tone signals"),
                       &app_configuration->bridge_signal_tone);

    /* ⭐ The exception to the no-descriptions rule: nobody can guess what this
     * is for, and most people will not need it. */
    dependent_checkbox(pane, view, locstr("Microphone capture"),
                       &app_configuration->bridge_mic_capture);
    pref_desc_label(view, locstr(
            "Send the controller's built-in microphone to the PC while it is bridged. "
            "Only needed for voice chat through the controller itself."), false);

    usbbridge_apply_enabled(pane);
    return view;
}
