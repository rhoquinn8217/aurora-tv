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

/* The device's identity, written into out; false if it has none. A DualSense or
 * Edge is its own MAC, and any other device its serial -- never a blank or
 * all-zeros one. Shared by the settings window, the panel and the control port,
 * so what is shown is what is matched. */
bool auto_bridge_identity(const ctm_bridge_dev_t *d, char *out, size_t out_len);

/* ⭐⭐ A MARK REMEMBERS THE IDENTITY AND THE NAME TOGETHER (rhoquinn8217,
 * 2026-09-14), for every device: "3286967D|Generic X-Box pad". A device with no
 * identity is remembered by its name alone, "|KMA2 LG RF dongle A2". The key a
 * device is stored under goes into out. */
void auto_bridge_mark_key(const ctm_bridge_dev_t *d, char *out, size_t out_len);

/* Is this device marked in the stored list? Comma-separated. Identities match
 * however they are punctuated and names without regard to case. ⓘ An entry
 * saved before names were kept has no '|' and matches on the identity alone. */
bool auto_bridge_marked(const char *macs_csv, const ctm_bridge_dev_t *d);

/* The list with this device marked or unmarked, written into out. Every entry
 * that matches the device goes when unmarking, an old identity-only one too.
 * The order of the survivors is kept, so a rewrite does not shuffle the file. */
void auto_bridge_mark_set(const char *macs_csv, const ctm_bridge_dev_t *d, bool on,
                          char *out, size_t out_len);

/* Bridge the devices the user chose, as a stream starts: with `all`, every
 * connected device that is not already bridged, marks or no marks; otherwise
 * each marked one. `macs_csv` is the stored list; NULL or empty marks nothing.
 * Returns how many were asked to bridge. */
int auto_bridge_run(const char *macs_csv, bool all);

#else

#define auto_bridge_run(macs_csv, all) (0)

#endif

#endif /* AUTO_BRIDGE_H */
