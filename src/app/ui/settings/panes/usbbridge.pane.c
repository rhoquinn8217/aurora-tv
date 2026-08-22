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

/* ⭐ 0 on this branch: the Bluetooth microphone arming code does not exist here.
 * The experimental branch defines it 1. ⓘ See upstream-direction.md,
 * 2026-08-21 -- the branches are meant to differ by the arming and nothing
 * else, so anything that has to change with it is gated on this rather than
 * kept in step by hand. */
#ifndef CTM_BT_MIC_ARMING
#define CTM_BT_MIC_ARMING 0
#endif

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
/* ⓘ The space that separates one setting-plus-description from the next.
 * ⚠️ An object rather than padding -- see the note in create_obj. */
static void usbb_gap(lv_obj_t *view)
{
    lv_obj_t *sp = lv_obj_create(view);
    lv_obj_remove_style_all(sp);
    lv_obj_set_size(sp, LV_PCT(100), LV_DPX(14));
    lv_obj_clear_flag(sp, LV_OBJ_FLAG_SCROLLABLE);
}

static lv_obj_t *dependent_checkbox_ex(usbbridge_pane_t *pane, lv_obj_t *view,
                                       const char *title, bool *value, bool reverse)
{
    lv_obj_t *cb = pref_checkbox(view, title, value, reverse);
    if (pane->dependent_count < (int) (sizeof(pane->dependent) / sizeof(pane->dependent[0]))) {
        pane->dependent[pane->dependent_count++] = cb;
    }
    return cb;
}

static lv_obj_t *dependent_checkbox(usbbridge_pane_t *pane, lv_obj_t *view,
                                    const char *title, bool *value)
{
    return dependent_checkbox_ex(pane, view, title, value, false);
}

/* ⭐ A "Disable ..." line: ticked means the stored setting is FALSE.
 *
 * ⓘ pref_checkbox already understands this -- its `reverse` argument exists for
 * it -- so the setting itself stays positive everywhere in the code. ⛔ Storing
 * an inverted value would mean every check that reads it has to remember, and
 * one that forgot would fail silently. */
static lv_obj_t *dependent_checkbox_inverted(usbbridge_pane_t *pane, lv_obj_t *view,
                                             const char *title, bool *value)
{
    return dependent_checkbox_ex(pane, view, title, value, true);
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
    /* ⭐⭐ A SPACER BEFORE EACH SETTING, and descriptions back in their own
     * labels.
     *
     * ⛔ THREE WAYS WERE TRIED. A plain gap left every description floating
     * between two settings, nearer the one below than the one it described.
     * pad_top on the checkbox did not move it -- pad is INSIDE the object, so
     * the box just grew taller. Folding the text into the checkbox LABEL
     * grouped it correctly but broke on hardware: a checkbox label does not
     * wrap and the box has a fixed height, so long lines ran off the edge and a
     * fourth line was clipped away entirely.
     *
     * ⭐ A spacer object is the one that works: the pane's own gap is zero, so a
     * description sits tight under its setting, and an empty box of fixed
     * height opens the space before the NEXT one. ⓘ Descriptions keep their own
     * smaller, dimmer styling, and they wrap. */
    lv_obj_set_style_pad_gap(view, 0, 0);

    /* ⭐ What the section is, before any switch in it.
     *
     * ⓘ "as if it were plugged in there" is the plain-language version of
     * "natively" -- the same meaning without needing to be a programmer. And
     * "device" rather than "controller" on purpose: the bridge is not
     * controller-only, and naming controllers would describe our use of it
     * rather than the feature. */
    /* ⭐⭐ THE WORD IS TAUGHT HERE OR NOWHERE. "Bridging" appears in every switch
     * below, so if this paragraph does not define it the rest of the screen is
     * jargon.
     *
     * ⚠️ AND "BRIDGE" IS OVERLOADED -- a network bridge, bridge mode on a
     * router, an audio bridge. A reader who knows any of those arrives with the
     * wrong idea, which is worse than arriving with none. ⓘ rhoquinn8217, 2026-08-20.
     *
     * ⭐ The word leads, so the reader knows the paragraph is teaching a term
     * rather than describing a feature.
     *
     * ⓘ "Natively" is deliberate and was argued over. It looks technical, but
     * anyone who has set up Sunshine has met the phrase "emulated controller",
     * and native is exactly the word that says this is the other thing -- the
     * one they came here hoping to read. */
    /* ⛔ NOT "USB Bridge" -- the pane already carries that name at the top, and
     * two identical titles read as a nesting error. ⓘ rhoquinn8217, 2026-08-20.
     * ⭐ "Overview" rather than "Directions": this is a definition and a
     * requirement, not a set of steps. */
    pref_title_label(view, locstr("Overview"));
    pref_desc_label(view, locstr(
            "Bridging allows your PC to natively interface with USB or Bluetooth devices "
            "connected to your TV. Bridged devices appear as if they are directly connected "
            "to the host PC."), false);
    pref_desc_label(view, locstr(
            "Requires the CTM-USBIP relay running on your host PC."), false);

    /* ⭐⭐ THE ONLY WAYS TO BRIDGE ARE THE PANEL AND THE GESTURE, so switching
     * this off switches both and nothing can be handed over.
     *
     * ⛔ IT IS NOT THE OLD "use CTM Bridge" SWITCH, removed 2026-08-19. That one
     * stopped Moonlight announcing any gamepad for the whole session, so an
     * ordinary stream behaved differently and a second controller had no route
     * to the PC at all. This only decides whether a device can be handed over;
     * a stream is unchanged either way. */
    usbb_gap(view);
    lv_obj_t *enable_checkbox =
            pref_checkbox(view, locstr("Enable Device Bridging"),
                          &app_configuration->bridge_enable, false);
    pref_desc_label(view, locstr(
            "Allows device bridging with the USB Bridge Overlay Panel or gestures "
            "(DualSense/DualSense Edge Only)."), false);
    lv_obj_add_event_cb(enable_checkbox, enable_state_update_cb, LV_EVENT_VALUE_CHANGED, pane);

    /* ⭐ A heading, so the four below do not each need to say "DualSense only".
     * ⓘ They are controller features -- a keyboard has no touchpad, no lightbar
     * and no speaker. */
    pref_title_label(view, locstr("DualSense/DualSense Edge Options"));

    /* ⭐⭐ CAPABILITIES FIRST, OPT-OUTS LAST, and the wording follows the same
     * split.
     *
     * ⭐ A CAPABILITY is something you learn: it says "Enable" and carries
     * instructions. ⭐ A SIGNAL already happens and needs no teaching, so it
     * says "Disable" and carries nothing.
     *
     * ⛔ EVERYTHING WAS "Enable" AT FIRST, for a consistent tick. rhoquinn8217 pushed
     * back and was right: the signals are advertised features, built over two
     * days, and "Enable lightbar signals" reads as an option nobody had thought
     * of rather than something already working. ➡️ "Disable" puts the burden on
     * the user to opt OUT, which is what these are.
     *
     * ⚠️ The gesture cannot follow: its description is INSTRUCTIONS -- the chord
     * and its timings, documented nowhere else in the interface -- and
     * instructions under "Disable gesture bridging" read as a manual for the
     * thing you are switching off.
     *
     * ⓘ So the two lines that say "Enable" are exactly the two that carry a
     * description, and the inconsistency has a visible reason rather than
     * looking accidental. */
    /* ⭐ THE HOLD, NOT THE TOTAL. The hold is what the user does; the total
     * includes the bridge itself, which they do not control and which varies by
     * transport. ⓘ End to end it is about 3 s and 5 s.
     *
     * ⚠️ THESE NUMBERS ARE THE CONSTANTS. `GESTURE_PLUG_HOLD_MS` is 1000 ms in
     * ctm_bridge_gesture.c; `DS5_CHORD_HOLD_MS` is 4000 ms in the core's
     * ctm_gesture_chord.inl. ➡️ If either changes, change this line -- it has
     * already gone stale once. */
    usbb_gap(view);
    dependent_checkbox(pane, view, locstr("Enable Gesture Bridging"),
                       &app_configuration->bridge_gesture);
    pref_desc_label(view, locstr("Allows the following:"), false);
    pref_desc_label(view, locstr("2-finger touchpad hold: 1 sec. -- Bridge"), false);
    pref_desc_label(view, locstr("2-finger touchpad hold: 4 sec. -- Release"), false);

    /* ⭐⭐ THE WARNING IS THE WHOLE DESCRIPTION. What the switch DOES is already
     * obvious from the section above -- bridging connects the device, so its
     * microphone comes with it -- and repeating that wastes the only line the
     * reader will actually read.
     *
     * ⭐ THE BATTERY CLAIM IS DOCUMENTED, not guessed. The DualSense microphone
     * is armed whenever the controller is on and draws power whether or not
     * anyone is speaking; Sony's own guidance and every battery-life guide say
     * to mute it when it is not needed. ⚠️ An earlier draft hedged this to "uses
     * battery while enabled" on the grounds that we had not measured it -- that
     * was the wrong rule applied. ⓘ Measure before claiming is for OUR
     * behaviour; this is a property of the hardware. */
    usbb_gap(view);
    dependent_checkbox(pane, view, locstr("Enable Wired Microphone"),
                       &app_configuration->bridge_mic_wired);
    pref_desc_label(view, locstr(
            "Warning: While the DualSense microphone is on, the controller drains its battery "
            "even when it is not in use."), false);

    /* ⭐⭐ PRESENT, AND PERMANENTLY OFF ON THIS BRANCH.
     *
     * ⛔ Showing it rather than hiding it is deliberate. ⓘ rhoquinn8217, 2026-08-21:
     * hiding it "begs the question we can just answer". ⭐ Somebody wondering why
     * their Bluetooth controller's microphone does nothing finds the answer
     * here, instead of concluding the feature is broken.
     *
     * ⭐ The wording says NOT READY rather than offering an excuse. It is true:
     * arming over Bluetooth makes the controller flood input through the TV's
     * own driver, and that has no fix from our side that could be shipped -- the
     * one we have lives in an SDL fork that cannot go upstream.
     *
     * ⚠️ IT IS ALSO WHAT KEEPS THE TWO BRANCHES ONE LINE APART. The experimental
     * branch shows the same control, selectable. ⛔ Deleting it here would put a
     * whole feature back between them. */
    usbb_gap(view);
    /* ⭐ "(Unavailable)" IN THE LABEL, not just a grey box. ⛔ Greying alone is
     * easy to miss on a television across a room, and it says nothing about
     * why. ⓘ "Unavailable" rather than "Disabled": nobody switched it off --
     * this build cannot offer it. ⭐ It also matches the first words of the
     * description below.
     *
     * ⚠️ Gated like everything else that changes with the arming, so the file
     * stays identical on both branches. */
    lv_obj_t *bt_mic = pref_checkbox(view,
#if CTM_BT_MIC_ARMING
                                     locstr("Enable BT Microphone"),
#else
                                     locstr("Enable BT Microphone (Unavailable)"),
#endif
                                     &app_configuration->bridge_mic_bt, false);
#if CTM_BT_MIC_ARMING
    /* ⓘ Selectable only where the arming code exists. */
    (void) bt_mic;
#else
    lv_obj_add_state(bt_mic, LV_STATE_DISABLED);
#endif
    pref_desc_label(view, locstr(
            "A bug in webOS's input driver causes Bluetooth microphone audio to be read as "
            "controller inputs, resulting in a flood of random inputs that can reach the host "
            "during a stream. Because webOS's input driver is system-locked, a permanent fix "
            "can only be done with root level access."), false);
#if CTM_BT_MIC_ARMING
    /* ⭐⭐ THE EXPERIMENTAL BRANCH ONLY, and the distinction matters: this build
     * ships STOCK SDL. Saying a workaround "has been applied" here would tell a
     * reader they are protected when they are not.
     *
     * ⓘ Behind the same condition that makes the checkbox selectable, so the
     * FILE is identical on both branches and only the define differs -- which is
     * the whole point of keeping them one line apart. ⛔ Two strings that had to
     * be kept in step by memory is how the microphone setting came to be built
     * twice on 2026-08-20. */
    pref_desc_label(view, locstr(
            "An app-level fix has been applied that provides a workaround, but only in Aurora. "
            "Leaving the app while the controller's microphone is enabled will result in a "
            "flood of random inputs."), false);
#endif

    /* ⓘ Last, and together: three opt-outs in a row read consistently among
     * themselves even though the two above them read the other way. ⓘ rhoquinn8217,
     * 2026-08-20: gestures and the microphone are user actions; signals are
     * acknowledgements, and less important. */
    usbb_gap(view);
    dependent_checkbox_inverted(pane, view, locstr("Disable Lightbar Bridge/Release Signals"),
                                &app_configuration->bridge_signal_light);
    dependent_checkbox_inverted(pane, view, locstr("Disable Rumble Bridge/Release Signals"),
                                &app_configuration->bridge_signal_rumble);
    dependent_checkbox_inverted(pane, view, locstr("Disable Audio Tone Bridge/Release Signals"),
                                &app_configuration->bridge_signal_tone);
    pref_desc_label(view, locstr(
            "Warning: With all three signals disabled, check the USB Bridge Overlay Panel or "
            "the Windows controller panel to confirm your device has bridged."), false);
    /* ⭐ A way to check, not a telling-off. ⛔ The first version said a refused
     * bridge would look like an unrecognised gesture -- true, but "refusal" is
     * jargon this screen never defines, and it left the reader with a problem
     * and no answer. ⓘ rhoquinn8217, 2026-08-20: "give them another way to check is
     * better than scolding them." */


    usbbridge_apply_enabled(pane);
    return view;
}
