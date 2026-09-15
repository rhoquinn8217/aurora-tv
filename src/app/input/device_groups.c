#include "device_groups.h"

#if defined(TARGET_WEBOS)

#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "auto_bridge.h"

static bool dev_is(const ctm_bridge_dev_t *d, const char *vid, const char *pid)
{
    return strcmp(d->vid, vid) == 0 && strcmp(d->pid, pid) == 0;
}

const char *device_short_name(const ctm_bridge_dev_t *d)
{
    /* ⛔ MATCHED ON VID/PID, NOT ON KIND, for the pads. `kind` is eight bytes, so
     * "ds5e_usb" arrives as "ds5e_us" and matches nothing: a wired Edge fell
     * through to its full system name. vid and pid cannot be truncated. */
    if (dev_is(d, "054c", "0ce6")) return "DualSense";
    if (dev_is(d, "054c", "0df2")) return "DualSense Edge";
    if (dev_is(d, "054c", "09cc") || dev_is(d, "054c", "05c4")) return "DualShock 4";
    if (strcmp(d->kind, "puck") == 0) return "Steam Controller";
    if (strcmp(d->kind, "xbox") == 0) return "Xbox Controller";
    return d->name;
}

void device_part_type(const ctm_bridge_dev_t *d, char *out, size_t out_len)
{
    if (out == NULL || out_len == 0) {
        return;
    }
    /* ⓘ Anything that is not a controller, a keyboard or a mouse is OTHER
     * (rhoquinn8217, 2026-09-14), never no word at all. Media and system keys
     * already read as a keyboard in the core. */
    const char *type = (d != NULL && d->type[0] != '\0') ? d->type : "other";
    size_t i = 0;
    for (; type[i] != '\0' && i + 1 < out_len; ++i) {
        out[i] = (char) toupper((unsigned char) type[i]);
    }
    out[i] = '\0';
}

static bool has_alnum(const char *s)
{
    for (; s != NULL && *s != '\0'; ++s) {
        if (isalnum((unsigned char) *s)) {
            return true;
        }
    }
    return false;
}

/* ⭐⭐ WHAT A DEVICE IS REMEMBERED BY (rhoquinn8217, 2026-09-14), in order:
 *  1. A PlayStation pad's own MAC. It follows the pad across a cable, Bluetooth
 *     and a dongle, which a serial does not.
 *  2. The USB device's own serial. Every part carries the same one, so any part
 *     answers for the device. ⓘ A serial of zeros is SHOWN, so the reader sees
 *     why the device is remembered by its name, and identifies nothing.
 *  3. Off USB, the part's own: over Bluetooth that is its MAC.
 * Otherwise it has none and is remembered by its name alone.
 *
 * ⭐ TWO WAYS TO SHOW IT (rhoquinn8217, 2026-09-15). The Auto Bridge window's
 * cards keep the tag ("MAC: ", "serial: "); the streaming overlay's USB Bridge
 * panel shows the value alone, where the tag cost a long serial its room.
 * "(no serial)" is the same in both: it is not a value. */
static void group_shown(device_group_t *g, const char *tag, const char *value)
{
    snprintf(g->shown, sizeof g->shown, "%s%s", tag, value);
    snprintf(g->shown_value, sizeof g->shown_value, "%s", value);
}

static void group_identity(const ctm_bridge_dev_t *devs, device_group_t *g)
{
    g->identity[0] = '\0';
    for (int p = 0; p < g->part_count; ++p) {
        const ctm_bridge_dev_t *d = &devs[g->part[p]];
        char mac[64];
        if (strncmp(d->kind, "ds5", 3) == 0 && auto_bridge_identity(d, mac, sizeof mac)) {
            snprintf(g->identity, sizeof g->identity, "%s", mac);
            group_shown(g, "MAC: ", mac);
            return;
        }
    }
    for (int p = 0; p < g->part_count; ++p) {
        const ctm_bridge_dev_t *d = &devs[g->part[p]];
        if (has_alnum(d->usb_serial)) {
            if (bridge_identity_usable(d->usb_serial)) {
                snprintf(g->identity, sizeof g->identity, "%s", d->usb_serial);
            }
            group_shown(g, "serial: ", d->usb_serial);
            return;
        }
    }
    for (int p = 0; p < g->part_count; ++p) {
        const ctm_bridge_dev_t *d = &devs[g->part[p]];
        if (bridge_identity_usable(d->serial)) {
            snprintf(g->identity, sizeof g->identity, "%s", d->serial);
            group_shown(g, bridge_identity_mac_shaped(d->serial) ? "MAC: " : "serial: ", d->serial);
            return;
        }
    }
    group_shown(g, "", "(no serial)");
}

/* One device's name: a single part is called what it is, several by the USB
 * device they share, as its maker and product. */
static void group_name(const ctm_bridge_dev_t *devs, device_group_t *g)
{
    if (g->part_count > 1) {
        for (int p = 0; p < g->part_count; ++p) {
            const ctm_bridge_dev_t *d = &devs[g->part[p]];
            if (d->device_name[0] != '\0') {
                snprintf(g->name, sizeof g->name, "%s", d->device_name);
                return;
            }
        }
    }
    snprintf(g->name, sizeof g->name, "%s", device_short_name(&devs[g->part[0]]));
}

/* Parts by interface number; one without a number after those with one. */
static int part_order(const ctm_bridge_dev_t *devs, int a, int b)
{
    const ctm_bridge_dev_t *da = &devs[a];
    const ctm_bridge_dev_t *db = &devs[b];
    const int ia = da->iface < 0 ? INT_MAX : da->iface;
    const int ib = db->iface < 0 ? INT_MAX : db->iface;
    if (ia != ib) {
        return ia < ib ? -1 : 1;
    }
    const int by_node = strcmp(da->node, db->node);
    if (by_node != 0) {
        return by_node;
    }
    return a < b ? -1 : (a > b ? 1 : 0);
}

/* Devices by name, so identical devices sit together; then by what is shown
 * under the name, then by node, so the order does not move between refreshes. */
static int group_order(const ctm_bridge_dev_t *devs, const device_group_t *a, const device_group_t *b)
{
    const int by_name = strcasecmp(a->name, b->name);
    if (by_name != 0) {
        return by_name;
    }
    const int by_shown = strcasecmp(a->shown, b->shown);
    if (by_shown != 0) {
        return by_shown;
    }
    return strcmp(devs[a->part[0]].node, devs[b->part[0]].node);
}

int device_groups_build(const ctm_bridge_dev_t *devs, int n, device_group_t *out, int max)
{
    if (devs == NULL || out == NULL || n <= 0 || max <= 0) {
        return 0;
    }
    int count = 0;
    for (int i = 0; i < n; ++i) {
        const ctm_bridge_dev_t *d = &devs[i];
        int k = 0;
        for (; k < count; ++k) {
            if (strcmp(devs[out[k].part[0]].group, d->group) == 0) {
                break;
            }
        }
        if (k == count) {
            if (count >= max) {
                continue;
            }
            memset(&out[count], 0, sizeof out[count]);
            count++;
        }
        device_group_t *g = &out[k];
        if (g->part_count >= DEVICE_GROUP_PARTS) {
            continue;
        }
        /* Kept in interface order as it is filled. */
        int at = g->part_count;
        while (at > 0 && part_order(devs, g->part[at - 1], i) > 0) {
            g->part[at] = g->part[at - 1];
            at--;
        }
        g->part[at] = i;
        g->part_count++;
        if (d->plugged) {
            g->plugged++;
        }
        if (d->node[0] != '\0') {
            g->has_node = true;
        }
    }
    for (int k = 0; k < count; ++k) {
        group_name(devs, &out[k]);
        group_identity(devs, &out[k]);
    }
    /* A short list, so an insertion sort: it needs the parts to compare nodes,
     * which qsort's comparator could not be given. */
    for (int k = 1; k < count; ++k) {
        device_group_t held = out[k];
        int at = k;
        while (at > 0 && group_order(devs, &out[at - 1], &held) > 0) {
            out[at] = out[at - 1];
            at--;
        }
        out[at] = held;
    }
    return count;
}

#endif /* TARGET_WEBOS */
