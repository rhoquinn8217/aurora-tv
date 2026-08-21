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

static bool s_active = false;

void ctm_bridge_set_host(const char *host, int port)
{
    ctm_bridge_set_agent_host(host, port);
}
/* Auto-plug ALL recognised controllers on stream start.
 *
 * Off. On a TV with one controller, plugging everything is a convenience. On a
 * hub carrying a keyboard, a mouse and a controller it takes all of them --
 * bridging claims a device exclusively, so the keyboard and mouse stop working
 * on the TV, and every session opens with a cascade of connect chimes and a
 * cleanup. Observed on three TVs.
 *
 * Nothing needs it now: a controller is bridged by holding two fingers on its
 * touchpad and pressing, and the overlay panel still plugs anything by hand. */
static bool s_autoplug = false;

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

/* Plug every recognised, not-yet-plugged controller. Caller MUST hold s_dev_mutex. */
static int glue_plug_all_locked(void)
{
    ctm_glue_enumerate();
    int count = 0;
    for (int i = 0; i < g_devices.count; ++i) {
        logical_device_t *item = &g_devices.items[i];
        const char *kind = bridge_kind_for_item(item);
        if (kind == NULL) {
            continue;
        }
        if (strcmp(kind, "hid") == 0 &&
            (item_is_tv_remote(item) || !item_is_mouse_or_keyboard(item))) {
            /* Remote = the pointer synthesizer (plugged by ctm_bridge_start,
             * never raw-relayed); other generic HID auto-plugs only when it
             * is a real mouse/keyboard — vendor exotics stay manual. */
            continue;
        }
        if (session_index_for_key(item->key) >= 0) {
            continue;   /* already plugged */
        }
        if (plug_in_item(item)) {
            count++;
            log_append("ctm glue: plugged '%s' (%s)", item->name, kind);
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
    /* ⭐ A CABLE'S AUDIO IS NOT READY THE MOMENT IT APPEARS -- measured at
     * several seconds -- so the core is told when each node showed up and works
     * out for itself whether a tone should wait. ⓘ Before the s_active check on
     * purpose: the clock should start when the device appears, whatever the
     * bridge happens to be doing. */
    if (present && dev && dev->path[0]) {
        ctm_feedback_note_appeared(dev->path);
    }
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
     * is for. */
    if (!s_autoplug) {
        return;
    }
    pthread_mutex_lock(&s_dev_mutex);
    glue_plug_all_locked();
    pthread_mutex_unlock(&s_dev_mutex);
}

/* Core bring-up shared by ctm_bridge_start() and the panel entry points:
 * stopSniff worker + agent discovery + enumerate + BT MAC publish. Without
 * this, a controller plugged from the overlay panel alone (no "Use CTM
 * Bridge" setting) attaches over usbip but falls into BT sniff mode --
 * enumerates on the host yet feels dead. Idempotent. */
static bool s_core_up = false;

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

    if (!discover_agent_once()) {
        log_append("ctm glue: no CTM agent found on the network");
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


    if (s_autoplug) {
        int count = ctm_bridge_plug_all();
        log_append("ctm glue: auto-plugged %d controller(s)", count);
    } else {
        log_append("ctm glue: auto-plug off, use the overlay panel to plug a controller");
    }

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
    if (s_autoplug && ctm_tv_pointer_plug()) {
        log_append("ctm glue: TV pointer bridged");
    }

    s_active = true;

    /* Watch for controllers connected/disconnected mid-stream and auto-plug them. */
    if (s_monitor == NULL) {
        s_monitor = ctm_monitor_start(glue_hotplug_cb, NULL);
        log_append(s_monitor ? "ctm glue: hotplug monitor started"
                             : "ctm glue: hotplug monitor failed to start");
    }
    return true;
}

int ctm_bridge_list(ctm_bridge_dev_t *out, int max)
{
    if (out == NULL || max <= 0) {
        return 0;
    }
    ctm_glue_ensure_core();
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
        /* The first backing node is the one the bridge plugs. */
        out[n].node[0] = '\0';
        for (int k = 0; k < item->device_count; ++k) {
            int j = item->device_indices[k];
            if (j >= 0 && j < g_scan.count && g_scan.devices[j].node[0]) {
                snprintf(out[n].node, sizeof(out[n].node), "%s", g_scan.devices[j].node);
                break;
            }
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
 * When: the gesture watcher, once, at the moment it would otherwise plug. */
bool ctm_bridge_node_is_plugged(const char *node)
{
    if (!node || !node[0]) {
        return false;
    }
    ctm_glue_ensure_core();
    pthread_mutex_lock(&s_dev_mutex);
    ctm_glue_enumerate();
    bool plugged = false;
    for (int i = 0; i < g_devices.count && !plugged; ++i) {
        logical_device_t *item = &g_devices.items[i];
        for (int k = 0; k < item->device_count; ++k) {
            int j = item->device_indices[k];
            if (j < 0 || j >= g_scan.count) {
                continue;
            }
            if (strcmp(g_scan.devices[j].node, node) != 0) {
                continue;
            }
            plugged = (session_index_for_key(item->key) >= 0);
            break;
        }
    }
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
bool ctm_bridge_signal_refused(const char *node)
{
    if (!ctm_bridge_signals_enabled()) return false;
    if (ctm_bridge_node_is_bluetooth(node)) {
        return ctm_signal_refused_bt(node) == 0;
    }
    return ctm_signal_wired_no_session(node, 2 /* BTSIG_REFUSED */) == 0;
}

bool ctm_bridge_node_signals_itself(const char *node)
{
    if (!ctm_bridge_signals_enabled()) return false;
    if (ctm_bridge_node_is_bluetooth(node)) return true;
    return ctm_bridge_node_is_plugged(node);
}

bool ctm_bridge_node_is_bluetooth(const char *node)
{
    if (!node || !node[0]) {
        return false;
    }
    ctm_glue_ensure_core();
    pthread_mutex_lock(&s_dev_mutex);
    ctm_glue_enumerate();
    bool bt = false;
    for (int i = 0; i < g_devices.count && !bt; ++i) {
        logical_device_t *item = &g_devices.items[i];
        for (int k = 0; k < item->device_count; ++k) {
            int j = item->device_indices[k];
            if (j < 0 || j >= g_scan.count) {
                continue;
            }
            if (strcmp(g_scan.devices[j].node, node) != 0) {
                continue;
            }
            const char *kind = bridge_kind_for_item(item);
            bt = (strcmp(kind, "ds5") == 0) || (strcmp(kind, "ds5e") == 0);
            break;
        }
    }
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
    n += (size_t) snprintf(out + n, out_len - n, "Bridged controllers: %d\n", g_session_count);
    for (int i = 0; i < g_session_count && n < out_len; ++i) {
        n += (size_t) snprintf(out + n, out_len - n, "  - %s [%s]\n",
                               g_sessions[i].key, g_sessions[i].busid);
    }
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
