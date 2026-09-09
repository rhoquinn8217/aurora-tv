#include "auto_bridge.h"

#if defined(TARGET_WEBOS)

#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "ctm_bridge_glue.h"
#include "ctm_bridge_gesture.h"
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
 * ⛔ AND IT MUST NOT KEY ON ctm_bridge_dev_t.mac. That field is the HID `uniq`,
 * which is EMPTY for a directly cabled DualSense Edge and is the DS5DONGLE'S
 * OWN SERIAL through a dongle -- it follows the dongle, not the pad. Keying on
 * it would mark the dongle: move the controller and its mark stays behind, and
 * the next controller on that dongle bridges itself uninvited. Measured on the
 * C1 2026-09-08 after it fooled two readings. */

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
        if (one[0] != '\0' && strcasecmp(one, mac) == 0) {
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
        if (strcasecmp(one, mac) == 0) {
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

int auto_bridge_run(const char *macs_csv)
{
    if (macs_csv == NULL || macs_csv[0] == '\0') {
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
        char mac[64];
        if (!ctm_bridge_gesture_mac_for_node(devs[i].node, mac, sizeof mac)) {
            continue;   /* no MAC: not an SDL controller, cannot be marked */
        }
        if (!auto_bridge_list_has(macs_csv, mac)) {
            continue;
        }
        /* ⭐ THE SAME PATH THE PANEL USES, so a bridge that happens by itself
         * and one a user asked for cannot drift apart. The gesture retires the
         * emulated pad and records that it owns the bridge; the direct plug is
         * the fallback for anything with no gesture path to borrow. */
        if (!ctm_bridge_gesture_request_bridge(devs[i].node)) {
            ctm_bridge_plug_index(devs[i].index);
        }
        asked++;
    }
    return asked;
}

#endif /* TARGET_WEBOS */
