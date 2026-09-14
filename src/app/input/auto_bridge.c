#include "auto_bridge.h"

#if defined(TARGET_WEBOS)

#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "ctm_bridge_glue.h"
#include "ctm_bridge_gesture.h"
#include "bridge_request.h"
#include "app.h"

/* ⭐⭐ THE MATCHING LIVES HERE, NOT IN THE GLUE, AND THAT IS DELIBERATE.
 *
 * The mark keys on the controller's own MAC, and only SDL knows it -- it reads
 * feature report 0x09. ⛔ The glue is the seam to the bridge core and cannot
 * see SDL or the app's headers on purpose, so it must not learn to do this.
 * ➡️ So the pass runs up here, where both halves are visible, and drives the
 * glue through the SAME public calls the panel uses. Nothing new was added to
 * the core or the glue for it.
 *
 * ⛔ AND A DUALSENSE MUST NOT KEY ON ctm_bridge_dev_t.mac. That field is the HID
 * `uniq`, which is EMPTY for a directly cabled DualSense Edge and is the
 * DS5DONGLE'S OWN SERIAL through a dongle -- it follows the dongle, not the pad.
 * Keying on it would mark the dongle: move the controller and its mark stays
 * behind, and the next controller on that dongle bridges itself uninvited.
 * Measured on the C1 2026-09-08 after it fooled two readings.
 *
 * ⭐⭐ EVERY OTHER DEVICE KEYS ON ITS SERIAL (rhoquinn8217, 2026-09-13).
 * SDL has no MAC for them -- a cabled Xbox pad could not be marked at all --
 * and they have none to give over USB. The core's serial is their uniq, or the
 * USB serial number where no driver filled uniq; a blank or all-zeros one marks
 * nothing. ⓘ The host's config auto link keys the same way, for controllers. */

static void mac_trim(const char *in, char *out, size_t out_len)
{
    while (*in == ' ' || *in == '\t') {
        in++;
    }
    snprintf(out, out_len, "%s", in);
    size_t n = strlen(out);
    while (n > 0 && (out[n - 1] == ' ' || out[n - 1] == '\t')) {
        out[--n] = '\0';
    }
}

bool auto_bridge_list_has(const char *macs_csv, const char *mac)
{
    if (macs_csv == NULL || mac == NULL || mac[0] == '\0') {
        return false;
    }
    char list[512];
    snprintf(list, sizeof(list), "%s", macs_csv);
    char *save = NULL;
    for (char *tok = strtok_r(list, ",", &save); tok != NULL;
         tok = strtok_r(NULL, ",", &save)) {
        char one[64];
        mac_trim(tok, one, sizeof(one));
        /* ⓘ Compared without case or punctuation: marks stored before
         * 2026-09-13 are SDL's dashed MACs, and the kernel writes colons. */
        if (one[0] != '\0' && bridge_identity_same(one, mac)) {
            return true;
        }
    }
    return false;
}

void auto_bridge_list_set(const char *macs_csv, const char *mac, bool on,
                          char *out, size_t out_len)
{
    if (out == NULL || out_len == 0) {
        return;
    }
    out[0] = '\0';
    if (mac == NULL || mac[0] == '\0') {
        snprintf(out, out_len, "%s", macs_csv ? macs_csv : "");
        return;
    }

    /* ⭐ Rebuilt from the survivors rather than edited in place, so removing an
     * entry cannot leave a stray comma and the order of the rest is kept --
     * a rewritten file that reshuffles looks like something went wrong. */
    char list[512];
    snprintf(list, sizeof(list), "%s", macs_csv ? macs_csv : "");
    char *save = NULL;
    bool present = false;
    for (char *tok = strtok_r(list, ",", &save); tok != NULL;
         tok = strtok_r(NULL, ",", &save)) {
        char one[64];
        mac_trim(tok, one, sizeof(one));
        if (one[0] == '\0') {
            continue;
        }
        if (bridge_identity_same(one, mac)) {
            present = true;
            if (!on) {
                continue;   /* dropping this one */
            }
        }
        if (out[0] != '\0') {
            strncat(out, ",", out_len - strlen(out) - 1);
        }
        strncat(out, one, out_len - strlen(out) - 1);
    }
    if (on && !present) {
        if (out[0] != '\0') {
            strncat(out, ",", out_len - strlen(out) - 1);
        }
        strncat(out, mac, out_len - strlen(out) - 1);
    }
}

bool auto_bridge_identity(const ctm_bridge_dev_t *d, char *out, size_t out_len)
{
    if (d == NULL || out == NULL || out_len == 0) {
        return false;
    }
    out[0] = '\0';
    /* ⓘ NOT limited to controllers. Build 321 was, and it refused the GameSir's
     * keyboard half, which reports a serial. rhoquinn8217, 2026-09-13: "It
     * should be able to auto bridge if it is a DS5 device or a device that has
     * a serial that is not blank or all zeros." Configs are what keep to
     * controllers, and that is the listener's rule, not this one. */

    /* A DualSense or Edge: its own MAC. ⓘ The kind is "ds5", "ds5_usb", "ds5e"
     * or "ds5e_usb", so its first three letters are enough. */
    if (strncmp(d->kind, "ds5", 3) == 0) {
        if (d->node[0] != '\0' && ctm_bridge_gesture_mac_for_node(d->node, out, out_len) &&
            bridge_identity_usable(out)) {
            return true;
        }
        /* ⓘ SDL has no MAC for it -- it was not opened as a controller, as when
         * Aurora already holds four -- so the kernel's uniq, but ONLY if it is a
         * MAC. Through a dongle on the C1 it was the dongle's serial. */
        if (bridge_identity_mac_shaped(d->mac)) {
            snprintf(out, out_len, "%s", d->mac);
            return true;
        }
        out[0] = '\0';
        return false;
    }

    /* Every other device: its serial. */
    if (!bridge_identity_usable(d->serial)) {
        return false;
    }
    snprintf(out, out_len, "%s", d->serial);
    return true;
}

/* ⛔ NEVER WRITTEN TO THE SETTINGS FILE: this is the whole of an unsaved mark's
 * life, and closing the app is what ends it. The user is told so on the row. */
static char s_session_marks[512];

void auto_bridge_session_key(const ctm_bridge_dev_t *d, char *out, size_t out_len)
{
    if (out == NULL || out_len == 0) {
        return;
    }
    out[0] = '\0';
    if (d == NULL || d->node[0] == '\0') {
        return;   /* nothing to bridge it by, so nothing to mark it by */
    }
    snprintf(out, out_len, "%s:%s@%s", d->vid, d->pid, d->node);
}

bool auto_bridge_session_has(const char *key)
{
    return auto_bridge_list_has(s_session_marks, key);
}

void auto_bridge_session_set(const char *key, bool on)
{
    char out[sizeof s_session_marks];
    auto_bridge_list_set(s_session_marks, key, on, out, sizeof out);
    snprintf(s_session_marks, sizeof s_session_marks, "%s", out);
}

int auto_bridge_run(const char *macs_csv, bool all)
{
    if (!all && (macs_csv == NULL || macs_csv[0] == '\0') && s_session_marks[0] == '\0') {
        return 0;   /* nothing marked: the common case, and it costs nothing */
    }

    ctm_bridge_dev_t devs[16];
    /* ⓘ The full list, not the quiet one: a stream is starting, so the core is
     * up already and ctm_bridge_start has just enumerated. */
    const int n = ctm_bridge_list(devs, 16);
    int asked = 0;

    for (int i = 0; i < n; ++i) {
        if (devs[i].plugged || devs[i].node[0] == '\0') {
            continue;
        }
        /* ⭐ "Bridge all devices on startup" overrides the marks entirely: every
         * device, with a serial or without. Otherwise only a marked one. */
        if (!all) {
            char identity[64];
            if (auto_bridge_identity(&devs[i], identity, sizeof identity)) {
                if (!auto_bridge_list_has(macs_csv, identity)) {
                    continue;
                }
            } else {
                /* No identity to save: marked, if at all, for this run of the app. */
                char key[96];
                auto_bridge_session_key(&devs[i], key, sizeof key);
                if (!auto_bridge_session_has(key)) {
                    continue;
                }
            }
        }
        /* ⭐ THE SAME PATH THE PANEL USES, so a bridge that happens by itself
         * and one a user asked for cannot drift apart. ⓘ Since 2026-09-12 that
         * is literally one function, bridge_request_device(), rather than a
         * copy of the panel's decision kept here. */
        (void) bridge_request_device(&devs[i]);
        asked++;
    }
    return asked;
}

#endif /* TARGET_WEBOS */
