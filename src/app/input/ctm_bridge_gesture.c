/* Plug a controller by a gesture on the controller itself, so bridging one does
 * not need the overlay.
 *
 * The mirror of the unplug gesture, which lives in the bridge core: once a
 * controller IS bridged the bridge holds it exclusively and moonlight cannot
 * see it, so that direction watches the raw report stream. This direction is
 * the opposite case -- the controller is NOT bridged, so SDL has it open for
 * menu navigation and reports it normally.
 *
 * Same gesture either way: two fingers on the touchpad while pressing it.
 * Held briefly here, because plugging in is the constructive direction and the
 * fallback (the overlay) still exists; unplugging asks for five seconds.
 *
 * The hard part is not the gesture, it is saying WHICH device to plug. Two
 * DualSense controllers are identical in every name and identifier they carry,
 * so the only thing that separates them is where they are attached. SDL knows
 * the evdev node it opened; that walks up to the USB interface and across to
 * the hidraw node the bridge plugs. Confirmed on hardware (webOS, unrooted):
 *   /sys/class/input/inputN  ->  .../2-1.3:1.3  ->  .../hidraw/hidrawN
 */

#include "ctm_bridge_gesture.h"

#if defined(TARGET_WEBOS)

#include "ctm_bridge_glue.h"
#include "app_input.h"
#include "input_gamepad.h"
#include "logging.h"

#include <dirent.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Shorter than the unplug hold: plugging in is constructive, and the overlay is
 * still there if it is missed. */
#define GESTURE_PLUG_HOLD_MS 2000

/* The app's own log is not readable on webOS -- there is no journal and no
 * /var/log -- so gesture activity goes to a file of its own, beside the ones
 * the bridge core writes. Without it a failure here is completely silent,
 * which cost a build cycle to learn. */
#define GESTURE_LOG "/tmp/ctm-gesture.log"

static void gesture_log(const char *fmt, ...) {
    FILE *f = fopen(GESTURE_LOG, "a");
    if (!f) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

/* One entry per controller being watched. Deliberately keyed by SDL's own
 * instance id rather than by position: controllers come and go. */
#define MAX_WATCHED 8

typedef struct {
    SDL_JoystickID id;
    uint32_t since;      /* SDL ticks when the gesture began; 0 = not held */
    bool fired;          /* already acted on this hold */
    bool asked_full;     /* full-report request already sent for this connection */
} watched_t;

#ifndef HIDIOCGFEATURE
#define HIDIOCGFEATURE(len) _IOC(_IOC_READ | _IOC_WRITE, 'H', 0x07, len)
#endif

/* Ask a DualSense for its full input report.
 *
 * Over Bluetooth the controller sends a REDUCED ten-byte report -- sticks and
 * some buttons, no touchpad at all -- until a host reads the calibration
 * feature report (0x05). Reading it is what makes the controller start sending
 * the full 78-byte report instead. Documented for the DualShock 4 by the Game
 * Controller Collective Wiki and defined identically for the DualSense in the
 * kernel's hid-playstation driver.
 *
 * Nothing on this TV does that read: dmesg shows the controller bound to
 * hid-generic, so hid-playstation -- which would read calibration on probe --
 * never runs. SDL's own PlayStation driver reads it when it opens a controller,
 * but measured 2026-08-06 that does not reliably happen again after a Bluetooth
 * reconnect, leaving the controller in reduced mode with no touchpad for the
 * gesture to see.
 *
 * Harmless when it is not needed: over USB the controller already sends the
 * full report, and reading calibration changes nothing. */
static void request_full_report(const char *dev_path) {
    if (!dev_path || strncmp(dev_path, "/dev/hidraw", 11) != 0) {
        return;
    }
    int fd = open(dev_path, O_RDWR);
    if (fd < 0) {
        gesture_log("full-report request: cannot open %s", dev_path);
        return;
    }
    uint8_t feature[64];
    memset(feature, 0, sizeof(feature));
    feature[0] = 0x05;
    bool ok = ioctl(fd, HIDIOCGFEATURE(sizeof(feature)), feature) >= 0;
    close(fd);
    gesture_log("full-report request on %s: %s", dev_path, ok ? "sent" : "failed");
}

static watched_t s_watched[MAX_WATCHED];

static watched_t *watched_for(SDL_JoystickID id) {
    watched_t *free_slot = NULL;
    for (int i = 0; i < MAX_WATCHED; ++i) {
        if (s_watched[i].id == id) {
            return &s_watched[i];
        }
        if (!free_slot && s_watched[i].id == 0) {
            free_slot = &s_watched[i];
        }
    }
    if (free_slot) {
        free_slot->id = id;
        free_slot->since = 0;
        free_slot->fired = false;
    }
    return free_slot;
}

/* Is the gesture being made right now? Two fingers down AND the touchpad
 * pressed -- the same three facts the bridge core reads out of the raw report,
 * asked of SDL instead. */
static bool gesture_held(SDL_GameController *controller) {
    if (!controller) {
        return false;
    }
    if (!SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_TOUCHPAD)) {
        return false;
    }
    if (SDL_GameControllerGetNumTouchpads(controller) < 1) {
        return false;
    }
    int down = 0;
    int fingers = SDL_GameControllerGetNumTouchpadFingers(controller, 0);
    for (int f = 0; f < fingers; ++f) {
        Uint8 state = 0;
        float x = 0, y = 0, pressure = 0;
        if (SDL_GameControllerGetTouchpadFinger(controller, 0, f, &state,
                                                &x, &y, &pressure) == 0 && state) {
            ++down;
        }
    }
    return down >= 2;
}

/* /dev/input/eventN -> the sysfs input directory that describes it. */
static bool sys_input_dir_for_event(const char *dev_path, char *out, size_t out_len) {
    const char *leaf = strrchr(dev_path, '/');
    leaf = leaf ? leaf + 1 : dev_path;
    if (strncmp(leaf, "event", 5) != 0) {
        return false;
    }
    snprintf(out, out_len, "/sys/class/input/%s/device", leaf);
    return access(out, F_OK) == 0;
}

/* Walk up from the input device to the USB interface, then find the hidraw node
 * underneath it. The interface is the input device's parent's parent, matching
 * the layout confirmed on hardware. */
static bool hidraw_node_for_event(const char *dev_path, char *out, size_t out_len) {
    /* On webOS, SDL opens the controller through hidraw and hands that node
     * straight back -- which is the thing we are looking for. Measured on C3,
     * 2026-08-06: SDL_GameControllerPath returned "/dev/hidraw0". Everything
     * below is the fallback for a build whose SDL reports an evdev node
     * instead, which is what upstream Linux typically does. */
    if (strncmp(dev_path, "/dev/hidraw", 11) == 0) {
        snprintf(out, out_len, "%s", dev_path);
        return true;
    }

    char sys_input[PATH_MAX];
    if (!sys_input_dir_for_event(dev_path, sys_input, sizeof(sys_input))) {
        return false;
    }

    char iface[PATH_MAX];
    snprintf(iface, sizeof(iface), "%s/..", sys_input);

    char resolved[PATH_MAX];
    if (!realpath(iface, resolved)) {
        return false;
    }

    /* The interface directory holds one 0003:VVVV:PPPP.NNNN entry per HID
     * device; the hidraw node lives inside it. The trailing number changes on
     * every re-enumeration, so it is searched for rather than remembered. */
    DIR *d = opendir(resolved);
    if (!d) {
        return false;
    }
    bool found = false;
    struct dirent *ent;
    while (!found && (ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') {
            continue;
        }
        char hidraw_dir[PATH_MAX];
        snprintf(hidraw_dir, sizeof(hidraw_dir), "%s/%s/hidraw", resolved, ent->d_name);
        DIR *hd = opendir(hidraw_dir);
        if (!hd) {
            continue;
        }
        struct dirent *hent;
        while ((hent = readdir(hd)) != NULL) {
            if (strncmp(hent->d_name, "hidraw", 6) == 0) {
                snprintf(out, out_len, "/dev/%s", hent->d_name);
                found = true;
                break;
            }
        }
        closedir(hd);
    }
    closedir(d);
    return found;
}

void ctm_bridge_gesture_reset(SDL_JoystickID id) {
    for (int i = 0; i < MAX_WATCHED; ++i) {
        if (s_watched[i].id == id) {
            memset(&s_watched[i], 0, sizeof(s_watched[i]));
            return;
        }
    }
}

/* Returns true if the controller was just handed to the bridge. */
static bool gesture_poll_one(SDL_GameController *controller, SDL_JoystickID id) {
    watched_t *w = watched_for(id);
    if (!w) {
        return false;
    }

    /* Once per connection, before anything else: make sure the controller is
     * sending its full report, or there is no touchpad to read. */
    if (!w->asked_full) {
        w->asked_full = true;
        request_full_report(SDL_GameControllerPath(controller));
    }

    if (!gesture_held(controller)) {
        w->since = 0;
        w->fired = false;
        return false;
    }
    if (w->fired) {
        return false;   /* wait for the fingers to lift */
    }

    uint32_t now = SDL_GetTicks();
    if (w->since == 0) {
        w->since = now ? now : 1;
        gesture_log("gesture started on controller %d", (int)id);
        return false;
    }
    if (now - w->since < GESTURE_PLUG_HOLD_MS) {
        return false;
    }
    w->fired = true;

    const char *dev_path = SDL_GameControllerPath(controller);
    if (!dev_path || !dev_path[0]) {
        gesture_log("fired, but SDL gave no device path");
        return false;
    }
    char node[64];
    if (!hidraw_node_for_event(dev_path, node, sizeof(node))) {
        gesture_log("fired, but no hidraw node behind %s", dev_path);
        return false;
    }
    bool ok = ctm_bridge_plug_node(node);
    gesture_log("fired on %s -> %s : %s", dev_path, node, ok ? "plugged" : "refused");

    return ok;
}


void ctm_bridge_gesture_tick(struct app_input_t *input) {
    if (!input) {
        return;
    }
    app_input_t *in = (app_input_t *)input;
    int n = (int)app_input_get_max_gamepads(in);
    for (int i = 0; i < n; ++i) {
        SDL_GameController *gc = in->gamepads[i].controller;
        if (!gc) {
            continue;
        }
        SDL_Joystick *js = SDL_GameControllerGetJoystick(gc);
        if (!js) {
            continue;
        }
        /* Moonlight deliberately KEEPS the controller open after bridging.
         *
         * Releasing it was tried on 2026-08-06 and reverted the same evening.
         * The theory was that SDL's own PlayStation driver contends with the
         * bridge for the device -- but the evidence was thin (the bridge was
         * reading reports perfectly well either way), and the real cause of
         * that session's trouble turned out to be a wedged Bluetooth link,
         * cleared by power-cycling the controller.
         *
         * Releasing also STRANDS the controller: nothing gives it back on
         * unbridge, so the gesture -- which needs moonlight to see the pad --
         * can never fire again. Three attempts failed until a power cycle. */
        (void)gesture_poll_one(gc, SDL_JoystickInstanceID(js));
    }
}

#endif /* TARGET_WEBOS */
