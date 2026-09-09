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

/* Bridge every marked controller that is connected and not already bridged.
 * `macs_csv` is the stored list, comma-separated; NULL or empty does nothing.
 * Returns how many were asked to bridge. Call once, as a stream starts. */
int auto_bridge_run(const char *macs_csv);

/* Is this MAC in the list? Comma-separated, case-insensitive, spaces ignored.
 * Shared with the settings pane so the two cannot disagree about membership. */
bool auto_bridge_list_has(const char *macs_csv, const char *mac);

/* The list with `mac` added or removed, written into out. The order of the
 * survivors is kept, so a rewrite does not shuffle the file. */
void auto_bridge_list_set(const char *macs_csv, const char *mac, bool on,
                          char *out, size_t out_len);

#else

#define auto_bridge_run(macs_csv) (0)

#endif

#endif /* AUTO_BRIDGE_H */
