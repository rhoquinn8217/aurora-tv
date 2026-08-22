#ifndef CTM_BRIDGE_GLUE_H
#define CTM_BRIDGE_GLUE_H

/* Thin moonlight-facing facade over the embedded CTM bridge core. moonlight calls
 * only these three functions; everything else (agent discovery, enumeration,
 * controller bridging, stopSniff keep-alive) stays inside the ctmbridge lib so
 * the core's headers don't leak into moonlight-lib. */

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Tell the bridge where the Windows CTM agent is, skipping discovery. When: the
 * host app already knows the address because it is streaming from that same
 * machine. Call before ctm_bridge_start(). Passing NULL or "" restores
 * discovery. Port 0 keeps the default agent port.
 *
 * Without this, the agent is found by a UDP broadcast probe, which never leaves
 * the local network -- so a TV streaming from a host elsewhere can never find
 * it. */
void ctm_bridge_set_host(const char *host, int port);

/* Start the bridge: discover the Windows CTM agent, enumerate controllers,
 * auto-plug the first one we recognise, and run the stopSniff keep-alive.
 * Idempotent (a second call while active is a no-op). Returns true if a
 * controller was bridged. */
/* ⛔ EXPERIMENTAL: Bluetooth microphone capture. Default off in the core, so
 * not calling this leaves it off. Call before ctm_bridge_start(). */
void ctm_bridge_set_capture_enabled(bool on);

bool ctm_bridge_start(void);

/* Stop bridging: unplug all sessions and stop the keep-alive thread. Idempotent. */
/* Switch the UNBRIDGE chord on or off. ⭐ The app owns the setting and the
 * bridge half of the gesture; this passes the other half to the core. */
void ctm_bridge_set_gesture_enabled(bool enabled);

/* Hold a bridged controller's input while the TV's overlay is open, so
 * navigating the panel does not also play the game. ⭐ Reports are blanked, not
 * dropped -- a held button is released rather than left stuck. */
/* Which confirmation signals the bridge may make, and whether it captures the
 * controller's microphone. ⭐ Set when a stream starts. */
void ctm_bridge_set_signals(bool light, bool rumble, bool tone);

void ctm_bridge_set_mic_capture(bool on);

void ctm_bridge_set_input_held(bool held);

void ctm_bridge_stop(void);

/* True while the bridge is active. */
bool ctm_bridge_active(void);

/* Write a short human-readable status (active state, agent, bridged controllers)
 * into out (NUL-terminated). For the on-stream CTM overlay panel. */
void ctm_bridge_status(char *out, size_t out_len);

/* One detected device, for the overlay's manual plug list. */
typedef struct {
    int index;      /* opaque device index; pass to ctm_bridge_plug/unplug_index */
    char name[128];
    char vid[8];
    char pid[8];
    char kind[8];   /* "ds5" / "ds4" / "xbox" / "puck" / "hid" */
    char bus[8];    /* "USB" / "BT" */
    char mac[24];   /* BT MAC (e.g. "58:10:31:..."), empty for USB */
    /* ⭐ The hidraw node, which is the identity everything else in this project
     * speaks -- SDL returns it as the controller path on webOS, and the bridge
     * plugs by it. `index` above is only valid until the next enumerate; this
     * is not. */
    char node[64];
    bool plugged;
} ctm_bridge_dev_t;

/* Write the discovered Windows agent host (or "offline") into out (NUL-terminated).
 * For the overlay header. */
/* Is the USB server answering? ⓘ Its address is reported by ctm_bridge_agent()
 * whether or not it is. */
/* Ask for a fresh reading of whether the USB server is answering. ⭐ Returns at
 * once; the answer lands on the next refresh. */
void ctm_bridge_agent_recheck(void);

/* True once a probe or a command has reached a verdict about the USB server.
 * ⭐ Until then the answer to ctm_bridge_agent_online() means nothing. */
bool ctm_bridge_agent_probed(void);

bool ctm_bridge_agent_online(void);

void ctm_bridge_agent(char *out, size_t out_len);

/* Re-enumerate and fill out[0..max-1] with the detected devices; returns the
 * count. Index i is stable for ctm_bridge_plug_index(i)/unplug_index(i) until the
 * next ctm_bridge_list() call. */
int ctm_bridge_list(ctm_bridge_dev_t *out, int max);

/* Is the controller behind this hidraw node already bridged? A plug attempt
 * cannot answer this -- it returns false both when already plugged and when
 * genuinely refused. */
bool ctm_bridge_node_is_plugged(const char *node);

/* True for a Bluetooth DualSense. */
bool ctm_bridge_node_is_bluetooth(const char *node);

/* Confirmation signals, all or nothing -- tone, pulse, lightbar and fallback,
 * on every transport. */
bool ctm_bridge_signals_enabled(void);

/* Will the core signal this controller itself? If so the app leaves it alone,
 * rather than adding a second, coarser signal on top. */
bool ctm_bridge_node_signals_itself(const char *node);

/* Signal a refused plug richly if this controller can be -- true if it did.
 * Bluetooth only: a cable needs a session to reach its sound card. */
bool ctm_bridge_signal_refused(const char *node);

/* Plug whichever device owns this /dev/hidrawN. When: a local gesture on the
 * controller itself named a device -- its node is the only identifier that
 * separates two otherwise identical controllers. Returns true if it plugged. */
bool ctm_bridge_plug_node(const char *node);

/* Manually plug / unplug the device at the given list index. */
bool ctm_bridge_plug_index(int index);
void ctm_bridge_unplug_index(int index);

/* Plug every recognised controller (skips already-plugged); returns count newly
 * plugged. Unplug all releases every bridged session. */
int ctm_bridge_plug_all(void);
void ctm_bridge_unplug_all(void);

/* Flat per-controller settings (mirrors the bridge's tv_bridge_worker_settings_t,
 * so moonlight doesn't need the ctmcore headers). */
typedef struct {
    int kind;                     /* 0 = hid, 4 = ds4, 5 = ds5 */
    int audio_mode;               /* 0 Auto / 1 Off / 2 Speaker / 3 Headset / 4 Both */
    int latency_ms;
    int haptics_gain_centi;
    int headset_volume_percent;
    int speaker_volume_percent;
    int ds5_patch_high, ds5_patch_low, ds5_patch2_high, ds5_patch2_low;
} ctm_bridge_settings_t;

/* Get / apply (live) the per-controller settings for the device at the index. */
bool ctm_bridge_get_settings(int index, ctm_bridge_settings_t *out);
void ctm_bridge_set_settings(int index, const ctm_bridge_settings_t *in);

/* TV pointer -> host mouse (synthesizer, kind "hid"): auto-plugged by
 * ctm_bridge_start; the Magic Remote row in the panel toggles it. While
 * active, the streaming input path feeds pointer state here INSTEAD of the
 * moonlight mouse channel (single input authority on the host). x/y in
 * surface coords of a w x h surface; buttons bit0=left bit1=right
 * bit2=middle; wheel in detents. */
bool ctm_bridge_pointer_active(void);
void ctm_bridge_pointer_feed(int x, int y, int w, int h, unsigned buttons, int wheel);
/* Send a keyboard key (HID Keyboard/Keypad usage; SDL scancodes for arrows /
 * Enter equal the HID usage) through the pointer device's keyboard report. */
void ctm_bridge_pointer_feed_key(unsigned hid_usage, bool down);

#ifdef __cplusplus
}
#endif

#endif /* CTM_BRIDGE_GLUE_H */
