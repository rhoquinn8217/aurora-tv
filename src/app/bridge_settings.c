#include "bridge_settings.h"

#include <stdlib.h>
#include <string.h>

#include "util/ini_ext.h"

#include "ini_writer.h"

static void set_string(char **field, const char *value);

static void append_csv(char **field, const char *value);
static void write_list(FILE *fp, const char *key, const char *csv);

void bridge_settings_defaults(bridge_settings_t *settings) {
    /* ⭐⭐ ON FOR A FRESH INSTALL, SINCE 2026-10-15 (rhoquinn8217, making a
     * DualSense's setup as seamless as possible). It was off from 2026-09-08:
     * bridging needs a listener on the PC, and shipping it on looked broken to
     * anyone without one. ⓘ What changed: Vibepollo starts DS5-USBIP with the
     * stream, the badge says whether it answers, and nothing bridges by itself
     * until it does (input/auto_bridge.h).
     * ✅ Existing installs are untouched: settings_load applies these defaults
     * and THEN lets the file override them, and the conf directory survives an
     * ipk install -- verified on the C1, whose pairing keys predate every
     * install since. */
    settings->enable = true;
    settings->gesture = true;
    settings->signal_light = true;
    settings->signal_rumble = true;
    settings->signal_tone = true;
    settings->mic_wired = true;
    settings->mic_bt = false;   /* never armed on this branch */
    set_string(&settings->auto_macs, "");
    set_string(&settings->auto_skip, "");
    settings->auto_all = false;
    settings->override = false;
    settings->tv_cursor_x = -1;
    settings->tv_cursor_y = 3;
    settings->tv_cursor_regular = false;
    settings->tv_cursor_regular_x = 5;
    settings->tv_cursor_regular_y = -1;
}

void bridge_settings_write(FILE *fp, const bridge_settings_t *settings) {
    ini_write_bool(fp, "bridge_enable", settings->enable);
    ini_write_bool(fp, "bridge_gesture", settings->gesture);
    ini_write_bool(fp, "bridge_signal_light", settings->signal_light);
    ini_write_bool(fp, "bridge_signal_rumble", settings->signal_rumble);
    ini_write_bool(fp, "bridge_signal_tone", settings->signal_tone);
    ini_write_bool(fp, "bridge_mic_wired", settings->mic_wired);
    ini_write_bool(fp, "bridge_mic_bt", settings->mic_bt);
    /* ⭐ ONE LINE PER MARK, not the whole list on one. A mark carries the device's
     * name since 2026-09-14, and the INI reader cuts a line at 200 characters
     * (inih's INI_MAX_LINE), which three or four marks on one line would pass --
     * losing the rest without a word. bridge_auto_macs, the old one-line list, is
     * still read. */
    write_list(fp, "bridge_auto_mark", settings->auto_macs);
    write_list(fp, "bridge_auto_skip", settings->auto_skip);
    ini_write_bool(fp, "bridge_auto_all", settings->auto_all);
    ini_write_bool(fp, "bridge_override", settings->override);
    ini_write_int(fp, "tv_cursor_shift_x", settings->tv_cursor_x);
    ini_write_int(fp, "tv_cursor_shift_y", settings->tv_cursor_y);
    ini_write_string(fp, "tv_cursor_look", settings->tv_cursor_regular ? "regular" : "sharp");
    ini_write_int(fp, "tv_cursor_regular_shift_x", settings->tv_cursor_regular_x);
    ini_write_int(fp, "tv_cursor_regular_shift_y", settings->tv_cursor_regular_y);
}

bool bridge_settings_parse(bridge_settings_t *settings, const char *name, const char *value) {
    if (INI_NAME_MATCH("bridge_enable")) {
        settings->enable = INI_IS_TRUE(value);
    } else if (INI_NAME_MATCH("bridge_gesture")) {
        settings->gesture = INI_IS_TRUE(value);
    } else if (INI_NAME_MATCH("bridge_signal_light")) {
        settings->signal_light = INI_IS_TRUE(value);
    } else if (INI_NAME_MATCH("bridge_signal_rumble")) {
        settings->signal_rumble = INI_IS_TRUE(value);
    } else if (INI_NAME_MATCH("bridge_signal_tone")) {
        settings->signal_tone = INI_IS_TRUE(value);
    } else if (INI_NAME_MATCH("bridge_mic_wired")) {
        settings->mic_wired = INI_IS_TRUE(value);
    } else if (INI_NAME_MATCH("bridge_mic_bt")) {
        settings->mic_bt = INI_IS_TRUE(value);
    } else if (INI_NAME_MATCH("bridge_auto_mark") || INI_NAME_MATCH("bridge_auto_macs")) {
        /* ⓘ bridge_auto_mark is one mark a line; bridge_auto_macs is the old
         * comma-separated list, read so marks saved before 2026-09-14 survive. */
        append_csv(&settings->auto_macs, value);
    } else if (INI_NAME_MATCH("bridge_auto_skip")) {
        append_csv(&settings->auto_skip, value);
    } else if (INI_NAME_MATCH("bridge_auto_all")) {
        settings->auto_all = INI_IS_TRUE(value);
    } else if (INI_NAME_MATCH("bridge_override")) {
        settings->override = INI_IS_TRUE(value);
    } else if (INI_NAME_MATCH("tv_cursor_shift_x")) {
        settings->tv_cursor_x = atoi(value);
    } else if (INI_NAME_MATCH("tv_cursor_shift_y")) {
        settings->tv_cursor_y = atoi(value);
    } else if (INI_NAME_MATCH("tv_cursor_look")) {
        settings->tv_cursor_regular = strcmp(value, "regular") == 0;
    } else if (INI_NAME_MATCH("tv_cursor_regular_shift_x")) {
        settings->tv_cursor_regular_x = atoi(value);
    } else if (INI_NAME_MATCH("tv_cursor_regular_shift_y")) {
        settings->tv_cursor_regular_y = atoi(value);
    } else {
        return false;
    }
    return true;
}

void bridge_settings_set_auto_macs(bridge_settings_t *settings, const char *csv) {
    if (settings == NULL) {
        return;
    }
    set_string(&settings->auto_macs, csv ? csv : "");
}

void bridge_settings_set_auto_skip(bridge_settings_t *settings, const char *csv) {
    if (settings == NULL) {
        return;
    }
    set_string(&settings->auto_skip, csv ? csv : "");
}

/* A comma-separated list, one entry a line under `key`. */
static void write_list(FILE *fp, const char *key, const char *csv) {
    if (csv == NULL) {
        return;
    }
    char list[4096];
    snprintf(list, sizeof list, "%s", csv);
    char *save = NULL;
    for (char *tok = strtok_r(list, ",", &save); tok != NULL; tok = strtok_r(NULL, ",", &save)) {
        while (*tok == ' ') {
            tok++;
        }
        if (*tok != '\0') {
            ini_write_string(fp, key, tok);
        }
    }
}

static void set_string(char **field, const char *value) {
    free(*field);
    *field = value != NULL ? strdup(value) : NULL;
}

/* Adds value to a comma-separated list, as a new entry. */
static void append_csv(char **field, const char *value) {
    if (value == NULL || value[0] == '\0') {
        return;
    }
    if (*field == NULL || (*field)[0] == '\0') {
        set_string(field, value);
        return;
    }
    const size_t len = strlen(*field) + 1 + strlen(value) + 1;
    char *joined = malloc(len);
    if (joined == NULL) {
        return;
    }
    snprintf(joined, len, "%s,%s", *field, value);
    free(*field);
    *field = joined;
}
