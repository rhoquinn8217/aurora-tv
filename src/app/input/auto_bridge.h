/* Bridging the controllers a user marked, when a stream starts.
 *
 * ⭐ The mark is per CONTROLLER, by the controller's own MAC, and it is the
 * user's: nothing here decides that a device should be bridged. With no marked
 * controller connected this does nothing at all.
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

/* What a mark keys on, written into out; false if the device cannot be marked.
 * A DualSense or Edge is its own MAC, and any other device its serial -- never
 * a blank or all-zeros one. Shared by the settings window, the panel and the
 * control port, so what is shown is what is matched. */
bool auto_bridge_identity(const ctm_bridge_dev_t *d, char *out, size_t out_len);

/* Bridge the devices the user chose, as a stream starts: with `all`, every
 * connected device that is not already bridged, marks or no marks; otherwise
 * each marked one. `macs_csv` is the stored list, comma-separated; NULL or
 * empty marks nothing. Returns how many were asked to bridge. */
int auto_bridge_run(const char *macs_csv, bool all);

/* Is this identity in the list? Comma-separated; case and punctuation do not
 * matter, so a MAC written with dashes matches one written with colons.
 * Shared with the settings pane so the two cannot disagree about membership. */
bool auto_bridge_list_has(const char *macs_csv, const char *mac);

/* The list with `mac` added or removed, written into out. The order of the
 * survivors is kept, so a rewrite does not shuffle the file. */
void auto_bridge_list_set(const char *macs_csv, const char *mac, bool on,
                          char *out, size_t out_len);

#else

#define auto_bridge_run(macs_csv, all) (0)

#endif

#endif /* AUTO_BRIDGE_H */
