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

#include <time.h>

#include "ctm_bridge_gesture.h"
#include "stream/session.h"
#include "stream/input/session_input.h"

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

/* Every line carries the time it was written.
 *
 * The lines from the bridge core already do; ours did not, which meant a hang
 * between two of our own lines could not be measured at all -- only described.
 * That happened on 2026-08-11: the app froze between "fired on" and "pulse
 * finished" and the log could not say whether that was one second or ninety.
 *
 * Same clock the bridge core uses, so lines from both interleave correctly. */
static void gesture_log(const char *fmt, ...) {
    FILE *f = fopen(GESTURE_LOG, "a");
    if (!f) {
        return;
    }
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    fprintf(f, "%lld.%03ld ", (long long)ts.tv_sec, ts.tv_nsec / 1000000L);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

/* Monotonic milliseconds, for measuring how long a call took. Separate from
 * the wall clock above: the wall clock says when, this says how long. */
static uint64_t gesture_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)(ts.tv_nsec / 1000000L);
}

/* One entry per controller being watched. Deliberately keyed by SDL's own
 * instance id rather than by position: controllers come and go. */
#define MAX_WATCHED 8

typedef struct {
    /* ZERO IS A VALID SDL ID, so the table cannot use it to mean "empty".
     *
     * The first controller on this platform genuinely gets id 0 -- the log
     * says "gesture started on controller 0" -- and a zeroed table then looks
     * like it already contains it. watched_for() survived that by accident:
     * for id 0 it returns the first empty slot, which happens to be right.
     * Anything that actually asks whether a controller is known did not. */
    bool in_use;
    SDL_JoystickID id;
    uint32_t since;      /* SDL ticks when the gesture began; 0 = not held */
    bool fired;          /* already acted on this hold */
    bool asked_full;     /* full-report request already sent for this connection */
    uint8_t flash_left;  /* half-steps of the outcome flash still to show */
    /* ⭐ WHICH OUTCOME THE FLASH IS SAYING. 0 = refused (red), 1 = bridged
     * (green). The mechanism is identical; only the colour and the count
     * differ, so one is not worth duplicating for the other. */
    uint8_t flash_ok;
    uint8_t prep_left;   /* steps of the pre-plug pulse still to show */
    uint32_t prep_next;  /* SDL ticks when the next pulse step is due */
    char prep_node[64];  /* the node to plug once the pulse finishes */
    bool ours_plugged;   /* we plugged this one and it is still bridged */
    uint32_t plug_check_next;  /* SDL ticks when to look again */
    uint8_t plug_miss;   /* consecutive "not plugged" answers -- see PLUG_MISSES */
    /* T-120: transport of prep_node, resolved once when it is set.
     * 1 = Bluetooth, 2 = wired. Lets the gates ask without enumerating. */
    uint8_t xport;

    uint8_t bye_left;    /* steps of the post-unplug pulse still to show */
    uint32_t bye_next;   /* SDL ticks when the next bye step is due */
    uint8_t buzz_left;   /* half-steps of the refusal rumble still to run */
    uint32_t buzz_next;  /* SDL ticks when the next buzz half-step is due */
    uint32_t bye_started;  /* SDL ticks when the bye pulse began */
    uint16_t bye_steps;    /* steps actually drawn, for timing the loop */
    uint32_t flash_next; /* SDL ticks when the next half-step is due */
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

/* Is this controller already being watched? Asked before watched_for(), which
 * claims a slot as a side effect and would make every controller look known. */
static bool watched_slot_exists(SDL_JoystickID id) {
    for (int i = 0; i < MAX_WATCHED; ++i) {
        if (s_watched[i].in_use && s_watched[i].id == id) return true;
    }
    return false;
}

static watched_t *watched_for(SDL_JoystickID id) {
    watched_t *free_slot = NULL;
    for (int i = 0; i < MAX_WATCHED; ++i) {
        if (s_watched[i].in_use && s_watched[i].id == id) {
            return &s_watched[i];
        }
        if (!free_slot && !s_watched[i].in_use) {
            free_slot = &s_watched[i];
        }
    }
    if (free_slot) {
        free_slot->in_use = true;
        free_slot->id = id;
        free_slot->since = 0;
        free_slot->fired = false;
    }
    return free_slot;
}

/* Say everything SDL knows about a controller, once, when it first appears.
 *
 * TWO QUESTIONS AT ONCE, and both were open.
 *
 * FIRST: whether the app can identify a controller BEFORE it is bridged. It
 * reads the controller's own address at plug time today, which is too late for
 * the two features that need it -- retiring the right emulated pad, and
 * deciding at stream start whether to upgrade this particular controller.
 * SDL's PlayStation driver reads the same report we do, so it may already have
 * the answer here. If the serial below is a MAC, identity is available from the
 * moment the controller appears and neither feature needs anything new.
 *
 * SECOND: an arrival trail. There was none, which is why a controller losing
 * its buttons after a reseat could not be explained by the log.
 *
 * The path matters as much as the serial: on webOS SDL returns the hidraw node
 * directly, and that is what everything else in this project speaks. */
static void log_controller_identity(SDL_GameController *controller, SDL_JoystickID id) {
    if (!controller) {
        gesture_log("controller %d arrived, but SDL has no handle for it", (int)id);
        return;
    }
    SDL_Joystick *joy = SDL_GameControllerGetJoystick(controller);
    const char *name = SDL_GameControllerName(controller);
    const char *path = SDL_GameControllerPath(controller);
    const char *serial = NULL;
#if SDL_VERSION_ATLEAST(2, 0, 14)
    serial = SDL_JoystickGetSerial(joy);
#endif
    gesture_log("controller %d arrived: name=[%s] path=[%s] serial=[%s] vid=%04x pid=%04x player=%d",
                (int)id,
                name ? name : "?",
                path ? path : "?",
                (serial && serial[0]) ? serial : "(none)",
                (unsigned)SDL_JoystickGetVendor(joy),
                (unsigned)SDL_JoystickGetProduct(joy),
                SDL_GameControllerGetPlayerIndex(controller));
}

/* Is the gesture being made right now? Two fingers down AND the touchpad
 * pressed -- the same three facts the bridge core reads out of the raw report,
 * asked of SDL instead. */
/* Should THIS file produce the signal, or has the core already?
 *
 * Both transports have a rich signal -- a tone and a felt pulse through the
 * controller's audio -- and a coarse SDL fallback. Running both gives two
 * buzzes of different characters, which reads as a controller that does not
 * know what it is doing.
 *
 * ⭐ So the question is not "which cable is this" but "is the rich one about
 * to play". On Bluetooth the core always can; on a cable it can once a session
 * has opened the speaker, which means a refused plug correctly falls back here
 * and a successful one correctly does not.
 *
 * ⚠️ And on Bluetooth there is deliberately NO fallback. The ways that path
 * can fail take SDL down with it -- a controller that has stopped accepting
 * reports ignores SDL's just as readily as ours -- so a second signal that
 * fails in the same conditions is noise, not insurance. */
static bool gesture_signal_here(const char *node)
{
    if (!ctm_bridge_signals_enabled()) return false;
    return !ctm_bridge_node_signals_itself(node);
}

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

/* Tell the user, on the controller itself, that a bridge attempt was refused.
 *
 * Three short YELLOW flashes. Two reasons it is not red. Red is the DualSense's
 * PLAYER TWO colour -- blue, red, green, purple in order -- so a red lightbar
 * already means something specific to anyone who has played a multiplayer game.
 * And a refused plug is a warning rather than a fault: it almost always means
 * the listener is not running, which is something to go and start, not
 * something broken. Amber says that everywhere else, and it leaves red free in
 * case something genuinely bad ever needs saying.
 *
 * Three flashes is clearly deliberate rather than a glitch.
 *
 * This is the one signal with nothing to compete against. A SUCCESSFUL bridge
 * is announced by the controller's own connect behaviour -- it lights up by
 * itself, and an attempt to say the same thing over the top was abandoned on
 * 2026-08-06 after it lost every fight with the host, and turned out to be
 * killing Bluetooth sessions by writing a wired report over a Bluetooth link.
 * A REFUSAL has no such signal: nothing connected, so nothing lit up, and
 * nothing else is writing to the controller.
 *
 * Written straight to the device, the same way the full-report request is: the
 * refusal happens here, and the bridge cannot say anything about a controller
 * it never took.
 *
 * WIRED ONLY, for the reason the light show learned the hard way: this is the
 * wired output report, and Bluetooth expects its own format with a checksum. */
/* The pre-plug pulse: magenta, ramping up and back down over about a second,
 * immediately before the controller is handed to the host.
 *
 * Legitimate at this moment and only this moment. Nothing is bridged yet, so
 * neither the game, nor Steam, nor Windows has any claim on the light -- the
 * controller is still ours right up to the instant we give it away. Magenta
 * because it matches the pairing colour: it reads as "connecting", not as a
 * status we are asserting.
 *
 * The plug is DEFERRED until the pulse ends. Waiting a second here would mean
 * sleeping on the app's main loop and freezing the interface, so the loop's own
 * passes are the clock and the plug happens on the last step. */
#define PREP_STEPS        20
#define PREP_STEP_MS      50
#define PREP_STEPS_HALF   (PREP_STEPS / 2)

/* The post-unplug signal: three shorter breaths rather than one long one.
 * Repetition is what makes it unmistakable, and each breath is brief enough
 * that three of them still pass in about a second and a half. */
#define BYE_PULSES          3
#define BYE_STEPS_PER_PULSE 10
#define BYE_STEPS           (BYE_PULSES * BYE_STEPS_PER_PULSE)

/* How often to ask whether a controller we plugged is still bridged.
 *
 * The app is never told when an unplug finishes -- the teardown happens on the
 * bridge core's own worker, and the watcher only ever sees the gesture start.
 * So the transition has to be noticed by asking.
 *
 * Twice a second, and only while a controller we plugged is still plugged.
 * That lookup enumerates devices, which is real work; asking every pass would
 * put a filesystem scan on the app's main loop. The cheaper fix, when it is
 * worth doing, is for the core to say so rather than for this to ask. */
#define PLUG_CHECK_MS     500
/* ⛔⛔ HOW MANY CONSECUTIVE "NOT PLUGGED" ANSWERS BEFORE WE BELIEVE IT.
 *
 * ctm_bridge_node_is_plugged() RE-ENUMERATES on every call, and an enumeration
 * that runs while a device is arriving or leaving can miss a node that is
 * perfectly well bridged. On one answer that is indistinguishable from an
 * unplug, so a healthy bridge was torn down by an unrelated controller being
 * paired.
 *
 * ⭐ Measured 2026-08-18: a DualSense was paired over Bluetooth while a
 * DualSense Edge was bridged. Three milliseconds after the new controller
 * arrived, the Edge was restored to moonlight and the app died shortly after:
 *
 *     controller 2 arrived: name=[DualSense Wireless Controller] ...
 *     moonlight: slot 1 back from the bridge
 *     /dev/hidraw0 came back to us -- pulsing yellow
 *
 * ⚠️ Nothing was pressed. The bridge was fine; the CHECK was wrong.
 *
 * ⭐ Two costs one extra interval -- half a second -- to notice a real unplug,
 * against never tearing down a live bridge because a scan blinked. A real
 * unplug stays unplugged; a race does not. */
#define PLUG_MISSES       2

/* ⭐⭐ T-120: THE BLUETOOTH PATH IS REBUILT ONE LAYER AT A TIME.
 *
 * A bridge over Bluetooth stalled, froze the interface, powered the controller
 * off, and has not played a confirmation tone since build 100. Nine theories
 * read out of this file were wrong; every real answer came from deploying old
 * builds. So the layers go back one at a time, each measured before the next.
 *
 * ⛔⛔ EVERY GATE IS BLUETOOTH-ONLY, behind a transport check resolved once per
 * bridge. Wired is untouched: it works, it passes acceptance, and a change it
 * can reach is a change that can break it.
 *
 * ⚠️ ALL 0: a Bluetooth controller bridges from the PANEL BUTTON and nothing
 * else -- no chord, no light, no pulse, no tone. That build answers one
 * question: is the bridge itself fast.
 *
 * ⓘ Nothing is deleted. The code behind each gate encodes a fortnight of
 * hardware findings, each learnt by breaking something. */
#define BT_LAYER_GESTURE  1
#define BT_LAYER_LIGHT    1
#define BT_LAYER_RUMBLE   0
#define BT_LAYER_TONE     0

/* The refusal rumble: three short sharp bursts.
 *
 * WHY RUMBLE AND NOT A TONE. A refused plug means no session and no audio
 * device open, so nothing that needs one can make a sound. SDL still holds
 * this controller -- it is how the flash above works -- and its rumble call
 * builds the report itself, so this needs no card, no scan and no guess about
 * which controller it reaches. It is addressed by device.
 *
 * THREE SHORT AND SHARP, against one long soft pulse for success. A rumble on
 * its own reads as a positive acknowledgement, so the difference has to be the
 * rhythm rather than the sensation -- the way a phone buzzes once for a
 * message and repeatedly for an alarm.
 *
 * It will also FEEL different from the success signal, which travels through
 * the audio path rather than the motors. That is useful here rather than a
 * problem. */
/* TIMINGS SIZED FOR BLUETOOTH, NOT FOR A CABLE.
 *
 * These were 90 ms on / 70 ms off. Over a cable that reads as three crisp
 * taps. Over Bluetooth it was reported as weak and, more tellingly, as
 * VARYING in strength -- which is what a smeared pattern feels like rather
 * than a quiet motor.
 *
 * The arithmetic supports that reading. webOS pushes a Bluetooth report
 * roughly every 30-40 ms, and every half-step here queues one. Six steps in
 * half a second is a report every 40 ms with nothing to spare, so an "on"
 * lands late and its "off" lands on top of it.
 *
 * At 200/150 each burst gets five or six reports and each gap gets four, so
 * the shape survives the pacing. Slower on a cable too, but three taps at
 * this length still read as three taps -- and a signal that is legible
 * everywhere beats one that is crisp on the transport most people do not
 * use.
 *
 * STRENGTH IS DELIBERATELY UNCHANGED, so the timings could be judged on
 * their own. Verified over Bluetooth: felt consistently every time, and the
 * same controller on a cable now feels the same as it does over Bluetooth. */
/* The confirmation pulse, felt rather than heard.
 *
 * WHY IT IS HERE RATHER THAN WITH THE TONE. Over a cable the tone and the
 * pulse are one buffer written to the controller's audio device. Bluetooth
 * has no such device, and two ways of reaching the motors there were tried
 * and abandoned:
 *
 *   - The speaker half is Opus-encoded and nothing on this TV can encode it.
 *   - The haptics half IS raw samples in the output report -- but that block
 *     is only present when the host is actually sending haptics. Measured on
 *     C3 2026-08-11: the pulse was armed and never once consumed, because no
 *     game was running. Which is precisely when a confirmation is wanted.
 *
 * SDL's rumble has none of those problems. It builds the right report for
 * whichever transport the controller is on, needs no codec and no report
 * format knowledge, and the refusal buzz already proves it reaches a
 * Bluetooth controller.
 *
 * One pulse, longer and gentler than a refusal burst, so the two are told
 * apart by feel rather than by counting. */
#define OK_PULSE_MS       520   /* doubled; see the refusal timings below */
#define OK_PULSE_STRENGTH 0x7FFF   /* softer than a refusal: this is good news */

#define BUZZ_BURSTS       3
/* Doubled for the same reason as the flashes above -- long enough to be
 * noticed and then looked at, rather than felt and missed. */
#define BUZZ_ON_MS        400
#define BUZZ_OFF_MS       300
#define BUZZ_STRENGTH     0xBFFF     /* firm, and short enough not to nag */

/* DOUBLED, so the signal is still going when you look at it.
 *
 * ⭐ rhoquinn8217, 2026-08-12: "by the time I notice the rumble to look down, the red
 * flashes have already passed." A signal you feel before you see is only
 * useful if it outlasts the reaction it provokes. Three flashes at 120 ms was
 * over in under a second. */
#define REFUSED_FLASHES   3
/* ⭐ Two green flashes on a successful bridge, against three red on a refusal.
 * Green for done, red for refused -- the light says which without anyone
 * learning a vocabulary. Fewer than the refusal because success is the ordinary
 * case and does not need insisting on. */
#define BRIDGED_FLASHES   2
#define REFUSED_ON_MS     240
#define REFUSED_OFF_MS    240

/* One step of the flash.
 *
 * Set through SDL rather than by writing a report ourselves. Two reasons, both
 * learned the hard way on 2026-08-06:
 *
 * SDL's own PlayStation driver is enabled here (it is what makes a Bluetooth
 * controller send its touchpad at all), and that driver writes output reports
 * including the lightbar. A raw write is simply overwritten by it -- the light
 * went straight to whatever SDL wanted and our colour was never seen.
 *
 * And SDL builds the right report for whichever transport the controller is
 * on, checksum included. Writing the wired report over a Bluetooth link was
 * killing sessions, which is why the earlier confirmation light had to be
 * gated to wired. Going through SDL removes that limit entirely.
 *
 * Deliberately NOT a loop with sleeps in it: this runs on the app's main loop,
 * and sleeping here would freeze the interface for the length of the signal.
 * The loop's own passes are the clock, exactly as the gesture hold is timed. */
static void flash_write(SDL_GameController *controller, uint8_t r, uint8_t g, uint8_t b) {
    if (controller) {
        SDL_GameControllerSetLED(controller, r, g, b);
    }
}

/* Leave the light on the controller's player colour.
 *
 * The lightbar is the player indicator -- blue, red, green, purple for players
 * one to four -- and moonlight assigns an index to every controller. A fixed
 * blue would tell a second controller it was player one: worse than leaving
 * the light alone, because it is a meaningful colour stated wrongly.
 *
 * Set directly rather than by re-applying the index: SDL sees the same value
 * and repaints nothing, which left the light stuck on the signal colour.
 * Measured 2026-08-06 on both transports. */
static void paint_player_colour(SDL_GameController *controller) {
    static const uint8_t player_rgb[4][3] = {
        { 0x00, 0x00, 0xff },   /* 1: blue   */
        { 0xff, 0x00, 0x00 },   /* 2: red    */
        { 0x00, 0xff, 0x00 },   /* 3: green  */
        { 0xff, 0x00, 0xff },   /* 4: purple */
    };
    int slot = SDL_GameControllerGetPlayerIndex(controller);
    if (slot < 0 || slot > 3) {
        slot = 0;
    }
    flash_write(controller, player_rgb[slot][0],
                player_rgb[slot][1], player_rgb[slot][2]);
}

/* A ramp that rises and falls: bright in the middle, dark at both ends, so it
 * reads as a breath rather than a blink. For a signal that ends on a colour of
 * its own, where the last step is not the thing being seen.
 *
 * `span` is the length of ONE breath, so a longer count simply repeats it. */
static uint8_t pulse_level_breath(uint8_t step, uint8_t span) {
    uint8_t half = span / 2;
    if (half == 0) {
        return 0;
    }
    uint8_t p = step % span;
    return (p > half) ? (uint8_t)((span - p) * (255 / half))
                      : (uint8_t)(p * (255 / half));
}

/* A ramp that only rises, ending at full brightness.
 *
 * For the handover: the light is brightest at the moment control is given
 * away, and stays lit through the second or so the host takes to enumerate.
 * Falling instead left it nearly dark for that whole gap, which read as a
 * stall rather than a handover. */
static uint8_t pulse_level_rising(uint8_t step) {
    if (step >= PREP_STEPS) {
        return 0;
    }
    return (uint8_t)(((PREP_STEPS - step) * 255u) / (PREP_STEPS - 1));
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
        if (s_watched[i].in_use && s_watched[i].id == id) {
            /* The app said nothing at all when a controller arrived or left,
             * so when its view of controllers went wrong there was no record
             * of how it got that way. A reseated controller lost its buttons
             * on 2026-08-09 and a grep for arrivals came back empty. */
            gesture_log("controller %d gone", (int)id);
            memset(&s_watched[i], 0, sizeof(s_watched[i]));
            return;
        }
    }
}

/* The stream input for the session currently running, or NULL when none is.
 *
 * Set once per pass by the tick. The gesture path needs it to tell moonlight
 * that a controller has changed hands, and it is threaded from the app rather
 * than looked up because there is no accessor for "the current session". */
static stream_input_t *s_stream_input;

/* Hand this controller to the bridge, or take it back, on moonlight's side.
 *
 * SEPARATE FROM THE BRIDGE STATE ON PURPOSE. The bridge decides whether a
 * controller is plugged; this only mirrors that decision into the stream so
 * the host stops seeing two of the same pad. Asking the bridge from inside
 * the send path was tried and crashed the app. */
static void gesture_moonlight_set_excluded(SDL_GameController *controller, bool excluded) {
    if (!s_stream_input || !controller) {
        return;
    }
    SDL_Joystick *js = SDL_GameControllerGetJoystick(controller);
    if (!js) {
        return;
    }
    app_gamepad_state_t *gp =
            app_input_gamepad_state_by_instance_id(s_stream_input->input,
                                                   SDL_JoystickInstanceID(js));
    if (!gp) {
        return;
    }
    if (excluded) {
        stream_input_exclude_gamepad(s_stream_input, gp);
        gesture_log("moonlight: slot %d handed to the bridge", gp->gs_id);
    } else {
        stream_input_restore_gamepad(s_stream_input, gp);
        gesture_log("moonlight: slot %d back from the bridge", gp->gs_id);
    }
}

/* Returns true if the controller was just handed to the bridge. */
static bool gesture_poll_one(SDL_GameController *controller, SDL_JoystickID id) {
    const bool was_known = (watched_slot_exists(id));
    watched_t *w = watched_for(id);
    if (w && !was_known) {
        log_controller_identity(controller, id);
    }
    if (!w) {
        return false;
    }

    /* A refusal rumble in progress: alternate on and off, one half-step per
     * pass, no sleeping. Runs alongside the yellow flash rather than after
     * it -- they are the same signal in two senses, and a user watching the
     * light and a user holding the controller should learn at the same
     * moment. */
    if (w->buzz_left > 0) {
        uint32_t now_ticks = SDL_GetTicks();
        if (now_ticks >= w->buzz_next) {
            --w->buzz_left;
            const bool on = (w->buzz_left % 2) == 1;
            if (controller) {
                SDL_GameControllerRumble(controller,
                                         on ? BUZZ_STRENGTH : 0,
                                         on ? BUZZ_STRENGTH : 0,
                                         on ? BUZZ_ON_MS : BUZZ_OFF_MS);
            }
            w->buzz_next = now_ticks + (on ? BUZZ_ON_MS : BUZZ_OFF_MS);
        }
        /* Deliberately does NOT return. The flash runs at the same time, and
         * stopping here would hold it up for half a second. */
    }

    /* The controller has just come back to us: pulse yellow, then leave it on
     * its player colour. Runs after an unplug has actually completed, which is
     * the moment the light stops belonging to the host. */
    if (w->bye_left > 0) {
        uint32_t now_ticks = SDL_GetTicks();
        if (now_ticks >= w->bye_next) {
            --w->bye_left;
            if (w->bye_left == 0) {
                paint_player_colour(controller);
                /* The step count against the elapsed time says whether the
                 * app's loop is fast enough to draw the shape intended: the
                 * loop's passes are the clock, and a slow one stretches every
                 * pulse into a single fade. */
                gesture_log("unplug pulse finished, %u steps in %ums, player colour set",
                            (unsigned)w->bye_steps,
                            (unsigned)(SDL_GetTicks() - w->bye_started));
            } else {
                uint8_t level = pulse_level_breath(w->bye_left, BYE_STEPS_PER_PULSE);
                flash_write(controller, level, level, 0);   /* red + green = yellow */
                ++w->bye_steps;
                w->bye_next = now_ticks + PREP_STEP_MS;
            }
        }
        return false;
    }

    /* Did a controller we plugged just stop being bridged? Asked on a timer,
     * never every pass -- see PLUG_CHECK_MS. */
    if (w->ours_plugged) {
        uint32_t now_ticks = SDL_GetTicks();
        if (now_ticks >= w->plug_check_next) {
            if (!ctm_bridge_node_is_plugged(w->prep_node)) {
                /* ⭐ One miss is not evidence. See PLUG_MISSES. */
                if (++w->plug_miss < PLUG_MISSES) {
                    gesture_log("%s not found on check %u of %u -- waiting",
                                w->prep_node, (unsigned) w->plug_miss,
                                (unsigned) PLUG_MISSES);
                    w->plug_check_next = now_ticks + PLUG_CHECK_MS;
                    return false;
                }
                w->plug_miss = 0;
                w->ours_plugged = false;
                gesture_moonlight_set_excluded(controller, false);
                /* ⛔⛔ ON BLUETOOTH THE CORE ALWAYS CLAIMS THE SIGNAL, AND
                 * WITH BT_LAYER_TONE OFF IT THEN DOES NOTHING.
                 *
                 * gesture_signal_here() asks "should I signal, or has the core
                 * already?" -- and on Bluetooth the answer is always "the core
                 * will". That was true until the layered rebuild gated the
                 * core's Bluetooth signal off, at which point neither side did
                 * anything and an unbridge had no flashes at all. Found in step
                 * 3, 2026-08-18.
                 *
                 * ⭐ So the question is not just "will the core signal" but
                 * "will the core signal AND is it switched on". */
                const bool core_will_signal =
                    !gesture_signal_here(w->prep_node) &&
                    (w->xport != 1 || BT_LAYER_TONE);
                if (!core_will_signal) {
                    w->bye_left = (w->xport != 1 || BT_LAYER_LIGHT) ? BYE_STEPS : 0;   /* T-120 */
                } else {
                    /* The core did the flashes. ⚠️ But it does not put the
                     * colour back: it releases its claim on the lightbar,
                     * which only stops it writing -- the last colour it wrote
                     * is still showing. The player colour belongs to this
                     * side, and the sequence that normally restores it is the
                     * one being skipped here, so do it directly. */
                    w->bye_left = 0;
                    paint_player_colour(controller);
                }
                w->bye_next = now_ticks;
                w->bye_started = now_ticks;
                w->bye_steps = 0;
                /* Handed back. The same pulse as the handover: the event is
                 * "this controller changed hands", and which way is already
                 * said by the light -- magenta going, yellow returning. */
                /* NOTHING FROM HERE ON THE WAY BACK. The core signals an
                 * unplug BEFORE it tears the session down, so the felt pulse
                 * has already played by the time this runs -- on both
                 * transports.
                 *
                 * ⚠️ AND ASKING WOULD GIVE THE WRONG ANSWER. By now the session
                 * is gone, so "will the core signal this?" answers no, and a
                 * second coarse buzz fires on top of the rich one that already
                 * did. The check would be happening after the thing it asks
                 * about. Measured on a cable, 2026-08-12: two buzzes on every
                 * unbridge, one rich and one not.
                 *
                 * ⓘ The one case this loses is a session that died without a
                 * clean unplug -- the listener going away -- where the core
                 * never signalled. That path leaves the host's pad unrestored
                 * anyway, so it is not worth keeping a buzz for. */
                gesture_log("%s came back to us -- pulsing yellow", w->prep_node);
            } else {
                w->plug_miss = 0;
                w->plug_check_next = now_ticks + PLUG_CHECK_MS;
            }
        }
    }

    /* A pre-plug pulse in progress: one step per pass, no sleeping. The plug
     * itself happens on the final step. */
    if (w->prep_left > 0) {
        uint32_t now_ticks = SDL_GetTicks();
        if (now_ticks >= w->prep_next) {
            --w->prep_left;
            if (w->prep_left == 0) {
                /* TIMED, because this is the suspect.
                 *
                 * This call runs on the app's main loop and talks to the agent
                 * over the network. If it is slow, everything freezes -- no
                 * input, no rendering -- which is exactly what was seen on
                 * 2026-08-11 over Bluetooth: the magenta pulse completed and
                 * the app stopped responding until it eventually recovered.
                 *
                 * The listener was receiving controller input at 400 Hz the
                 * whole time, so the plug had SUCCEEDED at the far end while
                 * this side was still waiting -- which points at waiting for a
                 * reply rather than for a connection. The number below is what
                 * turns that from a story into a measurement. */
                uint64_t plug_t0 = gesture_now_ms();
                bool ok = ctm_bridge_plug_node(w->prep_node);
                uint64_t plug_ms = gesture_now_ms() - plug_t0;
                gesture_log("pulse finished on %s : %s (plug call took %llums)",
                            w->prep_node, ok ? "plugged" : "refused",
                            (unsigned long long)plug_ms);
                if (ok) {
                    /* Bridged. Say so in the hand holding the controller.
                     *
                     * Fires on both transports. Over a cable the audio device
                     * also plays a tone and its own pulse, so this reinforces
                     * rather than replaces -- judged by feel, and easily
                     * gated later if it turns out to be too much. */
                    /* T-120: on Bluetooth the rumble -- and the signal check
                     * that costs an enumeration, measured at 5s on the C3 --
                     * wait for BT_LAYER_RUMBLE. Wired unchanged. */
                    if ((w->xport != 1 || BT_LAYER_RUMBLE) && gesture_signal_here(w->prep_node)) {
                        SDL_GameControllerRumble(controller, OK_PULSE_STRENGTH,
                                                 OK_PULSE_STRENGTH, OK_PULSE_MS);
                    }
                    gesture_log("confirmation pulse on %s: bridged",
                                w->prep_node);
                    w->ours_plugged = true;
                    w->plug_miss = 0;
                    w->plug_check_next = SDL_GetTicks() + PLUG_CHECK_MS;
                    /* ⛔ PUT THE LIGHT BACK. The pre-plug pulse leaves magenta
                     * and walks away: nothing after it repaints, so the light
                     * stayed magenta until Steam claimed it -- long after the
                     * controller was already working. Measured in step 3,
                     * 2026-08-18.
                     *
                     * ⭐ The unplug path has done this all along; the plug path
                     * never did, because on a cable the core's own signal
                     * repainted it and hid the gap. */
                    /* ⭐ TWO GREEN FLASHES, then the player colour. The pulse
                     * before a plug says "asking"; this says "done", and it
                     * mirrors the red the same mechanism shows on a refusal --
                     * so the light alone reports the outcome.
                     *
                     * ⓘ The flash tick restores the player colour on its last
                     * step, which is why nothing does so here. */
                    w->flash_ok = 1;
                    w->flash_left = BRIDGED_FLASHES * 2 + 1;   /* +1 for the restore */
                    w->flash_next = SDL_GetTicks();
                    /* ⛔ RETIRED AFTER THE PLUG, NOT BEFORE. Retiring first was
                     * tried on 2026-08-18 to close the input gap and did not
                     * help -- the gap is Bluetooth-only and wired has none, so
                     * it is the device becoming usable, not the handover. And
                     * it left a phantom: the pad is only restored when the
                     * plugged-check notices the bridge has ended, which needs
                     * ours_plugged, which is set here. */
                    gesture_moonlight_set_excluded(controller, true);
                }
                if (!ok) {
                    /* A refusal is signalled from here on BOTH transports: the
                     * plug failed, so on a cable there is no session and no
                     * speaker to write a richer one to.
                     *
                     * ⚠️ The one switch silences this too, deliberately. A
                     * switch that keeps failures but drops successes sounds
                     * thoughtful and is really a second thing to reason about
                     * -- and if signals are off, being told nothing on failure
                     * is what was asked for. */
                    if (ctm_bridge_signals_enabled()) {
                        /* ⭐ THE LIGHT AND THE SOUND ARE TWO DECISIONS, not
                         * one -- because on a cable they live on different
                         * devices.
                         *
                         * On Bluetooth the core's signal carries both: the
                         * lightbar and the audio ride the same report. On a
                         * cable the audio goes to the sound card and the
                         * lightbar to the HID device, so the core can only do
                         * the sound and the flashes have to come from here.
                         *
                         * Treating it as one decision left a wired refusal
                         * with two tones and no red at all. */
                        const bool sounded =
                            ctm_bridge_signal_refused(w->prep_node);
                        const bool lit =
                            sounded && ctm_bridge_node_is_bluetooth(w->prep_node);

                        if (lit) {
                            /* ⚠️ The core releases its claim on the lightbar
                             * but does not put the colour back -- releasing
                             * only stops it writing, and the last colour it
                             * wrote is still showing. */
                            paint_player_colour(controller);
                        } else {
                            w->flash_ok = 0;
                            w->flash_left = REFUSED_FLASHES * 2 + 1;   /* +1 for the restore */
                            w->flash_next = SDL_GetTicks();
                        }
                        if (!sounded) {
                            w->buzz_left = BUZZ_BURSTS * 2;   /* on and off per burst */
                            w->buzz_next = SDL_GetTicks();
                        }
                        gesture_log("refused on %s: core sounded=%d lit=%d",
                                    w->prep_node, (int)sounded, (int)lit);
                    }
                }
                return ok;
            }
            /* Up then back down: bright in the middle, dark at both ends, so it
             * reads as a breath rather than a blink. */
            uint8_t level = pulse_level_rising(w->prep_left);
            flash_write(controller, level, 0, level);   /* red + blue = magenta */
            w->prep_next = now_ticks + PREP_STEP_MS;
        }
        return false;
    }

    /* A refusal flash in progress: one half-step per pass, no sleeping. */
    if (w->flash_left > 0) {
        uint32_t now_ticks = SDL_GetTicks();
        if (now_ticks >= w->flash_next) {
            --w->flash_left;
            if (w->flash_left == 0) {
                paint_player_colour(controller);
                gesture_log("refusal flash finished, player colour set");
            } else {
                /* Odd counts are the lit ones, so the LAST flash step is lit
                 * rather than an unlit one nobody sees. */
                bool lit = (w->flash_left % 2) == 1;
                /* RED, not yellow.
                 *
                 * Yellow already means "the controller is yours again" -- it
                 * is the colour of a deliberate unbridge. Using it for a
                 * refusal made one colour carry two opposite meanings: you
                 * have it back because you asked, and you still have it
                 * because the bridge failed. */
                if (w->flash_ok) {
                    flash_write(controller, 0x00, lit ? 0xff : 0x00, 0x00);   /* green */
                } else {
                    /* RED, not yellow.
                     *
                     * Yellow already means "the controller is yours again" --
                     * the colour of a deliberate unbridge. Using it for a
                     * refusal made one colour carry two opposite meanings. */
                    flash_write(controller, lit ? 0xff : 0x00, 0x00, 0x00);
                }
                w->flash_next = now_ticks + (lit ? REFUSED_ON_MS : REFUSED_OFF_MS);
            }
        }
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
    /* T-120: the chord is off on Bluetooth until BT_LAYER_GESTURE. Asked once
     * per hold, not per report. Wired never reaches this. */
    if (!BT_LAYER_GESTURE) {
        const char *bt_path = SDL_GameControllerPath(controller);
        char bt_node[64];
        if (bt_path && bt_path[0] &&
            hidraw_node_for_event(bt_path, bt_node, sizeof(bt_node)) &&
            ctm_bridge_node_is_bluetooth(bt_node)) {
            w->fired = true;
            gesture_log("T-120: chord on Bluetooth %s ignored (BT_LAYER_GESTURE=0)", bt_node);
            return false;
        }
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
    /* Already bridged: stand down rather than ask again.
     *
     * Without this the watcher tries to plug a controller that is already
     * plugged, is refused, and flashes the refusal pattern -- a failure shown
     * for something that did not fail. It also made the unplug gesture look
     * broken, because the flash lands during the same hold that is on its way
     * to unplugging.
     *
     * Asked here rather than while the hold runs: fired is already set above,
     * so this costs one lookup per gesture instead of one per report. */
    if (ctm_bridge_node_is_plugged(node)) {
        gesture_log("%s is already plugged -- standing down", node);
        return false;
    }

    /* Start the pulse and hand over when it ends. The node is kept because it
     * is resolved now and used a second later. */
    snprintf(w->prep_node, sizeof(w->prep_node), "%s", node);
    w->xport = ctm_bridge_node_is_bluetooth(node) ? 1 : 2;   /* T-120 */
    /* ⚠️ LOGGED, because the gates depend on it and a wrong answer makes them
     * silently do nothing. ctm_bridge_node_is_bluetooth() matches the node by
     * exact string compare against the scan's own list -- if the two spell it
     * differently it returns false and a Bluetooth controller is treated as
     * wired. */
    gesture_log("T-120: %s transport=%s", node, w->xport == 1 ? "bluetooth" : "wired");
    /* The pre-plug pulse runs on BOTH transports: it happens before anything
     * knows whether the plug will succeed, and it is the only thing marking
     * the gap between the gesture and the outcome. The core's signal follows
     * and says which outcome it was.
     *
     * Silenced only by the one switch, like everything else. */
    /* ⛔ ONE STEP, NOT ZERO, when the light is gated off.
     *
     * The plug happens on the pulse's FINAL step, so a pulse of zero steps
     * never plugs at all -- pressing a row in the panel did nothing. Rather
     * than restructure that coupling mid-rebuild, a gated pulse is one step
     * long: the plug still fires, and one step is imperceptible. ⚠️ The
     * coupling itself is worth removing later; it is recorded in T-120. */
    w->prep_left = (!ctm_bridge_signals_enabled() || (w->xport == 1 && !BT_LAYER_LIGHT))
                       ? 1 : PREP_STEPS;
    w->prep_next = SDL_GetTicks();
    gesture_log("fired on %s -> %s : pulsing before handover", dev_path, node);
    return false;
}


/* The gamepad list, kept so a panel request can find a controller by node.
 * Set on every tick; NULL before the first one. */
static app_input_t *s_gesture_input = NULL;

int ctm_bridge_gesture_player_for_node(const char *node) {
    if (!node || !node[0] || !s_gesture_input) {
        return -1;
    }
    int n = (int) app_input_get_max_gamepads(s_gesture_input);
    for (int i = 0; i < n; ++i) {
        SDL_GameController *gc = s_gesture_input->gamepads[i].controller;
        if (!gc) continue;
        const char *dev_path = SDL_GameControllerPath(gc);
        if (!dev_path || !dev_path[0]) continue;
        char found[64];
        if (!hidraw_node_for_event(dev_path, found, sizeof(found))) continue;
        if (strcmp(found, node) != 0) continue;
        return SDL_GameControllerGetPlayerIndex(gc);
    }
    return -1;
}

bool ctm_bridge_gesture_request_bridge(const char *node) {
    if (!node || !node[0] || !s_gesture_input) {
        return false;
    }
    int n = (int) app_input_get_max_gamepads(s_gesture_input);
    for (int i = 0; i < n; ++i) {
        SDL_GameController *gc = s_gesture_input->gamepads[i].controller;
        if (!gc) {
            continue;
        }
        const char *dev_path = SDL_GameControllerPath(gc);
        if (!dev_path || !dev_path[0]) {
            continue;
        }
        char found[64];
        if (!hidraw_node_for_event(dev_path, found, sizeof(found))) {
            continue;
        }
        if (strcmp(found, node) != 0) {
            continue;
        }

        SDL_Joystick *js = SDL_GameControllerGetJoystick(gc);
        if (!js) {
            return false;
        }
        watched_t *w = watched_for(SDL_JoystickInstanceID(js));
        if (!w) {
            return false;
        }
        /* ⛔ NO "is it already plugged" CHECK HERE, deliberately.
         *
         * ctm_bridge_node_is_plugged() RE-ENUMERATES every device on every
         * call. The chord can afford that -- it runs once, after a two-second
         * hold, off the back of an input poll. On a button press it runs on the
         * LVGL thread and the overlay froze hard enough that the TV pointer
         * stalled with it. Measured 2026-08-18.
         *
         * ⭐ And it was redundant: the panel only offers "Bridge" on a row it
         * has already listed as not plugged. The caller knows. */

        /* ⭐ EXACTLY WHAT A COMPLETED CHORD SETS, and nothing more. Everything
         * that makes a bridge a bridge happens on the following ticks, in the
         * one place it is written. */
        snprintf(w->prep_node, sizeof(w->prep_node), "%s", node);
    w->xport = ctm_bridge_node_is_bluetooth(node) ? 1 : 2;   /* T-120 */
    /* ⚠️ LOGGED, because the gates depend on it and a wrong answer makes them
     * silently do nothing. ctm_bridge_node_is_bluetooth() matches the node by
     * exact string compare against the scan's own list -- if the two spell it
     * differently it returns false and a Bluetooth controller is treated as
     * wired. */
    gesture_log("T-120: %s transport=%s", node, w->xport == 1 ? "bluetooth" : "wired");
        /* ⛔ ONE STEP, NOT ZERO, when the light is gated off.
     *
     * The plug happens on the pulse's FINAL step, so a pulse of zero steps
     * never plugs at all -- pressing a row in the panel did nothing. Rather
     * than restructure that coupling mid-rebuild, a gated pulse is one step
     * long: the plug still fires, and one step is imperceptible. ⚠️ The
     * coupling itself is worth removing later; it is recorded in T-120. */
    w->prep_left = (!ctm_bridge_signals_enabled() || (w->xport == 1 && !BT_LAYER_LIGHT))
                       ? 1 : PREP_STEPS;
        w->prep_next = SDL_GetTicks();
        w->fired = true;
        gesture_log("panel asked to bridge %s : pulsing before handover", node);
        return true;
    }
    gesture_log("panel asked to bridge %s, but no controller is behind it", node);
    return false;
}

void ctm_bridge_gesture_tick(struct app_input_t *input, struct session_t *session) {
    s_stream_input = session ? session_get_input(session) : NULL;
    if (!input) {
        return;
    }
    app_input_t *in = (app_input_t *)input;
    s_gesture_input = in;
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
