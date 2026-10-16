#include "auto_bridge.h"

#if defined(TARGET_WEBOS)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <SDL.h>

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

bool auto_bridge_is_dualsense(const ctm_bridge_dev_t *devs, const device_group_t *g)
{
    if (devs == NULL || g == NULL) {
        return false;
    }
    for (int p = 0; p < g->part_count; ++p) {
        if (strncmp(devs[g->part[p]].kind, "ds5", 3) == 0) {
            return true;
        }
    }
    return false;
}

bool auto_bridge_ticked(const bridge_settings_t *s, const ctm_bridge_dev_t *devs, const device_group_t *g)
{
    if (s == NULL || devs == NULL || g == NULL) {
        return false;
    }
    if (auto_bridge_is_dualsense(devs, g)) {
        return !auto_bridge_marked(s->auto_skip, devs, g);
    }
    return auto_bridge_marked(s->auto_macs, devs, g);
}

void auto_bridge_set_ticked(bridge_settings_t *s, const ctm_bridge_dev_t *devs, const device_group_t *g,
                            bool on)
{
    if (s == NULL || devs == NULL || g == NULL) {
        return;
    }
    char out[AUTO_BRIDGE_LIST_MAX];
    if (auto_bridge_is_dualsense(devs, g)) {
        auto_bridge_mark_set(s->auto_skip, devs, g, !on, out, sizeof out);
        bridge_settings_set_auto_skip(s, out);
        return;
    }
    auto_bridge_mark_set(s->auto_macs, devs, g, on, out, sizeof out);
    bridge_settings_set_auto_macs(s, out);
}

int auto_bridge_run(const bridge_settings_t *s)
{
    if (s == NULL) {
        return 0;
    }

    /* ⓘ On the heap: two lists of this size are too much for the app's loop
     * to keep on its stack. */
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
        /* ⭐ "Bridge all devices when the stream starts" overrides the ticks
         * entirely: every device, with a serial or without. Otherwise only a
         * ticked one, which a DualSense is until someone unticks it. */
        if (!s->auto_all && !auto_bridge_ticked(s, devs, g)) {
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

/* ---------------------------------------------------- by themselves, when -- */

#define AB_START_WAIT_MS 20000   /* how long a stream's start waits for DS5-USBIP */
#define AB_RECHECK_MS     2000   /* how often it asks again meanwhile */
#define AB_SETTLE_MS      1000   /* after a connection's last node appears */
#define AB_SDL_WAIT_MS    6000   /* the longest a controller waits for SDL to open it */
#define AB_APPEARED_MAX     16
#define AB_WAITING_MAX       8

static SDL_atomic_t s_start_armed;      /* set by a stream's start, from its thread */
static uint32_t s_start_since;          /* when the loop first saw it armed */
static uint32_t s_last_recheck;
static bool s_streaming;                /* the loop's own view: a stream, with bridging on */

/* Nodes that appeared during the stream and have not been looked at. */
static char s_appeared[AB_APPEARED_MAX][64];
static int s_appeared_count;
static uint32_t s_last_appeared;

/* Parts of a ticked controller that connected, waiting for SDL to open them. */
static ctm_bridge_dev_t s_waiting[AB_WAITING_MAX];
static uint32_t s_waiting_since[AB_WAITING_MAX];
static int s_waiting_count;

void auto_bridge_stream_started(void)
{
    SDL_AtomicSet(&s_start_armed, 1);
}

void auto_bridge_stream_stopped(void)
{
    SDL_AtomicSet(&s_start_armed, 0);
}

void auto_bridge_note_node(const char *path)
{
    if (!s_streaming || path == NULL || path[0] == '\0') {
        return;   /* outside a stream, the next start's run takes what is there */
    }
    s_last_appeared = SDL_GetTicks();
    for (int i = 0; i < s_appeared_count; ++i) {
        if (strcmp(s_appeared[i], path) == 0) {
            return;
        }
    }
    if (s_appeared_count < AB_APPEARED_MAX) {
        snprintf(s_appeared[s_appeared_count++], sizeof s_appeared[0], "%s", path);
    }
}

static void ab_forget(void)
{
    s_appeared_count = 0;
    s_waiting_count = 0;
}

static bool ab_appeared(const char *node)
{
    if (node == NULL || node[0] == '\0') {
        return false;
    }
    for (int i = 0; i < s_appeared_count; ++i) {
        if (strcmp(s_appeared[i], node) == 0) {
            return true;
        }
    }
    return false;
}

/* Does DS5-USBIP answer? ⓘ Asks again every two seconds while it does not: at a
 * stream's start it may be starting with the stream. */
static bool ab_listener_ready(uint32_t now)
{
    if (ctm_bridge_agent_probed() && ctm_bridge_agent_online()) {
        return true;
    }
    if (now - s_last_recheck >= AB_RECHECK_MS) {
        s_last_recheck = now;
        ctm_bridge_agent_recheck();
    }
    return false;
}

/* ONE look at what connected: the devices its nodes belong to, ticked ones
 * bridged, controllers SDL has not opened yet set to wait for it. */
static void ab_look(uint32_t now)
{
    ctm_bridge_dev_t *devs = calloc(DEVICE_PARTS_MAX, sizeof *devs);
    device_group_t *groups = calloc(DEVICE_GROUPS_MAX, sizeof *groups);
    if (devs == NULL || groups == NULL) {
        free(devs);
        free(groups);
        s_appeared_count = 0;
        return;
    }
    const int n = ctm_bridge_list(devs, DEVICE_PARTS_MAX);
    const int count = device_groups_build(devs, n, groups, DEVICE_GROUPS_MAX);
    const uint32_t took = SDL_GetTicks() - now;
    const bridge_settings_t *s = &app_configuration->bridge;
    int asked = 0;

    for (int k = 0; k < count; ++k) {
        const device_group_t *g = &groups[k];
        bool connected = false;
        for (int p = 0; p < g->part_count && !connected; ++p) {
            connected = ab_appeared(devs[g->part[p]].node);
        }
        if (!connected) {
            continue;
        }
        if (!s->auto_all && !auto_bridge_ticked(s, devs, g)) {
            ctm_bridge_gesture_note("auto bridge: %s connected, not ticked -- left with the TV", g->name);
            continue;
        }
        for (int p = 0; p < g->part_count; ++p) {
            const ctm_bridge_dev_t *d = &devs[g->part[p]];
            if (d->plugged || d->node[0] == '\0') {
                continue;
            }
            if (d->controller && ctm_bridge_gesture_player_for_node(d->node) < 0) {
                if (s_waiting_count < AB_WAITING_MAX) {
                    s_waiting[s_waiting_count] = *d;
                    s_waiting_since[s_waiting_count] = now;
                    s_waiting_count++;
                }
                continue;
            }
            ctm_bridge_gesture_note("auto bridge: %s connected -- bridging %s", d->name, d->node);
            (void) bridge_request_device(d);
            asked++;
        }
    }
    ctm_bridge_gesture_note("auto bridge: looked at %d device(s) for %d new node(s) in %u ms; asked %d, "
                            "%d waiting for SDL",
                            count, s_appeared_count, (unsigned) took, asked, s_waiting_count);
    s_appeared_count = 0;
    free(groups);
    free(devs);
}

void auto_bridge_poll(bool streaming)
{
    const bool on = streaming && app_configuration != NULL && app_configuration->bridge.enable;
    if (!on) {
        if (s_streaming) {
            ab_forget();
        }
        s_streaming = false;
        s_start_since = 0;
        return;   /* ⓘ Armed stays armed: only the stream's stop disarms it */
    }
    s_streaming = true;
    const uint32_t now = SDL_GetTicks();

    /* ⭐ 1. THE STREAM'S START: every ticked device, once DS5-USBIP answers. */
    if (SDL_AtomicGet(&s_start_armed)) {
        if (s_start_since == 0) {
            s_start_since = now;
        }
        if (!ab_listener_ready(now)) {
            if (now - s_start_since >= AB_START_WAIT_MS) {
                SDL_AtomicSet(&s_start_armed, 0);
                s_start_since = 0;
                ab_forget();
                ctm_bridge_gesture_note("auto bridge: DS5-USBIP did not answer within %u s of the stream's "
                                        "start -- nothing bridged by itself", AB_START_WAIT_MS / 1000);
            }
            return;
        }
        SDL_AtomicSet(&s_start_armed, 0);
        const uint32_t waited = now - s_start_since;
        s_start_since = 0;
        ab_forget();   /* what connected meanwhile, this run takes */
        const int n = auto_bridge_run(&app_configuration->bridge);
        ctm_bridge_gesture_note("auto bridge at the stream's start: asked for %d part(s), DS5-USBIP "
                                "answered after %u ms", n, (unsigned) waited);
        return;
    }

    /* 2. Controllers that connected, once SDL has opened them: cheap, no list. */
    for (int i = 0; i < s_waiting_count;) {
        const ctm_bridge_dev_t *d = &s_waiting[i];
        const bool opened = ctm_bridge_gesture_player_for_node(d->node) >= 0;
        if (!opened && now - s_waiting_since[i] < AB_SDL_WAIT_MS) {
            ++i;
            continue;
        }
        ctm_bridge_gesture_note("auto bridge: %s connected -- bridging %s%s", d->name, d->node,
                                opened ? "" : " (SDL never opened it)");
        (void) bridge_request_device(d);
        s_waiting[i] = s_waiting[s_waiting_count - 1];
        s_waiting_since[i] = s_waiting_since[s_waiting_count - 1];
        s_waiting_count--;
    }

    /* 3. What connected: one look, once its nodes have stopped appearing. */
    if (s_appeared_count == 0 || now - s_last_appeared < AB_SETTLE_MS) {
        return;
    }
    if (!ab_listener_ready(now)) {
        if (now - s_last_appeared >= AB_START_WAIT_MS) {
            ctm_bridge_gesture_note("auto bridge: DS5-USBIP does not answer -- %d new node(s) left with the TV",
                                    s_appeared_count);
            s_appeared_count = 0;
        }
        return;
    }
    ab_look(now);
}

#endif /* TARGET_WEBOS */
