/* Bridging the devices a person has ticked, by themselves: when a stream
 * starts, and when one connects while it runs.
 *
 * ⭐ THE TICK IS PER DEVICE, and it is the person's. Any device bridges itself
 * when it is ticked in the Auto Bridge window; ⭐ a DualSense or an Edge is
 * ticked until someone unticks it (rhoquinn8217, 2026-10-15: "DS5 and DS5
 * edges are just ticked by default and can be untick"), to make a DualSense's
 * setup as seamless as possible.
 *
 * ⛔⛔ NOT THE AUTO-PLUG THAT WAS REMOVED. That bridged EVERYTHING on every
 * change and took working devices away from the TV without being asked. This
 * bridges the ticked ones, and "Bridge all" only when someone chose it.
 *
 * ⭐ AND WHEN ONE CONNECTS DURING A STREAM (rhoquinn8217, 2026-10-15), which
 * was ruled out on 2026-09-08: build 301 bridged from the hotplug callback,
 * which ran a multi-second enumerate on that thread while holding the device
 * lock; a quick cable replug queued several and the app froze. Now:
 * - nothing runs in a hotplug path: the arrival watch (ctm_bridge_gesture.c)
 *   only notes which hidraw nodes appeared, from its inotify read;
 * - one look per connection, once its nodes have stopped appearing for a
 *   second, however often a cable goes in and out, on the app's loop, where one
 *   look cannot overlap another;
 * - a look is quick: the full list took 20 ms on the C1 during a stream, five
 *   runs out of five, 2026-10-15. ⚠️ It holds the device lock while it runs,
 *   and the gesture's tick takes that lock every frame, so another thread would
 *   not spare the interface: being quick and rare is what does;
 * - a controller waits until SDL has opened it, so its bridge goes the
 *   gesture's way (bridge_request.c), where a Bluetooth DualSense waits until it
 *   will show its colours (ds5_bridge_held: bridged the moment it connects, its
 *   light bar ignored every colour, 2026-10-09).
 *
 * ⭐ AND NEVER BEFORE DS5-USBIP ANSWERS. Vibepollo starts it with the stream,
 * so at a stream's start it may not be there yet: both wait until it answers,
 * and a PC without it gets no attempt and no refusal signal. */
#ifndef AUTO_BRIDGE_H
#define AUTO_BRIDGE_H

#include <stdbool.h>
#include <stddef.h>

#if defined(TARGET_WEBOS)

#include "bridge_settings.h"
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

/* Is this device in the stored list? Comma-separated. `devs` is the list the
 * device was built from. Identities match however they are punctuated, and
 * names without regard to case. ⓘ Marks saved by earlier builds still match:
 * one part's identity and name (builds 324 to 326), or an identity alone. */
bool auto_bridge_marked(const char *csv, const ctm_bridge_dev_t *devs, const device_group_t *g);

/* The list with this device in it or out of it, written into out. Taking it
 * out drops every entry that names the device, older ones too. The order of
 * the rest is kept, so a rewrite does not shuffle the file. */
void auto_bridge_mark_set(const char *csv, const ctm_bridge_dev_t *devs, const device_group_t *g,
                          bool on, char *out, size_t out_len);

/* A DualSense or an Edge: a part whose kind starts "ds5" ("ds5", "ds5_usb",
 * "ds5e", "ds5e_usb"). */
bool auto_bridge_is_dualsense(const ctm_bridge_dev_t *devs, const device_group_t *g);

/* Is this device ticked in the Auto Bridge window? A DualSense unless it is in
 * the unticked list (auto_skip); any other device when it is in the marks
 * (auto_macs). "Bridge all" is apart from it. */
bool auto_bridge_ticked(const bridge_settings_t *s, const ctm_bridge_dev_t *devs, const device_group_t *g);

/* Tick or untick a device: a DualSense out of or into the unticked list, any
 * other into or out of the marks. */
void auto_bridge_set_ticked(bridge_settings_t *s, const ctm_bridge_dev_t *devs, const device_group_t *g,
                            bool on);

/* Bridge every part not yet bridged of every ticked device, or of every device
 * with "Bridge all". Returns how many parts were asked to bridge. */
int auto_bridge_run(const bridge_settings_t *s);

/* A stream has started: its run waits, on the app's loop, until DS5-USBIP
 * answers. Any thread. */
void auto_bridge_stream_started(void);

/* The stream is over: nothing bridges itself until the next. Any thread. */
void auto_bridge_stream_stopped(void);

/* A node has just appeared in /dev (the arrival watch's inotify read). */
void auto_bridge_note_node(const char *path);

/* Once per pass of the app's loop. */
void auto_bridge_poll(bool streaming);

#else
#define auto_bridge_stream_started() ((void) 0)
#define auto_bridge_stream_stopped() ((void) 0)
#define auto_bridge_note_node(path) ((void) 0)
#define auto_bridge_poll(streaming) ((void) 0)
#endif

#endif /* AUTO_BRIDGE_H */
