#include "bridge_settings.h"

#include <stdlib.h>
#include <string.h>

#include "util/ini_ext.h"

#include "ini_writer.h"

static void set_string(char **field, const char *value);

static void append_csv(char **field, const char *value);

void bridge_settings_defaults(bridge_settings_t *settings) {
    /* ⭐⭐ OFF ON A FRESH INSTALL (rhoquinn8217, 2026-09-08). Bridging needs a
     * listener running on the PC, so it cannot work until someone has set that
     * up; shipping it on means a feature that appears broken to everyone who
     * has not. ➡️ Turning it on is the moment the Auto Bridge section is first
     * met, which is why that section sits directly under this switch.
     * ✅ Existing installs are untouched: settings_load applies these defaults
     * and THEN lets the file override them, and the conf directory survives an
     * ipk install -- verified on the C1, whose pairing keys predate every
     * install since. */
    settings->enable = false;
    settings->gesture = true;
    settings->signal_light = true;
    settings->signal_rumble = true;
    settings->signal_tone = true;
    settings->mic_wired = true;
    settings->mic_bt = false;   /* never armed on this branch */
    set_string(&settings->auto_macs, "");
    settings->auto_all = false;
    settings->override = false;
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
    if (settings->auto_macs != NULL) {
        char list[4096];
        snprintf(list, sizeof list, "%s", settings->auto_macs);
        char *save = NULL;
        for (char *tok = strtok_r(list, ",", &save); tok != NULL; tok = strtok_r(NULL, ",", &save)) {
            while (*tok == ' ') {
                tok++;
            }
            if (*tok != '\0') {
                ini_write_string(fp, "bridge_auto_mark", tok);
            }
        }
    }
    ini_write_bool(fp, "bridge_auto_all", settings->auto_all);
    ini_write_bool(fp, "bridge_override", settings->override);
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
    } else if (INI_NAME_MATCH("bridge_auto_all")) {
        settings->auto_all = INI_IS_TRUE(value);
    } else if (INI_NAME_MATCH("bridge_override")) {
        settings->override = INI_IS_TRUE(value);
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
