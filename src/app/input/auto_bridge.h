/* Bridging the devices a user marked, when a stream starts.
 *
 * ⭐ The mark is per DEVICE, and it is the user's: nothing here decides that a
 * device should be bridged. With no marked device connected this does nothing
 * at all.
 *
 * ⛔⛔ NOT THE AUTO-PLUG THAT WAS REMOVED. That bridged EVERYTHING on every
 * change and took working devices away from the TV without being asked. This
 * bridges the named ones, once, and nothing else.
 *
 * ⛔ STREAM START ONLY. A marked controller cabled DURING a stream is bridged
 * by hand, as before. For a few hours on 2026-09-08 it bridged itself from the
 * hotplug callback, which ran a multi-second enumerate on that thread while
 * holding the device lock; a quick cable replug queued several and the app
 * froze. That path is not coming back. */

#ifndef AUTO_BRIDGE_H
#define AUTO_BRIDGE_H

#include <stdbool.h>
#include <stddef.h>

#if defined(TARGET_WEBOS)

#include "ctm_bridge_glue.h"
#include "device_groups.h"

/* The longest stored list of marks the callers build. */
#define AUTO_BRIDGE_LIST_MAX 4096

/* One part's identity, written into out; false if it has none. A DualSense or
 * Edge is its own MAC, and any other part its serial -- never a blank or
 * all-zeros one. ⓘ A device's identity is decided from its parts in
 * device_groups.c, which starts here. */
bool auto_bridge_identity(const ctm_bridge_dev_t *d, char *out, size_t out_len);

/* ⭐⭐ A MARK IS A DEVICE'S IDENTITY AND ITS NAME TOGETHER (rhoquinn8217,
 * 2026-09-14): "<identity>|<name>". A device with no identity, or only zeros,
 * is remembered by its name alone, "|<name>", and marks every connected device
 * of that name. The key goes into out. */
void auto_bridge_mark_key(const device_group_t *g, char *out, size_t out_len);

/* Is this device marked in the stored list? Comma-separated. `devs` is the list
 * the device was built from. Identities match however they are punctuated, and
 * names without regard to case. ⓘ Marks saved by earlier builds still match:
 * one part's identity and name (builds 324 to 326), or an identity alone. */
bool auto_bridge_marked(const char *csv, const ctm_bridge_dev_t *devs, const device_group_t *g);

/* The list with this device marked or unmarked, written into out. Unmarking
 * drops every entry that names the device, older ones too. The order of the
 * rest is kept, so a rewrite does not shuffle the file. */
void auto_bridge_mark_set(const char *csv, const ctm_bridge_dev_t *devs, const device_group_t *g,
                          bool on, char *out, size_t out_len);

/* Bridge the devices the user chose, as a stream starts: with `all`, every part
 * of every connected device that is not already bridged, marks or no marks;
 * otherwise every part of each marked device. `csv` is the stored list; NULL or
 * empty marks nothing. Returns how many parts were asked to bridge. */
int auto_bridge_run(const char *csv, bool all);

#else

#define auto_bridge_run(csv, all) (0)

#endif

#endif /* AUTO_BRIDGE_H */
