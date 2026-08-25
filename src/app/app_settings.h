/*
 * This file is part of Moonlight Embedded.
 *
 * Copyright (C) 2015-2017 Iwan Timmer
 *
 * Moonlight is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * Moonlight is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Moonlight; if not, see <http://www.gnu.org/licenses/>.
 */
#pragma once

#include <Limelight.h>

#include <stdbool.h>
#include "ss4s/video.h"

typedef struct window_state_t {
    int x, y, w, h;
} window_state_t;

typedef struct app_settings_t {
    STREAM_CONFIGURATION stream;
    int debug_level;
    char *decoder;
    char *audio_backend;
    char *audio_device;
    char *language;
    bool sops;
    bool localaudio;
    bool fullscreen;
    window_state_t window_state;
    int rotate;
    bool unsupported;
    bool quitappafter;
    bool autoresume;
    bool viewonly;
    /* ⭐ CAN A DEVICE BE HANDED TO THE PC AT ALL? Defaults ON.
     *
     * ⓘ The gesture and the USB Bridge panel are the only two ways to ask for a
     * bridge, so this switches both and nothing else. A stream behaves the same
     * either way -- keyboards, mice and controllers all reach the PC as usual.
     *
     * ⛔ NOT the old "use CTM Bridge" switch, removed 2026-08-19: that stopped
     * Moonlight announcing any gamepad for the whole session, so an ordinary
     * stream behaved differently and a second controller had no route at all. */
    bool bridge_enable;

    /* ⭐ The touchpad chord, on by default. Only meaningful while bridge_enable
     * is set, and the settings screen greys it out when that is off.
     *
     * ⓘ The USB Bridge PANEL has no switch of its own, deliberately: a panel
     * hidden while gestures still worked would look like the feature had
     * broken, with nothing to explain it. bridge_enable covers both. */
    bool bridge_gesture;

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
    bool bridge_signal_light;
    bool bridge_signal_rumble;
    bool bridge_signal_tone;
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
     * ⚠️ bridge_mic_bt EXISTS ON THIS BRANCH but can never be set: the settings
     * screen greys it out. ⭐ It is here so the two branches differ ONLY by the
     * arming code itself -- see upstream-direction.md, 2026-08-21. ⛔ Do not
     * "tidy" it away; that reintroduces the divergence it exists to prevent. */
    bool bridge_mic_wired;
    bool bridge_mic_bt;
    bool absmouse;
    bool hardware_mouse;
    bool virtual_mouse;
    bool swap_abxy;
    bool syskey_capture;
    bool hdr;   /* HDR10 (PQ) over HEVC Main10 or AV1 Main10 when host and decoder support it */
    /** Negotiate HEVC/AV1 Main10 without requiring HDR (SDR 10-bit; less banding). */
    bool force_10bit;
    bool force_full_color_range; /* SDR only: request full-range YUV (0-255) from host. No effect when HDR is on. */
    /** Report pad battery to host (Vibepollo/Sunshine virtual gamepads). Default on. */
    bool report_gamepad_battery;
    bool hevc;
    /** Sunshine/Apollo: negotiate AV1 Main8/Main10 when decoder exposes SS4S_VIDEO_AV1. */
    bool av1;
    /** Periodic HEVC IDR refresh interval in ms (0 = off, min 500 when enabled, step 500). */
    int idr_refresh_interval_ms;
    bool show_stats_on_start;
    bool show_stats_compact;
    /** On-screen log overlay preference (Yellow cycles Off/Live/Frozen). */
    bool show_logs;
    int stick_deadzone;
    /**
     * Sent to host as STREAM_CONFIGURATION.clientRefreshRateX100 (Hz * 100, e.g. 11994 = 119.94 Hz).
     * 0 = omit (host default frame pacing).
     */
    int client_refresh_rate_x100;
    /**
     * When true on webOS, map preset 30/60/120/240 fps to NTSC fractional rates
     * (e.g. 120 → 11988). When false, presets use integer fps (client_refresh_rate_x100 = 0).
     */
    bool use_ntsc_refresh;
    bool auto_adjust_bitrate;
    int abr_mode;
    /**
     * webOS rooted only: switch picture/sound to Game for the stream (not HDMI ALLM).
     * Default on; UI row only shown when Homebrew Channel elevated service is present.
     */
    bool game_mode;
    char *conf_dir;
    char *ini_path;
    char *condb_path;
    char *key_dir;
    bool conf_persistent;
} CONFIGURATION, *PCONFIGURATION, app_settings_t;

typedef struct audio_config_entry_t {
    int configuration;
    const char *value;
    const char *name;
} audio_config_entry_t;

extern const audio_config_entry_t audio_configs[];
extern const size_t audio_config_len;

#define CONF_NAME_MOONLIGHT "moonlight.ini"
#define CONF_NAME_HOSTS "hosts.ini"

#define RES_MERGE(w, h) (((w) & 0xFFFF) << 16 | ((h) & 0xFFFF))

#define RES_720P RES_MERGE(1280, 720)
#define RES_1080P RES_MERGE(1920, 1080)
#define RES_1440P RES_MERGE(2560, 1440)
#define RES_1800P RES_MERGE(3200, 1800)
/** ~90% of 4K (3584×2016); practical limit on LG C5 without cumulative 4K delay */
#define RES_3_6K RES_MERGE(3584, 2016)
#define RES_4K RES_MERGE(3840, 2160)

/** Fixed decode-unit reassembly buffer (megabytes). */
#define VDEC_REASSEMBLY_BUFFER_MB 2

void settings_initialize(app_settings_t *config, char *conf_dir);

bool settings_read(app_settings_t *config);

bool settings_save(app_settings_t *config);

/** Keep stream.fps aligned with client_refresh_rate_x100 when a fractional rate is set. */
void settings_sync_refresh_rate(app_settings_t *config);

/** webOS: apply NTSC x100 only when use_ntsc_refresh for 30/60/120/240; else clear for presets. */
void settings_reconcile_refresh_rate(app_settings_t *config);

/** NTSC refresh rate (Hz × 100) for a nominal FPS preset, or 0 if not mapped (e.g. 144). */
int settings_ntsc_refresh_rate_x100_for_fps(int nominal_fps);

/** On webOS, set or clear client_refresh_rate_x100 for a preset FPS based on use_ntsc_refresh. */
void settings_apply_ntsc_preset_refresh(app_settings_t *config, int nominal_fps);

void settings_clear(app_settings_t *config);

int settings_optimal_bitrate(const SS4S_VideoCapabilities *capabilities, int w, int h, int fps);

bool audio_config_valid(int config);