#pragma once

#include <stdbool.h>
#include <stdio.h>

/* ⭐ THE USB BRIDGE'S SETTINGS, saved in moonlight.ini beside the app's own.
 *
 * ⓘ They are one member of the app's settings, app_settings_t's `bridge`, so
 * every copy of the settings carries them, and upstream's settings code makes
 * three calls into this file: the defaults, the save and the read. The keys
 * in the file are the ones they have always had (bridge_enable and the rest,
 * in bridge_settings.c), so a TV keeps what it was set to.
 *
 * ⓘ Not ctm_bridge_settings_t, which is one controller's settings as the
 * listener hands them over. */
typedef struct bridge_settings_t {
    /* ⭐ CAN A DEVICE BE HANDED TO THE PC AT ALL? Defaults OFF (since build 307).
     *
     * ⓘ The gesture, the USB Bridge panel and Auto Bridge at stream start are
     * the only ways to ask for a bridge, so this switches all three and nothing
     * else. A stream behaves the same either way -- keyboards, mice and
     * controllers all reach the PC as usual.
     *
     * ⛔ NOT the old "use CTM Bridge" switch, removed 2026-08-19: that stopped
     * Moonlight announcing any gamepad for the whole session, so an ordinary
     * stream behaved differently and a second controller had no route at all. */
    bool enable;

    /* ⭐ The touchpad chord, on by default. Only meaningful while `enable` is
     * set, and the settings screen greys it out when that is off.
     *
     * ⓘ The USB Bridge PANEL has no switch of its own, deliberately: a panel
     * hidden while gestures still worked would look like the feature had
     * broken, with nothing to explain it. `enable` covers both. */
    bool gesture;

    /* ⭐ THE SIGNALS AND THE MICROPHONE. All on for now.
     *
     * ⓘ These are "I do not want that" switches, not a battery feature. A
     * bright light in a dark room, a buzz at midnight, a chirp while someone is
     * asleep, a microphone nobody asked for -- four reasons, one shape. ⚠️ The
     * battery saving is real for the microphone, small for rumble and the tone,
     * and negligible for the lightbar, so it is not what they are sold on.
     *
     * ⛔ TURN ALL THREE SIGNALS OFF AND A REFUSAL IS INVISIBLE: the chord does
     * nothing and there is no way to tell that from a gesture that was not
     * recognised. Said in the section description rather than per switch.
     *
     * ⚠️ DEFAULTS ARE NOT SETTLED. Everything is on while this is being worked
     * on so testing is not gated behind ticking boxes. ⓘ Microphone capture is
     * expected to end up OFF -- it is only for voice chat through the
     * controller itself, and most people will not want it. */
    bool signal_light;
    bool signal_rumble;
    bool signal_tone;
    /* ⭐⭐ WIRED AND BLUETOOTH ARE SEPARATE SETTINGS, deliberately.
     *
     * ⛔ They were one, called "microphone capture", and it silently meant
     * WIRED ONLY -- the core refuses Bluetooth outright and says so in the log:
     * `mic: capture not started -- not a wired connection`. ⚠️ A single
     * checkbox hid a distinction that matters enormously.
     *
     * ⛔⛔ WHY IT MATTERS: arming the microphone over Bluetooth triggers an
     * INPUT STORM through webOS's own hid-playstation driver -- the same
     * unfixed flag-check omission SDL has. The controller floods input and
     * becomes unusable. ⓘ We fixed it in an SDL fork, but that fork cannot go
     * upstream: it would ask GuiDev1994 to maintain a workaround for a fault in
     * the platform's driver. ➡️ So stable ships stock SDL and simply does not
     * arm it.
     *
     * ⚠️ mic_bt EXISTS ON THIS BRANCH but can never be set: the settings
     * screen greys it out. ⭐ It is here so the two branches differ ONLY by the
     * arming code itself -- see upstream-direction.md, 2026-08-21. ⛔ Do not
     * "tidy" it away; that reintroduces the divergence it exists to prevent. */
    bool mic_wired;
    bool mic_bt;
    /* ⭐ WHICH CONTROLLERS BRIDGE THEMSELVES when a stream starts, by the
     * controller's own MAC, comma-separated. Chosen in the USB Bridge settings
     * pane; empty means none.
     *
     * ⛔ MACs, NOT the core's `uniq`, and the difference is the whole feature.
     * Measured on the C1 2026-09-08: `uniq` is EMPTY for a directly cabled
     * DualSense Edge and is the DS5DONGLE'S OWN SERIAL through a dongle -- it
     * follows the dongle, so keying on it would mark the dongle rather than the
     * pad. Move the pad and its mark stays behind; put another pad on that
     * dongle and it bridges itself by mistake. ➡️ The MAC comes from SDL
     * (feature report 0x09) and is the pad's own on every path.
     *
     * ⛔ Empty means NONE. Never "whatever is present" -- that is the auto-plug
     * that was removed for taking devices away from the TV unasked. */
    char *auto_macs;
    /* ⭐ "Bridge all devices on startup" (rhoquinn8217, 2026-09-13): when a stream
     * starts, bridge EVERY device, serial or not, and ignore the marks above --
     * which are kept, so turning this off brings them back. ⛔ Off by default:
     * it is the user's explicit choice, never the unasked auto-plug. */
    bool auto_all;
    /* ⭐ Bridge Override: the TV's own mouse and touchpad handling off for the
     * stream. Off by default, and acts only with `enable` set. ⓘ It has
     * no button: bridge_override.h says how a person switches it.
     * ⛔ It never writes the Input settings it overrides; see
     * input/bridge_override.h. */
    bool override;
} bridge_settings_t;

/* The values a fresh install starts with. settings_initialize calls it after
 * clearing the whole of the app's settings. */
void bridge_settings_defaults(bridge_settings_t *settings);

/* Writes the settings into the open moonlight.ini, a key a line. */
void bridge_settings_write(FILE *fp, const bridge_settings_t *settings);

/* Reads one line of moonlight.ini. True when the key is one of these, so the
 * app's settings code looks no further. The section is not looked at: a key
 * is read wherever it sits, as it always has been. */
bool bridge_settings_parse(bridge_settings_t *settings, const char *name, const char *value);

/* Replace the auto-bridge list. ⓘ A setter because the field is an owned
 * string; the settings pane must not free and strdup it by hand. */
void bridge_settings_set_auto_macs(bridge_settings_t *settings, const char *csv);
