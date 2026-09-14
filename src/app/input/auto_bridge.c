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
 * A DualSense's identity is its own MAC, and only SDL knows it -- it reads
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
 * ⭐⭐ A MARK IS THE IDENTITY AND THE NAME TOGETHER, FOR EVERY DEVICE
 * (rhoquinn8217, 2026-09-14). Auto bridge is not the listener's config auto
 * link, where a DualSense's MAC has to follow the pad across Bluetooth, a cable
 * and a dongle; here a device only has to be told from the others on the TV.
 * - The identity is a DualSense's MAC or any other device's serial, and may be
 *   blank: a device without one is remembered by its name alone. ⓘ Two such
 *   devices with one name share a mark, which was judged unlikely to matter.
 * - The name keeps two halves of one device apart: a GameSir's pad and its
 *   keyboard interface share a serial but not a name.
 * Stored as "identity|name". ⓘ A mark saved before 2026-09-14 has no '|' and
 * matches on its identity alone, as it always did. */

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

/* The longest name a mark stores. ⛔ Keeps a whole mark line in the settings
 * file under the INI reader's 200 characters: "bridge_auto_mark = ", an identity
 * of up to 63 and the '|' leave room for this and no more. */
#define MARK_NAME_MAX 96

/* The name a mark stores: the device's own, with the list's two separators
 * blanked so a name cannot split an entry, and cut to MARK_NAME_MAX -- the same
 * cut on both sides of a comparison, so a long name still matches itself. */
static void mark_name(const ctm_bridge_dev_t *d, char *out, size_t out_len)
{
    char raw[sizeof d->name];
    snprintf(raw, sizeof raw, "%s", d->name);
    for (char *p = raw; *p != '\0'; ++p) {
        if (*p == ',' || *p == '|') {
            *p = ' ';
        }
    }
    if (strlen(raw) > MARK_NAME_MAX) {
        raw[MARK_NAME_MAX] = '\0';
    }
    mac_trim(raw, out, out_len);
}

/* Does one stored entry name the device with this identity and name? */
static bool entry_matches(const char *entry, const char *identity, const char *name)
{
    const char *bar = strchr(entry, '|');
    if (bar == NULL) {
        /* Saved before names were kept: the identity alone. */
        return identity[0] != '\0' && bridge_identity_same(entry, identity);
    }
    char id_raw[64];
    size_t id_len = (size_t) (bar - entry);
    if (id_len >= sizeof id_raw) {
        id_len = sizeof id_raw - 1;
    }
    memcpy(id_raw, entry, id_len);
    id_raw[id_len] = '\0';
    char id[64];
    mac_trim(id_raw, id, sizeof id);
    char nm[128];
    mac_trim(bar + 1, nm, sizeof nm);

    /* ⓘ Two blank identities ARE the same here, unlike in the core's rule: the
     * name is what carries the match for a device without one. */
    const bool same_id = (id[0] == '\0' || identity[0] == '\0')
                             ? (id[0] == '\0' && identity[0] == '\0')
                             : bridge_identity_same(id, identity);
    return same_id && strcasecmp(nm, name) == 0;
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

void auto_bridge_mark_key(const ctm_bridge_dev_t *d, char *out, size_t out_len)
{
    if (out == NULL || out_len == 0) {
        return;
    }
    out[0] = '\0';
    if (d == NULL) {
        return;
    }
    char identity[64];
    if (!auto_bridge_identity(d, identity, sizeof identity)) {
        identity[0] = '\0';
    }
    char name[128];
    mark_name(d, name, sizeof name);
    snprintf(out, out_len, "%s|%s", identity, name);
}

bool auto_bridge_marked(const char *macs_csv, const ctm_bridge_dev_t *d)
{
    if (macs_csv == NULL || macs_csv[0] == '\0' || d == NULL) {
        return false;
    }
    char identity[64];
    if (!auto_bridge_identity(d, identity, sizeof identity)) {
        identity[0] = '\0';
    }
    char name[128];
    mark_name(d, name, sizeof name);
    if (identity[0] == '\0' && name[0] == '\0') {
        return false;   /* nothing to know it by */
    }

    char list[2048];
    snprintf(list, sizeof(list), "%s", macs_csv);
    char *save = NULL;
    for (char *tok = strtok_r(list, ",", &save); tok != NULL;
         tok = strtok_r(NULL, ",", &save)) {
        char one[256];
        mac_trim(tok, one, sizeof(one));
        if (one[0] != '\0' && entry_matches(one, identity, name)) {
            return true;
        }
    }
    return false;
}

void auto_bridge_mark_set(const char *macs_csv, const ctm_bridge_dev_t *d, bool on,
                          char *out, size_t out_len)
{
    if (out == NULL || out_len == 0) {
        return;
    }
    out[0] = '\0';
    if (d == NULL) {
        snprintf(out, out_len, "%s", macs_csv ? macs_csv : "");
        return;
    }
    char identity[64];
    if (!auto_bridge_identity(d, identity, sizeof identity)) {
        identity[0] = '\0';
    }
    char name[128];
    mark_name(d, name, sizeof name);

    /* ⭐ Rebuilt from the survivors rather than edited in place, so removing an
     * entry cannot leave a stray comma and the order of the rest is kept --
     * a rewritten file that reshuffles looks like something went wrong. */
    char list[2048];
    snprintf(list, sizeof(list), "%s", macs_csv ? macs_csv : "");
    char *save = NULL;
    bool present = false;
    for (char *tok = strtok_r(list, ",", &save); tok != NULL;
         tok = strtok_r(NULL, ",", &save)) {
        char one[256];
        mac_trim(tok, one, sizeof(one));
        if (one[0] == '\0') {
            continue;
        }
        if (entry_matches(one, identity, name)) {
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
    if (on && !present && (identity[0] != '\0' || name[0] != '\0')) {
        char key[256];
        auto_bridge_mark_key(d, key, sizeof key);
        if (out[0] != '\0') {
            strncat(out, ",", out_len - strlen(out) - 1);
        }
        strncat(out, key, out_len - strlen(out) - 1);
    }
}

int auto_bridge_run(const char *macs_csv, bool all)
{
    if (!all && (macs_csv == NULL || macs_csv[0] == '\0')) {
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
        if (!all && !auto_bridge_marked(macs_csv, &devs[i])) {
            continue;
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
