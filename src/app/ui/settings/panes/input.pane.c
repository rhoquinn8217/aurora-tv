#include "app.h"
#include "config.h"

#include "pref_obj.h"

#include "util/i18n.h"

#include <stdlib.h>

typedef struct input_pane_t {
    lv_fragment_t base;

    lv_obj_t *absmouse_toggle;
    lv_obj_t *absmouse_hint;
    lv_obj_t *deadzone_label;
    lv_obj_t *deadzone_slider;
    lv_obj_t *swap_abxy_toggle;
} input_pane_t;

static lv_obj_t *create_obj(lv_fragment_t *self, lv_obj_t *view);

/* ⛔⛔ EXPERIMENTAL BRANCH ONLY -- the microphone-capture confirmation.
 *
 * The checkbox itself is an ordinary pref_checkbox, which writes the setting
 * before this runs -- pref_obj registers its handler first, and LVGL calls them
 * in order. So by the time we are called the value is already true, and the
 * dialog's job is to put it back if the answer is no.
 *
 * ⭐ TICKING ASKS. UNTICKING NEVER DOES. Turning a hazard off should never have
 * a step in the way. */
static void bt_cap_confirm_cb(lv_event_t *e);
static void bt_cap_answer_cb(lv_event_t *e);
static lv_obj_t *bt_cap_checkbox = NULL;

static void pane_ctor(lv_fragment_t *self, void *args);

#if FEATURE_INPUT_EVMOUSE
static void hwmouse_state_update_cb(lv_event_t *e);

static void hwmouse_state_update(input_pane_t *pane);
#endif

static void update_deadzone_label(input_pane_t *pane);

static void on_deadzone_changed(lv_event_t *e);

const lv_fragment_class_t settings_pane_input_cls = {
        .constructor_cb = pane_ctor,
        .create_obj_cb = create_obj,
        .instance_size = sizeof(input_pane_t),
};

static void pane_ctor(lv_fragment_t *self, void *args) {
    (void) self;
    (void) args;
}

static lv_obj_t *create_obj(lv_fragment_t *self, lv_obj_t *container) {
    input_pane_t *pane = (input_pane_t *) self;
    lv_obj_t *view = pref_pane_container(container);
    lv_obj_set_layout(view, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(view, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(view, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    pref_checkbox(view, locstr("View-only mode"), &app_configuration->viewonly, false);
    pref_desc_label(view, locstr("Don't send mouse, keyboard or gamepad input to host computer."), false);

    pref_checkbox(view, locstr("Use CTM Bridge"), &app_configuration->ctm_bridge, false);
    pref_desc_label(view, locstr("Route all controller input through the CTM bridge instead of Moonlight (disables Moonlight input)."), false);

    /* ⛔ EXPERIMENTAL: microphone capture. See bt_cap_confirm_cb below. */
    bt_cap_checkbox = pref_checkbox(view, locstr("Bluetooth microphone capture (experimental)"),
                                    &app_configuration->bt_mic_capture, false);
    lv_obj_add_event_cb(bt_cap_checkbox, bt_cap_confirm_cb, LV_EVENT_CLICKED, NULL);
    pref_desc_label(view, locstr("Streams a Bluetooth controller's microphone to the host. Turns off this app's protection against stray controller audio. Applies to the next stream."), false);

    pref_checkbox(view, locstr("Capture system keys"), &app_configuration->syskey_capture, false);
    pref_desc_label(view, locstr("Capture and send system keys (e.g. Meta/Win key) to host computer."), false);

    pref_header(view, locstr("Mouse"));

#if FEATURE_INPUT_EVMOUSE
    lv_obj_t *hwmouse_toggle = pref_checkbox(view, locstr("Use mouse hardware"),
                                             &app_configuration->hardware_mouse, false);
    lv_obj_add_event_cb(hwmouse_toggle, hwmouse_state_update_cb, LV_EVENT_VALUE_CHANGED, pane);
    pref_desc_label(view, locstr("Use plugged mouse device only when streaming. "
                                 "This will have better performance, but absolute mouse mode will not be enabled."),
                    false);
#endif

    pane->absmouse_toggle = pref_checkbox(view, locstr("Absolute mouse mode"),
                                          &app_configuration->absmouse, false);
    pane->absmouse_hint = pref_desc_label(view, locstr("Better for remote desktop. "
                                                       "For some games, mouse will not work properly."), false);

    pref_header(view, locstr("Gamepad"));

    pane->deadzone_label = pref_title_label(view, locstr("Analog stick deadzone"));
    pane->deadzone_slider = pref_slider(view, &app_configuration->stick_deadzone, 0, 20, 1);
    lv_obj_set_width(pane->deadzone_slider, LV_PCT(100));
    lv_obj_add_event_cb(pane->deadzone_slider, on_deadzone_changed, LV_EVENT_VALUE_CHANGED, pane);
    pref_desc_label(view, locstr("Note: Some games can enforce a larger deadzone "
                                 "than what Aurora is configured to use."),
                    false);

    pref_checkbox(view, locstr("Virtual mouse"), &app_configuration->virtual_mouse, false);
    pref_desc_label(view, locstr("Hold Start for 4 seconds during streaming to toggle virtual mouse "
                                 "(enable here first). Right stick moves the cursor, left stick scrolls, "
                                 "LT/RT are left/right mouse buttons."),
                    false);

    pane->swap_abxy_toggle = pref_checkbox(view, locstr("Swap ABXY buttons"), &app_configuration->swap_abxy, false);
    pref_desc_label(view, locstr("Swap A/B and X/Y gamepad buttons. Useful when you prefer Nintendo-like layouts."),
                    false);

    pref_desc_label(view, locstr("Hold Select/Back for 4 seconds during streaming to pin or unpin performance stats. "
                                 "Open the on-screen keyboard from the stream overlay or Magic Remote BLUE."),
                    false);

#if FEATURE_INPUT_EVMOUSE
    hwmouse_state_update(pane);
#endif
    update_deadzone_label(pane);
    return view;
}

#if FEATURE_INPUT_EVMOUSE
static void hwmouse_state_update_cb(lv_event_t *e) {
    hwmouse_state_update((input_pane_t *) lv_event_get_user_data(e));
}

static void hwmouse_state_update(input_pane_t *pane) {
    if (app_configuration->hardware_mouse) {
        lv_obj_add_state(pane->absmouse_toggle, LV_STATE_DISABLED);
        lv_label_set_text(pane->absmouse_hint, locstr("Absolute mouse mode can't be used when "
                                                      "\"Use mouse hardware\" enabled."));
    } else {
        lv_obj_clear_state(pane->absmouse_toggle, LV_STATE_DISABLED);
        lv_label_set_text(pane->absmouse_hint, locstr("Better for remote desktop. "
                                                      "For some games, mouse will not work properly."));
    }
}
#endif

static void update_deadzone_label(input_pane_t *pane) {
    lv_label_set_text_fmt(pane->deadzone_label, "%s - %d", locstr("Analog stick deadzone"),
                          app_configuration->stick_deadzone);
}

static void on_deadzone_changed(lv_event_t *e) {
    input_pane_t *pane = (input_pane_t *) lv_event_get_user_data(e);
    update_deadzone_label(pane);
}

/* ⛔⛔ EXPERIMENTAL BRANCH ONLY.
 *
 * ⚠️ The wording is deliberate and short. It has to survive being read on a
 * television, from a sofa, by someone who has already decided to say yes:
 *
 *   - the mechanism, in one sentence, because "microphone" alone does not
 *     explain why it could break the TV;
 *   - who is protected and who is not, because that is the whole risk;
 *   - the known crash, because it is the likeliest way to meet the hazard;
 *   - and how to stop it, LAST, because that is what someone needs when they
 *     are already in trouble and no longer reading carefully.
 *
 * ⛔ The TV's remote is deliberately NOT offered as a second way to stop it.
 * It works, but two answers in a panic is worse than one that always does. */
static void bt_cap_confirm_cb(lv_event_t *e) {
    (void) e;
    /* Unticking: never ask. */
    if (!app_configuration->bt_mic_capture) {
        return;
    }
    static const char *btn_txts[] = {"Cancel", "Enable anyway", ""};
    lv_obj_t *msgbox = lv_msgbox_create(
            NULL,
            locstr("Enable microphone capture?"),
            locstr("The controller sends microphone audio in the same reports it uses for buttons. "
                   "A tag says which is which, and most software never checks it -- audio then "
                   "reads as thousands of button presses a second.\n\n"
                   "This app is patched and ignores them. webOS is not, and cannot be.\n\n"
                   "So while this app is in front, you are fine. If it closes, crashes, or you "
                   "switch away, the TV starts pressing its own buttons.\n\n"
                   "This also turns off the app's own shutdown-on-audio protection.\n\n"
                   "Known issue: close the bridge panel with the \"x\" -- Circle crashes it.\n\n"
                   "To stop it: power the controller off."),
            btn_txts, false);
    lv_obj_add_event_cb(msgbox, bt_cap_answer_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_center(msgbox);
}

static void bt_cap_answer_cb(lv_event_t *e) {
    lv_obj_t *msgbox = lv_event_get_current_target(e);
    /* Button 0 is Cancel. Anything else is the deliberate yes. */
    if (lv_msgbox_get_active_btn(msgbox) == 0) {
        app_configuration->bt_mic_capture = false;
        if (bt_cap_checkbox) {
            lv_obj_clear_state(bt_cap_checkbox, LV_STATE_CHECKED);
        }
    }
    /* ⛔ ASYNC, NOT lv_msgbox_close(). Closing a message box from inside its own
     * event handler frees the object LVGL is still dispatching on -- it crashes,
     * reliably. Every handler in this app that closes its own box uses the async
     * form; the plain one is only safe from outside. */
    lv_msgbox_close_async(msgbox);
}
