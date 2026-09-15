#include "auto_bridge.h"

#if defined(TARGET_WEBOS)

#include <stdio.h>
#include <stdlib.h>
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
 * - The identity may be blank: a device without one is remembered by its name
 *   alone, and every connected device of that name is bridged with it.
 * - Since the same day a mark is a whole DEVICE, every part of it, rather than
 *   one of its parts (device_groups.h).
 * Stored as "identity|name". ⓘ Marks saved earlier still match -- see
 * entry_matches. */

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

/* The name a mark stores: the name given, with the list's two separators
 * blanked so a name cannot split an entry, and cut to MARK_NAME_MAX -- the same
 * cut on both sides of a comparison, so a long name still matches itself. */
static void mark_name(const char *name, char *out, size_t out_len)
{
    char raw[128];
    snprintf(raw, sizeof raw, "%s", name != NULL ? name : "");
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

/* ⓘ Two identities that are both blank ARE the same here, unlike in the core's
 * rule: the name is what carries the match for a device without one. A serial
 * of zeros counts as blank. */
static bool identities_match(const char *a, const char *b)
{
    const bool usable_a = bridge_identity_usable(a);
    const bool usable_b = bridge_identity_usable(b);
    if (!usable_a || !usable_b) {
        return !usable_a && !usable_b;
    }
    return bridge_identity_same(a, b);
}

static void part_identity(const ctm_bridge_dev_t *d, char *out, size_t out_len)
{
    if (!auto_bridge_identity(d, out, out_len)) {
        out[0] = '\0';
    }
}

/* Does one stored entry name this device? */
static bool entry_matches(const char *entry, const ctm_bridge_dev_t *devs, const device_group_t *g)
{
    const char *bar = strchr(entry, '|');
    if (bar == NULL) {
        /* Saved before names were kept: an identity alone, the device's or one
         * of its parts'. */
        if (!bridge_identity_usable(entry)) {
            return false;
        }
        if (bridge_identity_same(entry, g->identity)) {
            return true;
        }
        for (int p = 0; p < g->part_count; ++p) {
            char id[64];
            part_identity(&devs[g->part[p]], id, sizeof id);
            if (bridge_identity_same(entry, id)) {
                return true;
            }
        }
        return false;
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

    char device[128];
    mark_name(g->name, device, sizeof device);
    if (identities_match(id, g->identity) && strcasecmp(nm, device) == 0) {
        return true;
    }
    /* ⓘ Builds 324 to 326 marked one PART, by that part's identity and name.
     * Such a mark now stands for the device the part belongs to. */
    for (int p = 0; p < g->part_count; ++p) {
        const ctm_bridge_dev_t *d = &devs[g->part[p]];
        char part[128];
        mark_name(d->name, part, sizeof part);
        if (strcasecmp(nm, part) != 0) {
            continue;
        }
        char pid[64];
        part_identity(d, pid, sizeof pid);
        if (identities_match(id, pid) || identities_match(id, g->identity)) {
            return true;
        }
    }
    return false;
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

    /* A DualSense, an Edge or a DS4: its own MAC. ⓘ The kind is "ds5",
     * "ds5_usb", "ds5e", "ds5e_usb", "ds4" or "ds4_usb", so its first three
     * letters are enough.
     * ⭐ THE DS4 JOINED 2026-09-15 (rhoquinn8217). On the C1's kernel a cabled
     * DS4 has no uniq and no USB serial, so it had no identity at all and a mark
     * matched it by name alone: two DS4s could not be told apart. SDL reads its
     * MAC the same way as a DualSense's. */
    if (strncmp(d->kind, "ds5", 3) == 0 || strncmp(d->kind, "ds4", 3) == 0) {
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

void auto_bridge_mark_key(const device_group_t *g, char *out, size_t out_len)
{
    if (out == NULL || out_len == 0) {
        return;
    }
    out[0] = '\0';
    if (g == NULL) {
        return;
    }
    char name[128];
    mark_name(g->name, name, sizeof name);
    snprintf(out, out_len, "%s|%s", bridge_identity_usable(g->identity) ? g->identity : "", name);
}

bool auto_bridge_marked(const char *csv, const ctm_bridge_dev_t *devs, const device_group_t *g)
{
    if (csv == NULL || csv[0] == '\0' || devs == NULL || g == NULL || g->part_count == 0) {
        return false;
    }
    char name[128];
    mark_name(g->name, name, sizeof name);
    if (!bridge_identity_usable(g->identity) && name[0] == '\0') {
        return false;   /* nothing to know it by */
    }

    char list[AUTO_BRIDGE_LIST_MAX];
    snprintf(list, sizeof(list), "%s", csv);
    char *save = NULL;
    for (char *tok = strtok_r(list, ",", &save); tok != NULL;
         tok = strtok_r(NULL, ",", &save)) {
        char one[256];
        mac_trim(tok, one, sizeof(one));
        if (one[0] != '\0' && entry_matches(one, devs, g)) {
            return true;
        }
    }
    return false;
}

void auto_bridge_mark_set(const char *csv, const ctm_bridge_dev_t *devs, const device_group_t *g,
                          bool on, char *out, size_t out_len)
{
    if (out == NULL || out_len == 0) {
        return;
    }
    out[0] = '\0';
    if (devs == NULL || g == NULL || g->part_count == 0) {
        snprintf(out, out_len, "%s", csv ? csv : "");
        return;
    }
    char name[128];
    mark_name(g->name, name, sizeof name);

    /* ⭐ Rebuilt from the survivors rather than edited in place, so removing an
     * entry cannot leave a stray comma and the order of the rest is kept --
     * a rewritten file that reshuffles looks like something went wrong. */
    char list[AUTO_BRIDGE_LIST_MAX];
    snprintf(list, sizeof(list), "%s", csv ? csv : "");
    char *save = NULL;
    bool present = false;
    for (char *tok = strtok_r(list, ",", &save); tok != NULL;
         tok = strtok_r(NULL, ",", &save)) {
        char one[256];
        mac_trim(tok, one, sizeof(one));
        if (one[0] == '\0') {
            continue;
        }
        if (entry_matches(one, devs, g)) {
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
    if (on && !present && (bridge_identity_usable(g->identity) || name[0] != '\0')) {
        char key[256];
        auto_bridge_mark_key(g, key, sizeof key);
        if (out[0] != '\0') {
            strncat(out, ",", out_len - strlen(out) - 1);
        }
        strncat(out, key, out_len - strlen(out) - 1);
    }
}

int auto_bridge_run(const char *csv, bool all)
{
    if (!all && (csv == NULL || csv[0] == '\0')) {
        return 0;   /* nothing marked: the common case, and it costs nothing */
    }

    /* ⓘ On the heap: two lists of this size are too much to put on whichever
     * thread starts the stream. */
    ctm_bridge_dev_t *devs = calloc(DEVICE_PARTS_MAX, sizeof *devs);
    device_group_t *groups = calloc(DEVICE_GROUPS_MAX, sizeof *groups);
    if (devs == NULL || groups == NULL) {
        free(devs);
        free(groups);
        return 0;
    }
    /* ⓘ The full list, not the quiet one: a stream is starting, so the core is
     * up already and ctm_bridge_start has just enumerated. */
    const int n = ctm_bridge_list(devs, DEVICE_PARTS_MAX);
    const int count = device_groups_build(devs, n, groups, DEVICE_GROUPS_MAX);
    int asked = 0;

    for (int k = 0; k < count; ++k) {
        const device_group_t *g = &groups[k];
        /* ⭐ "Bridge all devices when the stream starts" overrides the marks
         * entirely: every device, with a serial or without. Otherwise only a
         * marked one. */
        if (!all && !auto_bridge_marked(csv, devs, g)) {
            continue;
        }
        /* ⭐⭐ EVERY PART OF IT, each on its own. */
        for (int p = 0; p < g->part_count; ++p) {
            const ctm_bridge_dev_t *d = &devs[g->part[p]];
            if (d->plugged || d->node[0] == '\0') {
                continue;
            }
            /* ⭐ THE SAME PATH THE PANEL USES, so a bridge that happens by
             * itself and one a user asked for cannot drift apart. ⓘ Since
             * 2026-09-12 that is literally one function,
             * bridge_request_device(), rather than a copy of the panel's
             * decision kept here. */
            (void) bridge_request_device(d);
            asked++;
        }
    }
    free(groups);
    free(devs);
    return asked;
}

#endif /* TARGET_WEBOS */
