/* moonlight-facing glue for the embedded CTM bridge core. Replicates the startup
 * the standalone app does in ui_app.c (stopSniff worker -> discover agent ->
 * enumerate -> bridge), minus the LVGL UI. Runs the bridge in-process; the
 * controller threads own the physical HID (hidraw + EVIOCGRAB), so moonlight must
 * have released its own input grip (see the ctm_bridge setting). */

#include "ctm_bridge_glue.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "ctm_state.h"   /* core API + shared globals (g_running, g_scan, ...) */
#include "ctm_hostmouse.h" /* TV-pointer synthesizer feed (kind "hid") */
#include "ctm_monitor.h" /* hotplug: connect/disconnect watch thread */
#include "device_identity.inl" /* the identity rules, shared with the core */

static bool s_active = false;

void ctm_bridge_set_host(const char *host, int port)
{
    ctm_bridge_set_agent_host(host, port);
}
/* ⓘ AUTO-PLUG IS GONE, and so is the switch that kept it off. It plugged ALL
 * recognised controllers on stream start. On a TV with one controller that is a
 * convenience; on a hub carrying a keyboard, a mouse and a controller it takes
 * all of them --
 * bridging claims a device exclusively, so the keyboard and mouse stop working
 * on the TV, and every session opens with a cascade of connect chimes and a
 * cleanup. Observed on three TVs.
 *
 * Nothing needs it now: a controller is bridged by its gesture, the overlay
 * panel or the user's own Auto Bridge marks. ⛔ The pinned `s_autoplug = false`
 * and the three branches it guarded were removed 2026-09-15 (a switch nobody
 * can turn on reads as a choice somebody is making). */

/* Enumerate + build the logical model + Stage-1 puck enumeration capture. The
 * Steam puck only exposes its full composite if g_puck_enum is cached BEFORE the
 * plug; the standalone app does this in refresh_devices(), so the glue must too. */
static void ctm_glue_enumerate(void)
{
    enumerate_devices(&g_scan);
    build_logical_devices(&g_scan, &g_devices);
    bool puck = false;
    for (int i = 0; i < g_devices.count; ++i) {
        if (strcmp(bridge_kind_for_item(&g_devices.items[i]), "puck") == 0) {
            puck_enum_capture(g_devices.items[i].vid, g_devices.items[i].pid);
            puck = true;
            break;
        }
    }
    if (!puck) {
        g_puck_enum.valid = 0;
    }
}

/* Serialises every g_devices/g_scan access so the hotplug monitor thread can't
 * race the UI thread's panel calls. */
static pthread_mutex_t s_dev_mutex = PTHREAD_MUTEX_INITIALIZER;
static ctm_monitor_t *s_monitor = NULL;

/* ⭐⭐ WHAT A RECONNECT MAY PUT BACK: ONLY WHAT AN OUTAGE TOOK.
 *
 * ⛔ THE FAULT: when a stream came back from an auto-reconnect, every recognised
 * controller that was not bridged got bridged, including ones nobody had asked
 * for, and plugged directly, so their emulated pads stayed on the host too. It
 * went unnoticed while most controllers could not be bridged anyway; a wired
 * Xbox pad can be now (2026-09-13).
 *
 * ➡️ The reaper remembers each device it releases because the host went away,
 * and the reconnect re-plugs exactly those. A device the user released is
 * never on the list. Cleared whenever the bridge starts or stops.
 *
 * ⓘ Kept by logical device key: a per-node session's key is "<device>#<node>",
 * and the part before the '#' is the device. Guarded by s_dev_mutex. */
#define DROPPED_MAX 16
static char s_dropped[DROPPED_MAX][96];
static int s_dropped_count = 0;

static void dropped_remember_locked(const char *session_key)
{
    char item_key[96];
    snprintf(item_key, sizeof(item_key), "%s", session_key);
    char *hash = strchr(item_key, '#');
    if (hash) {
        *hash = '\0';
    }
    for (int i = 0; i < s_dropped_count; ++i) {
        if (strcmp(s_dropped[i], item_key) == 0) {
            return;
        }
    }
    if (s_dropped_count < DROPPED_MAX) {
        snprintf(s_dropped[s_dropped_count++], sizeof(s_dropped[0]), "%s", item_key);
    }
}

static int dropped_index_locked(const char *item_key)
{
    for (int i = 0; i < s_dropped_count; ++i) {
        if (strcmp(s_dropped[i], item_key) == 0) {
            return i;
        }
    }
    return -1;
}

static void dropped_forget_locked(int index)
{
    if (index < 0 || index >= s_dropped_count) {
        return;
    }
    for (int i = index; i + 1 < s_dropped_count; ++i) {
        memcpy(s_dropped[i], s_dropped[i + 1], sizeof(s_dropped[0]));
    }
    --s_dropped_count;
}

/* Re-plug what an outage dropped. Caller MUST hold s_dev_mutex. */
static int glue_plug_all_locked(void)
{
    if (s_dropped_count == 0) {
        return 0;   /* nothing was dropped: the common case, and no enumeration */
    }
    ctm_glue_enumerate();
    int count = 0;
    for (int i = 0; i < g_devices.count; ++i) {
        logical_device_t *item = &g_devices.items[i];
        const int dropped = dropped_index_locked(item->key);
        if (dropped < 0) {
            continue;   /* never bridged, or released on purpose */
        }
        if (item_is_tv_remote(item)) {
            continue;   /* the pointer synthesizer, plugged by ctm_bridge_start */
        }
        if (session_index_for_key(item->key) >= 0) {
            dropped_forget_locked(dropped);
            continue;   /* already plugged */
        }
        if (plug_in_item(item)) {
            count++;
            dropped_forget_locked(dropped);
            log_append("ctm glue: re-plugged '%s' (%s) after an outage", item->name,
                       bridge_kind_for_item(item));
        }
    }
    if (count > 0) {
        publish_bt_macs();
    }
    return count;
}

/* Hotplug callback (monitor thread): on any connect/disconnect, re-sync by
 * plugging newly-present recognised controllers. Serialised with the panel via
 * s_dev_mutex; disconnect cleanup is handled by the controller thread, which
 * exits when its HID read fails. */
static void glue_hotplug_cb(void *ud, const ctm_controller_dev_t *dev, int present)
{
    (void) ud; (void) dev; (void) present;
    /* ⓘ Until 2026-09-08 this told the core when a node appeared, so a tone
     * could wait for a fresh cable's audio to "become usable". The core plays
     * the signal twice on a fresh cable instead and needs no clock. */
    if (!s_active) {
        return;
    }
    /* Any device appearing or disappearing used to plug EVERYTHING, without
     * asking whether auto-plug was wanted. Two surprises came from that:
     * a stream opened with every device on a hub bridged at once, and pulling
     * an unrelated hub bridged a controller that had been left alone. Bridging
     * claims a device exclusively, so both took working devices away from the
     * TV without being asked.
     *
     * Noticing a change is still worth doing; acting on it is what the gesture
     * is for. ⓘ So nothing is plugged from here any more. */
}

/* Core bring-up shared by ctm_bridge_start() and the panel entry points:
 * stopSniff worker + agent discovery + enumerate + BT MAC publish. Without
 * this, a controller plugged from the overlay panel alone (no "Use CTM
 * Bridge" setting) attaches over usbip but falls into BT sniff mode --
 * enumerates on the host yet feels dead. Idempotent. */
static bool s_core_up = false;

/* ⭐ Who to tell when a bridged keyboard presses Ctrl+Alt+Shift+O (rhoquinn8217,
 * 2026-09-13). Its grab keeps the keys from Aurora's own shortcuts, so the core
 * finds the shortcut and calls this from the keyboard's input thread. ⓘ Set by
 * the app, which owns the event bus; this library cannot include it. */
void bridge_set_overlay_request(void (*cb)(void))
{
    controller_set_overlay_cb(cb);
}

static void ctm_glue_ensure_core(void)
{
    if (s_core_up) {
        return;
    }
    g_running = true;

    /* Keep every bridged BT controller out of sniff mode for the session. */
    if (!g_stop_sniff_thread_started) {
        if (pthread_create(&g_stop_sniff_thread, NULL, stop_sniff_worker, NULL) == 0) {
            g_stop_sniff_thread_started = true;
            log_append("ctm glue: stopSniff worker started");
        } else {
            log_append("ctm glue: stopSniff worker start failed");
        }
    }

    if (!agent_is_known()) {
        log_append("ctm glue: no agent address yet -- a stream sets it");
    }
    ctm_glue_enumerate();
    // Fill g_bt_macs so the stopSniff worker actually keeps the BT controllers
    // out of sniff mode (the worker reads this list every 500 ms).
    publish_bt_macs();
    s_core_up = true;
}

/* ⭐⭐ THE GESTURE SETTING, HANDED IN RATHER THAN READ.
 *
 * ⛔ This target cannot see the app's headers on purpose -- it is the seam that
 * has to stay thin if a bridge is ever contributed upstream -- so the setting
 * arrives the same way the host address does: the caller reads it and passes
 * it, and nothing here knows what a preference file is.
 *
 * ⓘ The gesture has two halves in two places: the app detects the BRIDGE chord,
 * and the core detects the UNBRIDGE chord, because once bridged a controller's
 * touchpad reports come through the bridge and the app cannot see them. ➡️ One
 * user setting, so both are told; this is the core's half.
 *
 * ⓘ Set once when a stream starts, which is right: it cannot change during one,
 * and leaving a stream by any route unbridges everything anyway. */
void ctm_bridge_set_gesture_enabled(bool enabled)
{
    ctm_gesture_set_enabled(enabled ? 1 : 0);
}

/* ⭐⭐ HOLD OR RELEASE A BRIDGED CONTROLLER'S INPUT.
 *
 * ⓘ Called as the TV's own overlay opens and closes -- see app_ui_open. The
 * core blanks each report while held rather than dropping it, so a button that
 * was down when the overlay opened is actually released in the game instead of
 * staying stuck. ⭐ Audio, rumble, the lightbar and the unbridge chord all keep
 * working throughout.
 *
 * ⓘ Safe with nothing bridged: the flag is only read in the relay path, and
 * with no session there is nothing to relay. */
/* ⭐ WHICH SIGNALS THIS SIDE MAY MAKE, and whether the microphone is captured.
 *
 * ⓘ Handed in rather than read here: this target cannot see the app's headers
 * on purpose, and that seam is what has to stay thin if a bridge is ever
 * contributed upstream. Same shape as the host address and the gesture switch. */
void ctm_bridge_set_signals(bool light, bool rumble, bool tone)
{
    ctm_signals_set_enabled(light ? 1 : 0, rumble ? 1 : 0, tone ? 1 : 0);
}

void ctm_bridge_set_mic_capture(bool on)
{
    ctm_mic_capture_set_enabled(on ? 1 : 0);
}

void ctm_bridge_set_input_held(bool held)
{
    ctm_input_set_held(held ? 1 : 0);
}

bool ctm_bridge_start(void)
{
    if (s_active) {
        return true;
    }
    ctm_glue_ensure_core();
    /* A new bridge owes nothing to the last one's outages. */
    pthread_mutex_lock(&s_dev_mutex);
    s_dropped_count = 0;
    pthread_mutex_unlock(&s_dev_mutex);

    /* ⭐⭐ START THE AGENT PROBE WHEN THE BRIDGE COMES UP, not when something is
     * first plugged.
     *
     * ⛔ THE FAULT: ctm_bridge_gesture_init is called from plug_in_item -- the
     * PLUG path -- so the probe only ever started once a device had actually
     * been bridged. ⚠️ With the USB server down you cannot bridge anything, so
     * the probe never started, so the panel never learned it was down and
     * showed its default instead. ➡️ It was worst exactly when it mattered most.
     *
     * ⓘ Found 2026-08-20 after the panel read ONLINE with the listener stopped:
     * the log had no `agent probe thread started` line for that run at all.
     *
     * ⓘ Safe to call more than once -- it starts a worker only if one is not
     * already running. */
    ctm_bridge_gesture_init();

    /* The TV pointer used to be bridged here unconditionally, whatever the
     * auto-plug setting said -- the third path that claimed a device without
     * being asked, and the one that kept appearing in the host's log as
     * ctm-remote.
     *
     * It is also redundant now. It existed because enabling the bridge put the
     * session into view-only, which silenced moonlight's own mouse; bridging
     * the pointer was the only way to get one to the host. That was fixed on
     * 2026-08-05 -- moonlight's mouse works with the bridge on -- so this was
     * claiming a device for a job already done.
     *
     * The overlay row still plugs it deliberately for anyone who wants it. */

    s_active = true;

    /* Watch for controllers connected/disconnected mid-stream and auto-plug them. */
    if (s_monitor == NULL) {
        s_monitor = ctm_monitor_start(glue_hotplug_cb, NULL);
        log_append(s_monitor ? "ctm glue: hotplug monitor started"
                             : "ctm glue: hotplug monitor failed to start");
    }
    return true;
}

/* The listing itself. Enumerates and fills; brings NOTHING up. */
static int glue_list_locked_body(ctm_bridge_dev_t *out, int max)
{
    pthread_mutex_lock(&s_dev_mutex);
    ctm_glue_enumerate();
    int n = 0;
    for (int i = 0; i < g_devices.count && n < max; ++i) {
        logical_device_t *item = &g_devices.items[i];
        /* Skip phantom / unconnected entries the enumerate surfaces (no VID:PID).
         * (Pre-existing in the shared enumerate; we just don't show the junk.) */
        if (item->vid[0] == '\0' || item->pid[0] == '\0') {
            continue;
        }
        const char *kind = bridge_kind_for_item(item);
        out[n].index = i;   /* g_devices index, stable for plug/unplug this call */
        snprintf(out[n].name, sizeof(out[n].name), "%s", item->name);
        snprintf(out[n].vid, sizeof(out[n].vid), "%s", item->vid);
        snprintf(out[n].pid, sizeof(out[n].pid), "%s", item->pid);
        snprintf(out[n].kind, sizeof(out[n].kind), "%s", kind ? kind : "hid");
        snprintf(out[n].bus, sizeof(out[n].bus), "%s", item->bus);
        snprintf(out[n].mac, sizeof(out[n].mac), "%s", item->mac);
        snprintf(out[n].serial, sizeof(out[n].serial), "%s", item->serial);
        out[n].controller = item_is_controller(item);
        snprintf(out[n].type, sizeof(out[n].type), "%s", item_type_label(item));
        /* The first backing node is the one the bridge plugs, and the device it
         * belongs to is read from it. */
        out[n].node[0] = '\0';
        out[n].group[0] = '\0';
        out[n].device_name[0] = '\0';
        out[n].usb_serial[0] = '\0';
        out[n].iface = -1;
        for (int k = 0; k < item->device_count; ++k) {
            int j = item->device_indices[k];
            if (j >= 0 && j < g_scan.count && g_scan.devices[j].node[0]) {
                const device_info_t *dev = &g_scan.devices[j];
                snprintf(out[n].node, sizeof(out[n].node), "%s", dev->node);
                snprintf(out[n].group, sizeof(out[n].group), "%s", dev->group);
                snprintf(out[n].device_name, sizeof(out[n].device_name), "%s", dev->device_name);
                snprintf(out[n].usb_serial, sizeof(out[n].usb_serial), "%s", dev->usb_serial);
                out[n].iface = dev->iface_num;
                break;
            }
        }
        if (!out[n].group[0]) {
            snprintf(out[n].group, sizeof(out[n].group), "item:%s", item->key);
        }
        /* The TV's own Magic Remote row IS the pointer synthesizer (raw relay
         * of its LG-vendor descriptor would code-10 on Windows). */
        out[n].plugged = item_is_tv_remote(item) ? ctm_tv_pointer_active()
                                                 : (session_index_for_key(item->key) >= 0);
        n++;
    }
    pthread_mutex_unlock(&s_dev_mutex);
    return n;
}

int ctm_bridge_list(ctm_bridge_dev_t *out, int max)
{
    if (out == NULL || max <= 0) {
        return 0;
    }
    ctm_glue_ensure_core();
    return glue_list_locked_body(out, max);
}

/* ⭐⭐ THE SAME LIST WITHOUT WAKING ANYTHING (T-135, 2026-09-08).
 *
 * ⛔ ctm_bridge_list() calls ctm_glue_ensure_core(), which starts the stopSniff
 * worker: work that showing a list is no reason to start. ⓘ Until 2026-09-15
 * it also ran a broadcast probe for an agent that cannot exist before a host is
 * chosen; that probe is gone, and the address now only ever comes from a
 * stream starting.
 * ⚠️ The settings pane lists devices with no stream running, so it must not go
 * through that door.
 * ➡️ This enumerates and reports, and nothing else. */
int ctm_bridge_list_quiet(ctm_bridge_dev_t *out, int max)
{
    if (out == NULL || max <= 0) {
        return 0;
    }
    return glue_list_locked_body(out, max);
}

/* The row behind a node in the last enumeration, or NULL. Caller holds
 * s_dev_mutex. */
static logical_device_t *item_for_node_locked(const char *node)
{
    for (int i = 0; i < g_devices.count; ++i) {
        logical_device_t *item = &g_devices.items[i];
        for (int k = 0; k < item->device_count; ++k) {
            int j = item->device_indices[k];
            if (j < 0 || j >= g_scan.count) {
                continue;
            }
            if (strcmp(g_scan.devices[j].node, node) == 0) {
                return item;
            }
        }
    }
    return NULL;
}

/* Is the controller behind this hidraw node already bridged?
 *
 * The same lookup a plug does, asked as a question instead. It exists because
 * a plug attempt cannot answer it: plugging something already plugged comes
 * back false, exactly like a genuine failure, so the gesture watcher showed a
 * refusal for something that had not failed.
 *
 * Answered here rather than in the bridge core deliberately -- the behaviour
 * being fixed is this app's, and the core is the part heading upstream.
 *
 * When: the gesture watcher at the moment it would otherwise plug, and every
 * 500 ms for each controller it bridged (PLUG_CHECK_MS).
 *
 * ⛔⛔ THE LAST SCAN FIRST, AND A NEW ONE ONLY FOR A NODE IT DOES NOT HAVE
 * (U5s, 2026-09-14, build 329). This used to enumerate on every call, and a
 * scan of fourteen parts took 0.25 to 0.85 s on the interface thread: with a
 * DualShock 4 and a GameSir's pad bridged through the gesture, the overlay and
 * the USB Bridge panel barely moved, and releasing those two -- not the Razer,
 * which is plugged directly and never checked -- is what freed it.
 * ⭐ Whether a device is bridged is the session table's answer, and that is
 * live. The scan only maps the node to its row's key, which a connected device
 * keeps, so a stale scan cannot make a live bridge look gone -- which also
 * removes the "a scan blinked" misses PLUG_MISSES exists for. */
bool ctm_bridge_node_is_plugged(const char *node)
{
    if (!node || !node[0]) {
        return false;
    }
    ctm_glue_ensure_core();
    pthread_mutex_lock(&s_dev_mutex);
    logical_device_t *item = item_for_node_locked(node);
    if (item == NULL) {
        ctm_glue_enumerate();
        item = item_for_node_locked(node);
    }
    const bool plugged = item != NULL && session_index_for_key(item->key) >= 0;
    pthread_mutex_unlock(&s_dev_mutex);
    return plugged;
}

/* Is this node a Bluetooth controller rather than a wired one?
 *
 * Asked so the app can leave the confirmation signals to the bridge core on
 * Bluetooth, where it builds the controller's reports itself and can do the
 * light, the pulse and the tone together. Running the app's own patterns as
 * well produces two overlapping signals with different characters, which
 * reads as a controller that does not know what it is doing.
 *
 * The distinction is already in the device kind -- "ds5" is the Bluetooth
 * DualSense, "ds5_usb" the wired one -- so nothing new has to be worked out
 * to answer it. */
/* The one switch, asked through the glue because the app cannot see the core's
 * headers directly. One definition, both sides. */
bool ctm_bridge_signals_enabled(void)
{
    return CTM_SIGNALS_ENABLED ? true : false;
}

/* Will the bridge core signal this controller itself?
 *
 * ⭐ THE RIGHT QUESTION, replacing "is this Bluetooth". Both transports have a
 * rich signal and a coarse SDL fallback, and running both gives two buzzes of
 * different characters -- which reads as a controller that does not know what
 * it is doing. What matters is not which cable it is but whether the rich one
 * is about to play.
 *
 * Bluetooth: always. The core builds the controller's own reports -- light,
 * felt pulse and tone together -- and needs nothing but a device node.
 *
 * Wired: only once a session exists, because that is what opens the speaker
 * the tone and pulse are written to. So a refused plug on a cable correctly
 * falls back, and a successful one correctly does not. */
/* Signal a REFUSED plug richly, on whichever transport this is.
 *
 * Returns true if it did, false if the caller should fall back to SDL.
 *
 * ⭐ A refusal is the moment the user most needs telling, and until now it got
 * the coarsest signal we had -- because a failed plug leaves no session to
 * play through. Neither transport actually needs one:
 *
 *   Bluetooth: the signal is bytes to a device node, and the node is still
 *     there. Nothing about it depends on the host, which is the point, since
 *     the host being unreachable is WHY the plug failed.
 *
 *   Wired: the sound card exists because the controller is plugged into the
 *     TV, not because a bridge succeeded. ⚠️ But with two plugged in there is
 *     no telling which card is which, so that case declines and falls back. */
/* The bridge kind of the row behind a node, or NULL. Caller holds s_dev_mutex
 * and has enumerated. */
static const char *kind_for_node_locked(const char *node)
{
    const logical_device_t *item = item_for_node_locked(node);
    return item != NULL ? bridge_kind_for_item(item) : NULL;
}

bool bridge_identity_usable(const char *s)
{
    return identity_usable(s) != 0;
}

bool bridge_identity_same(const char *a, const char *b)
{
    return identity_same(a, b) != 0;
}

bool bridge_identity_mac_shaped(const char *s)
{
    return identity_mac_shaped(s) != 0;
}

bool ctm_bridge_signal_refused(const char *node)
{
    if (!ctm_bridge_signals_enabled() || !node || !node[0]) return false;
    char kind[16] = "";
    ctm_glue_ensure_core();
    pthread_mutex_lock(&s_dev_mutex);
    ctm_glue_enumerate();
    const char *k = kind_for_node_locked(node);
    if (k) {
        snprintf(kind, sizeof(kind), "%s", k);
    }
    pthread_mutex_unlock(&s_dev_mutex);

    if (strcmp(kind, "ds5") == 0 || strcmp(kind, "ds5e") == 0) {
        return ctm_signal_refused_bt(node) == 0;
    }
    /* ⛔ The wired signal plays through a DualSense's sound card, taken by
     * elimination when the node's own cannot be told apart. For any other
     * controller that meant the refusal played on whichever DualSense was
     * free. ➡️ Anything that is not a DualSense returns false, and gets the SDL
     * buzz, which reaches every pad. */
    if (strcmp(kind, "ds5_usb") == 0 || strcmp(kind, "ds5e_usb") == 0) {
        return ctm_signal_wired_no_session(node, 2 /* BTSIG_REFUSED */) == 0;
    }
    return false;
}

/* Does the CORE have a connected signal for the pad on this node? (T-212)
 *
 * ⓘ The core answers, from the same two ops fields its own branch uses. The
 * device it is asked about carries this NODE's path, because a type's matches()
 * can depend on it -- the Bluetooth Xbox type takes hidraw nodes and the xpad
 * type takes js/event ones. */
static bool core_signals_node_locked(const char *node)
{
    const logical_device_t *item = item_for_node_locked(node);
    if (item == NULL) return false;

    ctm_controller_dev_t dev;
    memset(&dev, 0, sizeof dev);
    snprintf(dev.vid, sizeof dev.vid, "%s", item->vid);
    snprintf(dev.pid, sizeof dev.pid, "%s", item->pid);
    /* ⛔⛔ THE LABEL, NOT THE KERNEL NUMBER. A type's matches() compares
     * dev.bus against "USB" and "BT", while item->bus holds sysfs's "0003" or
     * "0005". ⚠️ Passed raw, EVERY DualSense matcher failed, the registry fell
     * through to generic, and the TV pulsed a pad the core was about to sing to
     * -- felt on the monitor 2026-09-18 as a rumble with no sound, then the
     * core's own tone and rumble seconds later. ⭐ bus_label() is the conversion
     * the real bridge path has always used (ui_bridge.c, plug_in_item). */
    snprintf(dev.bus, sizeof dev.bus, "%s", bus_label(item->bus));
    snprintf(dev.name, sizeof dev.name, "%s", item->name);
    snprintf(dev.mac, sizeof dev.mac, "%s", item->mac);
    snprintf(dev.serial, sizeof dev.serial, "%s", item->serial);
    snprintf(dev.driver, sizeof dev.driver, "%s", item->driver);
    snprintf(dev.path, sizeof dev.path, "%s", node);
    return ctm_controller_will_signal_connect(&dev) != 0;
}

bool ctm_bridge_node_signals_itself(const char *node)
{
    if (!ctm_bridge_signals_enabled()) return false;

    /* ⛔⛔ T-212: ASK WHETHER THE CORE WILL, NOT WHETHER THE PAD IS PLUGGED.
     *
     * This said "plugged, so the core has it" -- and for a Bluetooth Xbox pad
     * or a Bluetooth DS4 the core has NO connected signal at all, so the TV
     * stood aside for nobody and the bridge was silent. Measured 2026-09-18:
     * both pads logged "NOT SENT BY US -- the core claims the signal here",
     * and neither pad's core log held a signal line.
     *
     * ⭐ A pad the core cannot signal now falls through to the TV's own pulse,
     * which is what the caller does when this is false. */
    ctm_glue_ensure_core();
    pthread_mutex_lock(&s_dev_mutex);
    ctm_glue_enumerate();
    const bool core_has_one = core_signals_node_locked(node);
    pthread_mutex_unlock(&s_dev_mutex);
    /* ⏱️ ONE ENUMERATION, NOT THREE. This ended in
     * `ctm_bridge_node_is_bluetooth(node) || ctm_bridge_node_is_plugged(node)`,
     * and each of those takes the lock and enumerates every device again.
     * Both were only ever asking "is this pad really here", which the core's
     * answer above already settles -- it returns false for a node it cannot
     * find. ⚠️ The extra passes were measured at 5079ms on the monitor
     * 2026-09-18: five seconds between the bridge and the pulse confirming it. */
    return core_has_one;
}

bool ctm_bridge_node_is_bluetooth(const char *node)
{
    if (!node || !node[0]) {
        return false;
    }
    ctm_glue_ensure_core();
    pthread_mutex_lock(&s_dev_mutex);
    ctm_glue_enumerate();
    /* ⛔⛔ THE BUS SAYS THIS, NOT A LIST OF TWO KINDS.
     *
     * It tested for "ds5" and "ds5e" and nothing else, from when a
     * DualSense was the only pad that could be bridged over Bluetooth. Every
     * other Bluetooth pad has answered WIRED ever since -- the T-212 run
     * logged a Bluetooth Xbox pad as `transport=wired` on 2026-09-18.
     *
     * ⭐ item->bus is sysfs's bustype and bus_label() turns "0005" into
     * "BT", which is the same conversion the bridge path uses to decide a
     * pad's type. So this now answers for any pad, including ones no type
     * exists for yet. */
    const logical_device_t *item = item_for_node_locked(node);
    const bool bt = item != NULL && strcmp(bus_label(item->bus), "BT") == 0;
    pthread_mutex_unlock(&s_dev_mutex);
    return bt;
}

bool ctm_bridge_plug_node(const char *node)
{
    if (!s_active) return false;
    ctm_glue_ensure_core();
    pthread_mutex_lock(&s_dev_mutex);
    bool ok = plug_in_by_node(node);
    pthread_mutex_unlock(&s_dev_mutex);
    return ok;
}

bool ctm_bridge_plug_index(int index)
{
    ctm_glue_ensure_core();
    pthread_mutex_lock(&s_dev_mutex);
    bool ok = false;
    if (index >= 0 && index < g_devices.count) {
        logical_device_t *item = &g_devices.items[index];
        if (item_is_tv_remote(item)) {
            ok = ctm_tv_pointer_plug();
        } else {
            ok = plug_in_item(item);
            if (ok) {
                publish_bt_macs();   /* keep the newly-plugged BT controller out of sniff mode */
            }
        }
        log_append("ctm glue: manual plug '%s' (%s) -> %s", item->name,
                   bridge_kind_for_item(item), ok ? "ok" : "failed");
    }
    pthread_mutex_unlock(&s_dev_mutex);
    return ok;
}

/* ⭐⭐ RELEASE A CONTROLLER WHOSE HOST HAS GONE. T-127, 2026-08-23.
 *
 * ⛔ THE FAULT: close the listener's window and the controller stayed claimed by
 * a host that no longer exists -- no speaker, no triggers, no microphone, and
 * nothing on screen saying why. **The only way out was knowing to press
 * Release.**
 *
 * ⓘ THE CORE ALREADY GIVES UP. After fifteen seconds of a host that will not
 * answer it stops retrying and reports `host_gone` in its status. ⛔ But it
 * CANNOT release itself: retiring the emulated pad, restoring moonlight's input,
 * updating the panel row and pulsing the light are all app-side, and
 * `stop_session()` joins the session thread -- **which that thread cannot do to
 * itself.** ➡️ So the core raises a flag and this reaps it, from a safe thread.
 *
 * ⭐ WHY THIS NEEDS NOTHING ELSE: `plug_key_is_set()` asks whether a SESSION
 * EXISTS rather than reading a stored flag -- its own comment says so, *"it
 * mirrors live pointer state rather than a persisted flag so a stale 'plugged'
 * cannot strand the row"*. ➡️ **Once the session is stopped the device reports
 * unplugged, and the gesture's existing watcher does the whole app-side release
 * on its own.**
 *
 * ⚠️ DELIBERATELY CHEAP. It walks the session table -- an in-memory array of at
 * most a handful of entries -- and does NOT enumerate. ⓘ The gesture tick calls
 * this every frame, and `ctm_glue_enumerate()` has been measured at 5,144 ms on
 * a C3. **Anything that enumerated here would be a freeze of its own.**
 *
 * ➡️ Returns the number released, so the caller can log it once rather than per
 * frame. */
int ctm_bridge_reap_gone_hosts(void)
{
    int reaped = 0;
    char gone[MAX_SESSIONS][96];
    int gone_count = 0;

    pthread_mutex_lock(&s_dev_mutex);
    /* ⛔ Under the core's table lock too, and past a stopping entry: the chord's
     * release worker tears controllers down without s_dev_mutex, so this read a
     * controller's status while it could be freed. */
    pthread_mutex_lock(&g_sessions_mutex);
    for (int i = 0; i < g_session_count && gone_count < MAX_SESSIONS; ++i) {
        ctm_controller_t *c = g_sessions[i].controller;
        if (!c || g_sessions[i].stopping) continue;
        ctm_controller_status_t st;
        ctm_controller_get_status(c, &st);
        if (!st.host_gone) continue;
        snprintf(gone[gone_count], sizeof(gone[gone_count]), "%s", g_sessions[i].key);
        ++gone_count;
    }
    pthread_mutex_unlock(&g_sessions_mutex);
    /* ⚠️ Collected first, stopped second. `stop_session()` removes entries from
     * the very table being walked, so stopping inside the loop would skip the
     * entry that shifts down into the current index. */
    for (int i = 0; i < gone_count; ++i) {
        log_append("ctm glue: host gone -- releasing '%s'", gone[i]);
        dropped_remember_locked(gone[i]);   /* a reconnect may put it back */
        stop_session(gone[i]);
        ++reaped;
    }
    pthread_mutex_unlock(&s_dev_mutex);
    return reaped;
}

void ctm_bridge_unplug_index(int index)
{
    pthread_mutex_lock(&s_dev_mutex);
    if (index >= 0 && index < g_devices.count) {
        if (item_is_tv_remote(&g_devices.items[index])) {
            ctm_tv_pointer_unplug();
        } else {
            stop_session(g_devices.items[index].key);
        }
        log_append("ctm glue: manual unplug '%s'", g_devices.items[index].name);
    }
    pthread_mutex_unlock(&s_dev_mutex);
}

bool ctm_bridge_pointer_active(void)
{
    return ctm_tv_pointer_active();
}

void ctm_bridge_pointer_feed(int x, int y, int w, int h, unsigned buttons, int wheel)
{
    ctm_hostmouse_feed(x, y, w, h, buttons, wheel);
}

void ctm_bridge_pointer_feed_key(unsigned hid_usage, bool down)
{
    ctm_hostmouse_feed_key((uint8_t) hid_usage, down);
}

int ctm_bridge_plug_all(void)
{
    ctm_glue_ensure_core();
    pthread_mutex_lock(&s_dev_mutex);
    int count = glue_plug_all_locked();
    pthread_mutex_unlock(&s_dev_mutex);
    return count;
}

void ctm_bridge_unplug_all(void)
{
    pthread_mutex_lock(&s_dev_mutex);
    release_local_sessions_on_exit();
    pthread_mutex_unlock(&s_dev_mutex);
    log_append("ctm glue: unplugged all");
}

bool ctm_bridge_get_settings(int index, ctm_bridge_settings_t *out)
{
    if (out == NULL) {
        return false;
    }
    pthread_mutex_lock(&s_dev_mutex);
    bool ok = false;
    if (index >= 0 && index < g_devices.count) {
        tv_bridge_worker_settings_t *s = settings_for_item(&g_devices.items[index]);
        if (s != NULL) {
            out->kind = (int) s->kind;
            out->audio_mode = (int) s->audio_mode;
            out->latency_ms = (int) s->latency_ms;
            out->haptics_gain_centi = (int) s->haptics_gain_centi;
            out->headset_volume_percent = (int) s->headset_volume_percent;
            out->speaker_volume_percent = (int) s->speaker_volume_percent;
            out->ds5_patch_high = (int) s->ds5_patch_high_nibble;
            out->ds5_patch_low = (int) s->ds5_patch_low_nibble;
            out->ds5_patch2_high = (int) s->ds5_patch2_high_nibble;
            out->ds5_patch2_low = (int) s->ds5_patch2_low_nibble;
            ok = true;
        }
    }
    pthread_mutex_unlock(&s_dev_mutex);
    return ok;
}

void ctm_bridge_set_settings(int index, const ctm_bridge_settings_t *in)
{
    if (in == NULL) {
        return;
    }
    pthread_mutex_lock(&s_dev_mutex);
    if (index >= 0 && index < g_devices.count) {
        tv_bridge_worker_settings_t *s = settings_for_item(&g_devices.items[index]);
        if (s != NULL) {
            s->audio_mode = (tv_bridge_audio_mode_t) in->audio_mode;
            s->latency_ms = (unsigned int) in->latency_ms;
            s->haptics_gain_centi = (unsigned int) in->haptics_gain_centi;
            s->headset_volume_percent = (unsigned int) in->headset_volume_percent;
            s->speaker_volume_percent = (unsigned int) in->speaker_volume_percent;
            s->ds5_patch_high_nibble = (unsigned int) in->ds5_patch_high;
            s->ds5_patch_low_nibble = (unsigned int) in->ds5_patch_low;
            s->ds5_patch2_high_nibble = (unsigned int) in->ds5_patch2_high;
            s->ds5_patch2_low_nibble = (unsigned int) in->ds5_patch2_low;
            apply_settings_to_session(&g_devices.items[index]);
        }
    }
    pthread_mutex_unlock(&s_dev_mutex);
}

void ctm_bridge_stop(void)
{
    if (!s_active && !s_core_up) {
        return;
    }
    /* Stop hotplug first: joins the monitor thread (do this WITHOUT holding
     * s_dev_mutex so an in-flight callback can finish) before tearing down. */
    if (s_monitor) {
        ctm_monitor_stop(s_monitor);
        s_monitor = NULL;
    }
    release_local_sessions_on_exit();
    pthread_mutex_lock(&s_dev_mutex);
    s_dropped_count = 0;
    pthread_mutex_unlock(&s_dev_mutex);
    g_running = false;
    if (g_stop_sniff_thread_started) {
        pthread_join(g_stop_sniff_thread, NULL);
        g_stop_sniff_thread_started = false;
    }
    log_append("ctm glue: bridge stopped");
    s_active = false;
    s_core_up = false;
}

bool ctm_bridge_active(void)
{
    return s_active;
}

void ctm_bridge_status(char *out, size_t out_len)
{
    if (out == NULL || out_len == 0) {
        return;
    }
    size_t n = 0;
    n += (size_t) snprintf(out + n, out_len - n, "Bridge: %s\n", s_active ? "active" : "inactive");
    if (n >= out_len) return;
    n += (size_t) snprintf(out + n, out_len - n, "Agent: %s\n",
                           (g_agent_online && g_agent_host[0]) ? g_agent_host : "not found");
    if (n >= out_len) return;
    pthread_mutex_lock(&g_sessions_mutex);
    n += (size_t) snprintf(out + n, out_len - n, "Bridged controllers: %d\n", g_session_count);
    for (int i = 0; i < g_session_count && n < out_len; ++i) {
        n += (size_t) snprintf(out + n, out_len - n, "  - %s [%s]\n",
                               g_sessions[i].key, g_sessions[i].busid);
    }
    pthread_mutex_unlock(&g_sessions_mutex);
}

/* Is the USB server answering? ⭐ Separate from its address, which is known
 * whether or not anything is listening at it. */
/* ⭐ Ask for a fresh reading. ⓘ The panel calls this as it opens, so a listener
 * started mid-stream is noticed at once rather than up to ten seconds later. */
void ctm_bridge_agent_recheck(void)
{
    ctm_agent_probe_soon();
}

/* ⭐ Has anything actually reached a verdict yet? ⓘ Separate from the verdict
 * itself, so the panel can say "not known" instead of asserting "offline"
 * before a single probe has completed. */
bool ctm_bridge_agent_probed(void)
{
    return g_agent_probed;
}

bool ctm_bridge_agent_online(void)
{
    return g_agent_online != 0;
}

void ctm_bridge_agent(char *out, size_t out_len)
{
    if (out == NULL || out_len == 0) {
        return;
    }
    /* ⛔ THE ADDRESS IS REPORTED EITHER WAY. It used to be replaced by "offline",
     * which made the panel drop the IP when the server went down -- and the
     * address does not change just because nothing is answering at it. ⓘ rhoquinn8217,
     * 2026-08-20. ➡️ Callers ask ctm_bridge_agent_online() for the state. */
    snprintf(out, out_len, "%s", g_agent_host[0] ? g_agent_host : "not set");
}
