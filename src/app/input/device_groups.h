/* The devices a person plugged in, rather than the parts the kernel lists.
 *
 * ⭐⭐ EVERY PART OF A DEVICE GOES TOGETHER (rhoquinn8217, 2026-09-14). One USB
 * device may present several HID interfaces -- a mouse dongle five, a controller
 * a keyboard part beside its pad -- and the core lists each as a device of its
 * own. The Auto Bridge window and the USB Bridge panel show a DEVICE, and
 * selecting, bridging or releasing it takes every part at once.
 *
 * ⓘ Each part is still bridged on its own and still reaches the listener as its
 * own device. Presenting them there as one composite device is later work.
 *
 * The core says which parts belong together (ctm_bridge_dev_t.group, the USB
 * physical path without its interface). This gathers them, names the device and
 * decides what it is remembered by, in one place, so every screen agrees. */

#ifndef DEVICE_GROUPS_H
#define DEVICE_GROUPS_H

#include <stdbool.h>
#include <stddef.h>

#if defined(TARGET_WEBOS)

#include "ctm_bridge_glue.h"

/* The most parts a device keeps, the most devices a list holds, and the most
 * parts a list is read with. ⓘ The busiest device measured, a Razer Orochi V2
 * dongle, has five parts. */
#define DEVICE_GROUP_PARTS 16
#define DEVICE_GROUPS_MAX  32
#define DEVICE_PARTS_MAX   32

typedef struct {
    /* What the device is called. A device of one part is called what that part
     * is (a DualSense by that name, not by its maker's long one); a device of
     * several by its USB maker and product. */
    char name[128];
    /* What auto bridge remembers it by, together with the name: a PlayStation
     * pad's MAC, otherwise the device's serial. "" when it has none, or only
     * zeros. */
    char identity[64];
    /* The line under the name: exactly one of the MAC, the serial, or "(no
     * serial)", with no "MAC:" or "serial:" before it (2026-09-15). ⓘ A serial
     * of zeros is shown as it is, although it identifies nothing. */
    char shown[96];
    /* Positions in the list the device was built from, by interface number. */
    int part[DEVICE_GROUP_PARTS];
    int part_count;
    int plugged;   /* how many of its parts are bridged */
    bool has_node; /* some part can be bridged: it has a device node */
} device_group_t;

/* Gathers the parts in devs into devices, ordered by name, and writes them into
 * out. Returns how many. */
int device_groups_build(const ctm_bridge_dev_t *devs, int n, device_group_t *out, int max);

/* A part's name as the kernel reported it, but for pads known by a shorter
 * one: "Sony Interactive Entertainment DualSense Wireless Controller" is most
 * of a screen. */
const char *device_short_name(const ctm_bridge_dev_t *d);

/* What a part is, for a person reading it: "CONTROLLER", "KEYBOARD", "MOUSE"
 * or "OTHER". */
void device_part_type(const ctm_bridge_dev_t *d, char *out, size_t out_len);

#endif /* TARGET_WEBOS */

#endif /* DEVICE_GROUPS_H */
