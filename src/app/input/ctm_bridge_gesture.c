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
#include "app.h"   /* app_configuration, for the bridge_enable switch */
#include "stream/session.h"
#include "stream/input/session_input.h"

#if defined(TARGET_WEBOS)

#include <errno.h>
#include <fcntl.h>
#include <sys/inotify.h>
#include <unistd.h>

#include "ctm_bridge_glue.h"
#include "app_input.h"
#include "input_gamepad.h"
#include "logging.h"
#include "util/bus.h"
#include "util/user_event.h"

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
/* ⭐ One second, down from two on 2026-08-20. ⚠️ The hold exists to make the
 * gesture deliberate, and a second is still deliberate on a touchpad you have
 * to press with two fingers -- but it is closer to the edge, so an accidental
 * bridge is the thing to watch for. */
#define GESTURE_PLUG_HOLD_MS 1000

/* Gesture activity goes to a file of its own, beside the ones the bridge core
 * writes, so a gesture reads end to end in one place. Without it a failure here
 * is completely silent, which cost a build cycle to learn.
 *
 * ⛔ NOT /tmp. webOS 26 made /tmp traverse-only, so on the U5s this file was
 * never written at all. The core resolves its own logs to the app's logs
 * directory, with /tmp kept only where that directory is not available, and
 * this now asks it for the same file. Declared here rather than by including
 * the core's state header, the way controller_common.c does. */
#define GESTURE_LOG "ctm-gesture.log"
FILE *ctm_log_open(const char *name, const char *mode);

/* Every line carries the time it was written.
 *
 * The lines from the bridge core already do; ours did not, which meant a hang
 * between two of our own lines could not be measured at all -- only described.
 * That happened on 2026-08-11: the app froze between "fired on" and "pulse
 * finished" and the log could not say whether that was one second or ninety.
 *
 * Same clock the bridge core uses, so lines from both interleave correctly. */
static void gesture_log(const char *fmt, ...) {
    FILE *f = ctm_log_open(GESTURE_LOG, "a");
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
    /* ⭐ THE COLOUR THE FLASH IS SAYING IT IN (T-223). This was a yes/no
     * flag whose only caller set it to "refused", while the tick that read
     * it wrote red unconditionally -- so a third pattern could not be asked
     * for at all. The mechanism was always identical bar the colour, which
     * is now simply carried here: red refuses, yellow hands back. */
    uint8_t flash_r, flash_g, flash_b;
    uint8_t prep_left;   /* steps of the pre-plug pulse still to show */
    uint32_t prep_next;  /* SDL ticks when the next pulse step is due */
    char prep_node[64];  /* the node to plug once the pulse finishes */
    bool ours_plugged;   /* we plugged this one and it is still bridged */
    uint32_t plug_check_next;  /* SDL ticks when to look again */
    uint8_t plug_miss;   /* consecutive "not plugged" answers -- see PLUG_MISSES */
    /* T-120: transport of prep_node, resolved once when it is set.
     * 1 = Bluetooth, 2 = wired. Lets the gates ask without enumerating. */
    uint8_t xport;

    uint8_t buzz_left;   /* half-steps of the refusal rumble still to run */
    uint32_t buzz_next;  /* SDL ticks when the next buzz half-step is due */
    uint32_t flash_next; /* SDL ticks when the next half-step is due */
    /* SDL's player index when this side last painted the player colour, so a
     * number that arrives or changes later is painted too. PAINTED_NEVER until
     * the first paint. */
    int painted_slot;
} watched_t;

#define PAINTED_NEVER (-100)

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

/* A DualSense or a DualSense Edge, by its USB ids: the same rule the
 * listener's microphone guard uses, so the two sides agree on what counts. */
static bool controller_is_dualsense(SDL_GameController *controller) {
    const Uint16 vendor = SDL_GameControllerGetVendor(controller);
    const Uint16 product = SDL_GameControllerGetProduct(controller);
    return vendor == 0x054c && (product == 0x0ce6 || product == 0x0df2);
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
        free_slot->painted_slot = PAINTED_NEVER;
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
/* T-212 -- SAY WHETHER A PULSE ACTUALLY HAPPENED.
 *
 * ⛔ Every "pulse" line in this file used to print whether or not a rumble was
 * sent: the SDL call sits inside an `if`, and its return value was discarded.
 * So a log full of "confirmation pulse: bridged" said nothing about whether the
 * pad was ever asked to rumble -- and T-212 spent days reading it as if it did.
 *
 * ⭐ This wraps the call so the log carries the three things that matter: that
 * we tried, what SDL said, and, when we did not try, WHY not.
 *
 * ⓘ SDL_GameControllerRumble returns 0 when it accepted the request and -1 when
 * the controller cannot rumble or has gone; SDL_GetError() then says which. */
static void gesture_rumble_logged(SDL_GameController *controller, const char *what,
                                  const char *node, Uint16 strength, Uint32 ms)
{
    const char *where = (node && node[0]) ? node : "a pad";
    if (controller == NULL) {
        gesture_log("%s on %s: NOT SENT -- SDL has no controller handle for it",
                    what, where);
        return;
    }
    const int rc = SDL_GameControllerRumble(controller, strength, strength, ms);
    if (rc == 0) {
        gesture_log("%s on %s: sent to SDL (%u for %ums)", what, where,
                    (unsigned) strength, (unsigned) ms);
    } else {
        gesture_log("%s on %s: REFUSED by SDL -- %s", what, where, SDL_GetError());
    }
}

/* The same idea as gesture_rumble_logged, for the light. ⭐ Only the handback
 * paint uses it: flash_write() runs inside animations at frame rate and a log
 * line there would drown the file. ⚠️ SDL_GameControllerSetLED returns -1
 * when the pad has no light OR when its driver refused the write, and those
 * two look identical from here -- so the line says which pad it was. */
static void gesture_paint_logged(SDL_GameController *controller, const char *what,
                                 const char *node, int rc)
{
    const char *where = (node && node[0]) ? node : "a pad";
    if (controller == NULL) {
        gesture_log("%s on %s: NOT SENT -- SDL has no controller handle for it",
                    what, where);
        return;
    }
    const int slot = SDL_GameControllerGetPlayerIndex(controller);
    const int has = SDL_GameControllerHasLED(controller) ? 1 : 0;
    if (rc == 0) {
        gesture_log("%s on %s: player %d colour accepted by SDL", what, where, slot);
    } else {
        gesture_log("%s on %s: REFUSED by SDL (player %d, HasLED=%d) -- %s",
                    what, where, slot, has, SDL_GetError());
    }
}

/* Arm the outcome flash: `flashes` lit steps in one colour, then the player
 * colour again. ⓘ The count is doubled for the dark halves and carries one
 * extra step, which is the restore -- the last flash is lit, so without it
 * the pad keeps the colour. */
static void flash_arm(watched_t *w, uint8_t r, uint8_t g, uint8_t b, int flashes)
{
    if (w == NULL) return;
    w->flash_r = r;
    w->flash_g = g;
    w->flash_b = b;
    w->flash_left = (uint8_t)(flashes * 2 + 1);
    w->flash_next = SDL_GetTicks();
}

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
/* ⭐⭐ TWO breaths, not three, and MANY more steps in each.
 *
 * ⛔ Ten steps is too few to read as breathing -- each one is a visible jump,
 * and nine jumps in a row look like a flicker however long they take. Slowing
 * the step to 90 ms made it MORE steppy, not smoother: measured 453 ms then
 * 812 ms for the same nine steps, and it still read as fast.
 *
 * ⭐ Smoothness is the step COUNT; pace is the interval. 26 steps at 45 ms is a
 * breath of about 1.2 s with 26 brightness levels in it. ⓘ Dropped to two
 * breaths so the handback does not run to three and a half seconds. */

/* ⭐⭐ HOW LONG ONE BREATH TAKES. Ten steps at this interval.
 *
 * ⛔ It used to borrow the pre-plug pulse's 50 ms, which made a breath half a
 * second long -- and half a second reads as a BLINK, not breathing. rhoquinn8217,
 * 2026-08-19: "it's just going really fast now." ⓘ The log had been saying so
 * all along: `bridge pulse finished, 9 steps in 453ms`.
 *
 * ⚠️ The pre-plug pulse keeps the shorter step deliberately: it runs DURING the
 * two-second hold and has to fill it, so it is a different job.
 *
 * ⭐ 90 ms gives a breath just under a second, which is about the rate a person
 * breathes and is what makes it read as calm rather than urgent. */
/* ⭐ 20 ms, which the app's loop can hold -- it ran 25 steps in 1128 ms at 45,
 * so the pace is real rather than aspirational. Sixty steps at 20 ms is a
 * breath of 1.2 s with THIRTY brightness levels climbing and thirty falling. */
/* ⭐ How long ONE breath lasts. The shape is drawn from the clock, so this is
 * a real duration rather than a step count times an interval -- and it holds
 * whatever the app's loop is doing. About a second reads as breathing; half a
 * second reads as a blink. */

/* ⭐ The BRIDGE breath is deliberately shorter than the handback's.
 *
 * ⛔ Not a style choice. A bridge hands the controller over the instant this
 * starts -- the emulated pad retires, the real one arrives, and the host
 * repaints -- so a long breath spends most of itself competing with a device
 * changing hands. ⚠️ Measured by eye across builds: 450 ms read as clean,
 * 800-1200 ms read as flickering throughout.
 *
 * ⓘ A handback has no such problem, because nothing is being relayed by then --
 * which is why yellow has never flickered and can afford to be slow. */

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
 * ⓘ THE REBUILD FINISHED WITH EVERY LAYER BACK ON, and the four switches that
 * gated them (BT_LAYER_GESTURE, _LIGHT, _RUMBLE, _CORE_SIGNAL, all pinned to 1)
 * were removed with the branches that only ran at 0 (2026-09-15). The code
 * that remains is the layers themselves; the hardware findings behind them are
 * in the comments where each one acts. */

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


/* ⭐⭐ THE HANDBACK, FOR A PAD THAT HAS NOTHING ELSE TO SAY IT WITH
 * (rhoquinn8217, 2026-09-15: "we need a rumble for xbox controller on
 * release").
 *
 * ⛔ A release is announced by the core, in sound and haptics, and by this side
 * putting the player colour back. An Xbox pad has no lightbar, no speaker and
 * no core signal of its own, so it was handed back in complete silence: the
 * only way to know it had worked was to look at the panel.
 *
 * ⓘ ONE pulse, because this one is good news. Three is the refusal, and the
 * difference is a signal that stops against one that insists -- see
 * BUZZ_BURSTS. */
#define BYE_PULSE_MS       220
#define BYE_PULSE_STRENGTH 0xAFFF
/* ⭐⭐ THREE FOR A REFUSAL, ONE FOR EVERYTHING ELSE (rhoquinn8217, 2026-09-15,
 * after feeling both): "We should 3 for the refusal as you had before. It odd
 * enough that it will signal that something is wrong, while a 1 buzz means ok
 * and we will keep for bridge and release."
 *
 * ⛔ IT WAS BRIEFLY ONE, and that is worth knowing rather than re-deciding: the
 * theory was that a count cannot be felt and told apart, so every signal should
 * buzz once. Felt on a pad, the opposite is true of the ODD one -- a single
 * pulse reads as "done", and a rumble that keeps going reads as "wrong" without
 * anyone counting it. ➡️ So the count is not a number to read. It is the
 * difference between a signal that stops and one that insists.
 *
 * ⓘ A bridge and a release each stay at one pulse. */
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
/* ⭐ THREE, MATCHING THE CORE (T-223). BTSIG_HANDED_BACK is "yellow, three
 * flashes", so a pad the TV paints for says exactly what a DualSense says.
 * Three is also clearly deliberate rather than a glitch. */
#define HANDBACK_FLASHES  3
/* ⭐ Two green flashes on a successful bridge, against three red on a refusal.
 * Green for done, red for refused -- the light says which without anyone
 * learning a vocabulary. Fewer than the refusal because success is the ordinary
 * case and does not need insisting on. */
/* One breath of green on a successful bridge, against three hard red flashes on
 * a refusal. Shorter than the yellow handback, which is two breaths -- a bridge
 * is the start of something and does not need dwelling on. */
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
/* ⓘ Returns what SDL said: 0 accepted, -1 refused or no light. Almost every
 * caller is an animation step and ignores it; the handback paint reads it, so a
 * colour that never lands is a line in the log. */
static int flash_write(SDL_GameController *controller, uint8_t r, uint8_t g, uint8_t b) {
    if (controller) {
        return SDL_GameControllerSetLED(controller, r, g, b);
    }
    return -1;
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
/* ⭐⭐ THE COLOUR A BRIDGED CONTROLLER IS HANDED OVER IN.
 *
 * ⛔ After a bridge the light is NOT ours. The controller belongs to the host,
 * and Windows and Steam paint it themselves -- measured 2026-08-07: plugging a
 * ds5 in leaves the lightbar TEAL.
 *
 * ⚠️ So repainting the player colour on the way out was a claim we do not have.
 * It also showed: our colour, then Steam's a moment later, a visible flip that
 * says the wrong thing about who owns the controller.
 *
 * ⭐ Painting it where the host is about to put it makes the handover
 * seamless. rhoquinn8217, 2026-08-19: "setting it early makes the flow cleaner."
 *
 * ⓘ THE EXACT VALUE IS APPROXIMATE. Teal was observed, not measured as an RGB
 * triple. If it ever visibly steps when Steam takes over, this is the number to
 * tune -- it is one line. */

static int paint_player_colour(SDL_GameController *controller) {
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
    return flash_write(controller, player_rgb[slot][0],
                       player_rgb[slot][1], player_rgb[slot][2]);
}

/* The player colour, remembered against the index it was painted for. */
static int paint_player_colour_for(watched_t *w, SDL_GameController *controller) {
    const int rc = paint_player_colour(controller);
    if (w) {
        w->painted_slot = SDL_GameControllerGetPlayerIndex(controller);
    }
    return rc;
}

/* A ramp that rises and falls: bright in the middle, dark at both ends, so it
 * reads as a breath rather than a blink. For a signal that ends on a colour of
 * its own, where the last step is not the thing being seen.
 *
 * `span` is the length of ONE breath, so a longer count simply repeats it. */
/* ⭐⭐ BRIGHTNESS FROM THE CLOCK, NOT FROM A STEP COUNTER.
 *
 * ⛔ The pulse used to count steps and sleep between them, which means the
 * SHAPE depended on the app's loop keeping time -- and that loop is also
 * drawing the interface. This file already warned about it: an earlier version
 * "froze at its brightest and stayed there, because it animated on the UI
 * thread and lost the competition for it."
 *
 * ⚠️ It was got wrong twice in one day, in opposite directions. Fifty
 * milliseconds a step looked smooth; ninety looked steppy; twenty looked like
 * the light was fighting something, because the loop cannot promise fifty
 * updates a second and the steps landed unevenly.
 *
 * ⭐ Asking the clock removes the problem instead of tuning it. A late pass
 * simply lands at the brightness that moment deserves, so jitter shifts nothing
 * -- and the shape is the same on a busy loop as an idle one. */


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

/* From an evdev node to the hidraw node of the same HID device.
 *
 * ⛔ THIS USED TO WALK ONE LEVEL SHORT. /sys/class/input/eventN/device is the
 * INPUT device (inputM); its parent is only the "input" folder, which holds no
 * hidraw at any depth. So no controller SDL read through evdev was ever joined
 * to its row, and each fell back to a plain plug that retired nothing.
 * ➡️ The input device's own `device` is the HID device, whose hidraw directory
 * names the node: the same walk the core makes from inputM. Read off the U5s
 * 2026-09-13: event9/device/device/hidraw holds hidraw0 (the DS4) and event13's
 * holds hidraw1 (the KMA2 keyboard). */
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

    /* ⓘ Followed through the links rather than resolved with realpath, which is
     * flaky inside the dev-mode jail. The hidraw number changes on every
     * re-enumeration, so it is read rather than remembered. */
    char hidraw_dir[PATH_MAX];
    snprintf(hidraw_dir, sizeof(hidraw_dir), "%s/device/hidraw", sys_input);
    DIR *hd = opendir(hidraw_dir);
    if (!hd) {
        return false;   /* no hidraw: an xpad pad, for one -- see below */
    }
    bool found = false;
    struct dirent *hent;
    while ((hent = readdir(hd)) != NULL) {
        if (strncmp(hent->d_name, "hidraw", 6) == 0) {
            snprintf(out, out_len, "/dev/%s", hent->d_name);
            found = true;
            break;
        }
    }
    closedir(hd);
    return found;
}

/* ⭐⭐ IS THE SDL CONTROLLER AT dev_path THE DEVICE BEHIND THE ROW node?
 *
 * The one join, used by the bridge request, the player lookup and the MAC
 * lookup, so the three cannot disagree about which controller a row is.
 *
 * 1. The same string: a DualSense, which SDL opens through hidraw.
 * 2. The hidraw node of the evdev node SDL reports.
 * 3. ⭐ A row that is itself an input node. A wired Xbox pad has no hidraw at
 *    all: its row is /dev/input/jsN and SDL reports its sibling eventN. Both
 *    are children of one input device, and eventN's `device` link IS that
 *    device, so the pair is the same pad exactly when
 *    /sys/class/input/eventN/device/jsN exists. Read off the U5s 2026-09-13:
 *    event12/device/js7 exists (the Series pad), event12/device/js8 does not
 *    (that is the One S pad's), and event17/device/js8 does. */
static bool controller_path_is_node(const char *dev_path, const char *node) {
    if (!dev_path || !dev_path[0] || !node || !node[0]) {
        return false;
    }
    if (strcmp(dev_path, node) == 0) {
        return true;
    }
    char found[64];
    if (hidraw_node_for_event(dev_path, found, sizeof(found))) {
        return strcmp(found, node) == 0;
    }
    if (strncmp(node, "/dev/input/", 11) != 0 || strncmp(dev_path, "/dev/input/event", 16) != 0) {
        return false;
    }
    char sibling[PATH_MAX];
    snprintf(sibling, sizeof(sibling), "/sys/class/input/%s/device/%s", dev_path + 11, node + 11);
    return access(sibling, F_OK) == 0;
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
        /* ⭐⭐ THE PLAYER COLOUR WHEN A CONTROLLER FIRST APPEARS, whether it is
         * connected while Aurora runs or was connected before Aurora started
         * (SDL reports both the same way).
         *
         * ⓘ The other places this side paints it: after a release and after a
         * refusal (both 2026-09-15, below), when SDL's player number arrives or
         * changes (just below), and when a stream ends
         * (ctm_bridge_gesture_restore_player_colours).
         * ⛔ Never after a BRIDGE. rhoquinn8217, 2026-08-19: on a controller the
         * host has just taken the colour is overwritten within a moment anyway,
         * "and it comes off like an error". A bridge ends on the core's green. */
        paint_player_colour_for(w, controller);
    }
    if (!w) {
        return false;
    }

    /* ⭐ AND AGAIN WHEN THE PLAYER NUMBER ARRIVES OR CHANGES (rhoquinn8217,
     * 2026-09-15): "When the controller is connected while aurora is up or if
     * the controller is already connected and aurora is started, the color
     * should be set to the player color."
     * ⚠️ SDL can give a controller its number after the first sight, and its
     * own PlayStation drivers repaint a dim colour of their own when it does, so
     * one paint at arrival could be blue for every pad, or overwritten. ⓘ Only
     * while the light is this side's: not bridged, no hold or pattern running,
     * and only on a change -- never every pass. */
    if (!w->ours_plugged && w->since == 0 && w->prep_left == 0 && w->flash_left == 0 &&
        SDL_GameControllerGetPlayerIndex(controller) != w->painted_slot) {
        paint_player_colour_for(w, controller);
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
    /* ⓘ The bye pulse is GONE. Every pattern is drawn by the core now, in the
     * same code that plays its tone, so this side animates nothing. See the
     * note where the bridge no longer arms a green. */

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
                /* ⭐⭐ THE PLAYER COLOUR AFTER A RELEASE (rhoquinn8217,
                 * 2026-09-15). The core's yellow ends dark, and a pad left dark
                 * read as "not connected". The plug-out, yellow included, has
                 * finished by the time the node reads unplugged, so this lands
                 * after it. ⓘ It also replaces the gesture's magenta as the
                 * colour SDL remembers, which SDL re-sends to a DS4 with every
                 * rumble. */
                /* ⭐⭐ YELLOW ON THE WAY BACK, FOR A PAD THE CORE WILL NOT
                 * PAINT (T-223). The core flashes yellow three times with its
                 * tone -- BTSIG_HANDED_BACK -- and it has no signal at all for
                 * a Bluetooth DS4, so that pad came back with nothing but the
                 * player colour. Same question as the pulse below: ask whether
                 * the core owns this signal, and paint it here when it does
                 * not. ⓘ The flash restores the player colour itself on its
                 * last step, so the two paths end in the same place. */
                const bool paint_here =
                    app_configuration && app_configuration->bridge_signal_light &&
                    gesture_signal_here(w->prep_node);
                if (paint_here) {
                    flash_arm(w, 0xff, 0xff, 0x00, HANDBACK_FLASHES);
                    gesture_log("handback colour on %s: three yellow flashes from here",
                                w->prep_node);
                } else {
                    const int paint_rc = paint_player_colour_for(w, controller);
                    gesture_paint_logged(controller, "handback colour", w->prep_node,
                                         paint_rc);
                }
                /* ⭐ AND A RUMBLE FOR A PAD WITH NO LIGHT TO PAINT. The line
                 * above puts a colour back on a controller that has one; an
                 * Xbox pad has none, no speaker either, and no core signal of
                 * its own, so until now it was handed back with no sign at all.
                 * ⓘ Under the user's rumble switch, like every other signal,
                 * and shaped to be told apart by feel: see BYE_PULSE_MS. */
                /* ⛔⛔ ASK THE QUESTION THE CONFIRMATION PULSE ASKS (T-212).
                 *
                 * `!SDL_GameControllerHasLED` was a stand-in for "this pad has
                 * no other sign to give", and it is wrong for a Bluetooth DS4:
                 * it HAS a light, the core has no signal for it, and the colour
                 * put back above is not something a hand feels.
                 *
                 * ⚠️ Measured on the monitor 2026-09-18 -- it rumbled on the
                 * bridge and was handed back in silence, while the Xbox pad beside
                 * it did both. ⭐ A DualSense still says nothing here, because
                 * gesture_signal_here() is false for it: the core does sing.
                 *
                 * ⓘ gesture_signal_here() folds in ctm_bridge_signals_enabled(),
                 * so the switch that used to be tested on this line still is. */
                if (controller && app_configuration &&
                    app_configuration->bridge_signal_rumble &&
                    gesture_signal_here(w->prep_node)) {
                    gesture_rumble_logged(controller, "handback pulse", w->prep_node,
                                          BYE_PULSE_STRENGTH, BYE_PULSE_MS);
                } else if (controller) {
                    /* ⭐ Say why, so a silent handback is a line in the log
                     * rather than a question for the next hardware run. */
                    gesture_log("handback pulse on %s: NOT SENT BY US -- %s",
                                w->prep_node,
                                (app_configuration &&
                                 app_configuration->bridge_signal_rumble)
                                    ? "the core claims the signal here"
                                    : "the bridge rumble switch is off");
                }
                /* ⛔⛔ ON BLUETOOTH THE CORE ALWAYS CLAIMS THE SIGNAL, AND
                 * WITH BT_LAYER_CORE_SIGNAL OFF IT THEN DOES NOTHING.
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
                /* ⛔⛔ THE LIGHT IS ALWAYS OURS NOW. Changed 2026-08-19.
                 *
                 * This used to ask whether the core would signal the unplug,
                 * and skip the yellow pulse if it would -- because the core's
                 * Bluetooth signal painted the lightbar itself.
                 *
                 * ⚠️ IT NO LONGER DOES. Claiming the light was removed from it
                 * earlier the same day: it was landing after the app's own
                 * pattern and then staying, and it was claiming the lightbar
                 * and player LEDs on all 122 reports with the colour bytes at
                 * zero, which is what made the patterns flicker.
                 *
                 * ⭐ The wired signal never painted the light at all -- it is
                 * audio and haptics only. So on BOTH transports the core now
                 * carries the SOUND and the FEEL, and every colour on this
                 * controller comes from here.
                 *
                 * ⛔ The symptom when this was still asking: `pulsing yellow`
                 * in the log, and no `unplug pulse finished` ever after it.
                 * Armed, then given zero steps. */
                /* ⭐⭐ ON BLUETOOTH THE CORE PAINTS THIS ONE, AND IT IS ON TIME.
                 *
                 * The unbridge chord is detected in the CORE -- a bridged
                 * controller's touchpad reports come through it, so this side
                 * is blind to them. We only learn of the unplug from the
                 * plugged-check, a second or more after the core's tone has
                 * already finished. ⛔ A yellow pulse armed here is always
                 * LATE, which is exactly how it looked: the sound, then the
                 * colour afterwards.
                 *
                 * ⭐ So the core carries the light with the sound, and this
                 * side only puts the player colour back once it catches up.
                 *
                 * ⚠️ A BRIDGE IS THE OPPOSITE and the core must NOT paint it --
                 * that signal runs after the bridge completes and lands on top
                 * of our green. Both directions were got wrong before this was
                 * settled. */
                /* ⭐ THE LIGHT IS THE CORE'S ON BOTH TRANSPORTS. It detects
                 * the unbridge chord -- a bridged controller's touchpad reports
                 * come through it -- so it knows first and paints yellow with
                 * the tone. This side does nothing at all now: it learns of the
                 * unplug a second later from its plugged-check, and the player
                 * colour is no longer restored after a pattern. */
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
                /* ⚠️ This said "pulsing yellow" and drew nothing: the paragraphs
                 * above had already moved the colour to the core, and the line
                 * was left behind describing a pulse that no longer existed. */
                gesture_log("%s came back to us", w->prep_node);
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
                    /* ⓘ T-120 gated this rumble on Bluetooth while its layers
                     * were rebuilt; the gate is gone and the layer is on. */
                    /* ⛔⛔ THE SAME TRAP AS THE BYE PULSE. On Bluetooth
                     * gesture_signal_here() is always false -- the core claims
                     * the signal there -- so this never fired, gate or no gate.
                     * BT_LAYER_RUMBLE could not do anything while
                     * BT_LAYER_CORE_SIGNAL was 0. Found in step 4, 2026-08-18.
                     *
                     * ⭐ Ask whether the core will signal AND is switched on.
                     *
                     * ⏱️ TIMED: gesture_signal_here() ends in a full device
                     * enumeration and was measured at 5144ms on the C3 against
                     * 0 on the monitor. The number is logged so a slow one is
                     * seen rather than felt. */
                    uint64_t sig_t0 = gesture_now_ms();
                    const bool core_signals_ok = !gesture_signal_here(w->prep_node);
                    const uint64_t sig_ms = gesture_now_ms() - sig_t0;
                    gesture_log("confirmation pulse on %s: bridged (signal check %llums)",
                                w->prep_node, (unsigned long long) sig_ms);
                    if (!core_signals_ok) {
                        gesture_rumble_logged(controller, "confirmation pulse",
                                              w->prep_node, OK_PULSE_STRENGTH,
                                              OK_PULSE_MS);
                    } else {
                        /* ⛔ NOT a pulse. The core is expected to signal this pad
                         * instead -- and whether it does for a Bluetooth XBOX pad
                         * is exactly what T-212 has never established. */
                        gesture_log("confirmation pulse on %s: NOT SENT BY US -- the core "
                                    "claims the signal here (transport=%s)",
                                    w->prep_node, w->xport == 1 ? "bluetooth" : "wired");
                    }
                    w->ours_plugged = true;
                    w->plug_miss = 0;
                    w->plug_check_next = SDL_GetTicks() + PLUG_CHECK_MS;
                    /* A CONTROLLER IS NOW BRIDGED, so warn if the TV's own
                     * mouse mode is on (T-170). It will go on driving the host
                     * cursor, which is DELIBERATE -- without it a bridged pad
                     * cannot reach the settings page to choose a listener
                     * preset, and you would need a second device -- but it
                     * surprises anyone who then sets one.
                     *
                     * Only controllers reach this function at all: a keyboard
                     * or mouse has no SDL controller behind its node and takes
                     * the direct plug instead. So every path that bridges a
                     * CONTROLLER passes here -- the chord, a panel row, Auto
                     * Bridge and the control port -- and nothing else does.
                     *
                     * Posted rather than called: this file includes no UI
                     * headers, and whether mouse mode is on is the streaming
                     * controller's to answer. Same mechanism as the overlay
                     * request below. */
                    gesture_log("mouse-mode warning: posting the event");
                    bus_pushevent(USER_CTM_MOUSE_MODE_WARN, NULL, NULL);
                    /* ⭐⭐ GREEN ON SUCCESS, FOR A PAD THE CORE WILL NOT PAINT
                     * (T-223). The paragraph below is still true for a
                     * DualSense -- the core draws its green with its tone, so
                     * the two arrive together -- but the core has no signal for
                     * a Bluetooth DS4, which left the only lit pad in the set
                     * going magenta straight back to its player colour, saying
                     * nothing about whether the bridge worked.
                     * ⓘ Steady, not a pattern: the host claims the light within
                     * a moment of this and would cut a flash sequence in half. */
                    if (core_signals_ok == false && app_configuration &&
                        app_configuration->bridge_signal_light) {
                        const int green_rc = flash_write(controller, 0x00, 0xff, 0x00);
                        gesture_paint_logged(controller, "bridged green",
                                             w->prep_node, green_rc);
                        w->painted_slot = PAINTED_NEVER;   /* not the player colour now */
                    }
                    /* ⛔ PUT THE LIGHT BACK. The pre-plug pulse leaves magenta
                     * and walks away: nothing after it repaints, so the light
                     * stayed magenta until Steam claimed it -- long after the
                     * controller was already working. Measured in step 3,
                     * 2026-08-18.
                     *
                     * ⭐ The unplug path has done this all along; the plug path
                     * never did, because on a cable the core's own signal
                     * repainted it and hid the gap. */
                    /* ⛔⛔ NO GREEN FROM THIS SIDE. Changed 2026-08-19.
                     *
                     * ⚠️ It used to be drawn here, the moment the plug call
                     * returned -- about a SECOND before the core plays the
                     * confirmation tone. The light and the sound never arrived
                     * together, and no amount of tuning the breath could fix
                     * that, because they were triggered by different events.
                     *
                     * ⭐ The core paints it now, in the same code that plays the
                     * tone, so they are simultaneous by construction. That
                     * makes every signal one rule: green, yellow and red all
                     * come from the core, each with its own sound, on both
                     * transports -- and this side only ever puts the player
                     * colour back.
                     *
                     * ⓘ The green WILL stutter, because a bridge completing is
                     * the moment the emulated pad retires and the real
                     * controller arrives on the host. Knowingly traded for the
                     * timing; see the note in ctm_bt_signal.inl. */
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
                            /* ⭐ The core lit it, and its Bluetooth signal has
                             * finished by the time the call returns. Then the
                             * player colour (rhoquinn8217, 2026-09-15), where a
                             * refused controller used to keep whatever the last
                             * flash left. */
                            paint_player_colour_for(w, controller);
                        } else {
                            /* ⭐ The user's switch. ⓘ The refusal is the one
                             * worth being loudest about, so it is gated last
                             * and independently of the others. */
                            if (app_configuration->bridge_signal_light)
                                flash_arm(w, 0xff, 0x00, 0x00, REFUSED_FLASHES);
                        }
                        if (!sounded) {
                            if (app_configuration->bridge_signal_rumble)
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
             * reads as a breath rather than a blink.
             *
             * ⓘ Stays MAGENTA the whole way. Turning it green halfway was tried
             * on 2026-08-19 and rejected by rhoquinn8217: green before the plug means
             * "gesture accepted", and a refusal 70 ms later contradicts it.
             * ⭐ Green after the plug only ever shows on success and red only
             * on failure, so the two can never disagree. */
            /* ⭐ Magenta-while-asking is a lightbar signal like any other. */
            if (app_configuration->bridge_signal_light) {
                uint8_t level = pulse_level_rising(w->prep_left);
                flash_write(controller, level, 0, level);   /* red + blue = magenta */
            }
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
                gesture_log("outcome flash finished on %s", w->prep_node);
                /* ⭐ The restore step the count always kept room for: the
                 * player colour after the red (rhoquinn8217, 2026-09-15). Its
                 * last flash was lit, so without this the pad stayed red. */
                paint_player_colour_for(w, controller);
            } else {
                /* Odd counts are the lit ones, so the LAST flash step is lit
                 * rather than an unlit one nobody sees. */
                bool lit = (w->flash_left % 2) == 1;
                /* ⭐ RED REFUSES, YELLOW HANDS BACK, and they are never the
                 * same pad at the same moment: yellow says "the controller is
                 * yours again because you asked", red says "it is still yours
                 * because the bridge failed". ⓘ Which one this is was decided
                 * by whoever armed it; this tick only draws.
                 *
                 * ⚠️ Success is NOT a flash. It is a steady green, painted
                 * where the plug completes, because the host claims the light
                 * moments later and a pattern would be cut in half. */
                flash_write(controller,
                            lit ? w->flash_r : 0x00,
                            lit ? w->flash_g : 0x00,
                            lit ? w->flash_b : 0x00);
                w->flash_next = now_ticks + (lit ? REFUSED_ON_MS : REFUSED_OFF_MS);
            }
        }
        return false;
    }

    /* Once per connection, before anything else: make sure a DualSense is
     * sending its full report, or there is no touchpad to read.
     *
     * ⛔ A DUALSENSE OR AN EDGE ONLY. Feature report 0x05 is the DualSense's
     * request, and this loop polls every controller SDL opened. Sent to all of
     * them, the C1's log showed a Switch Pro Controller refusing it and a
     * GameSir in PlayStation mode answering it, on every app start
     * (2026-09-16). Nothing broke that time; a request meant for one device
     * reaching others is how the listener's microphone guard did break the
     * Pro Controller's handshake. */
    if (!w->asked_full) {
        w->asked_full = true;
        const char *path = SDL_GameControllerPath(controller);
        if (controller_is_dualsense(controller)) {
            request_full_report(path);
        } else {
            gesture_log("full-report request skipped on %s: not a DualSense (%04x:%04x)",
                        path ? path : "-", SDL_GameControllerGetVendor(controller),
                        SDL_GameControllerGetProduct(controller));
        }
    }

    /* ⭐⭐ THE GESTURE SWITCH BELONGS HERE, NOT AT THE TOP OF THE TICK.
     *
     * ⛔ It was on the tick itself, and that broke the PANEL: the panel only
     * ASKS for a bridge -- the pulse, the plug and the flashes all run from
     * this poll, so returning early left the request armed and never finished.
     * rhoquinn8217, 2026-08-20: "it just flashes, looks broken and unresponsive."
     *
     * ⭐ Everything above this point is machinery that must keep running
     * whatever the setting says. Only what follows -- noticing a new chord --
     * is the gesture. */
    if (app_configuration &&
        !(app_configuration->bridge_enable && app_configuration->bridge_gesture)) {
        w->since = 0;
        w->fired = false;
        return false;
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
    w->prep_left = !ctm_bridge_signals_enabled()
                       ? 1 : PREP_STEPS;
    w->prep_next = SDL_GetTicks();
    gesture_log("fired on %s -> %s : pulsing before handover", dev_path, node);
    return false;
}


/* The gamepad list, kept so a panel request can find a controller by node.
 * Set on every tick; NULL before the first one. */
static app_input_t *s_gesture_input = NULL;

/* ⭐⭐ PUT THE PLAYER COLOUR BACK ON EVERY CONTROLLER WE ARE WATCHING.
 *
 * ⓘ Called at the two moments a controller comes back to the TV's own world:
 * when a stream disconnects, and when Aurora is switched away from mid-stream.
 * The rest -- a controller first appearing, its player number arriving, a
 * release and a refusal -- are handled in gesture_poll_one.
 *
 * ⛔ NEVER AFTER A BRIDGE. It used to be restored after every pattern, which
 * rhoquinn8217 ruled out on 2026-08-19: on a controller the host has just
 * taken, the colour is overwritten within a moment anyway and "comes off like
 * an error". ⓘ A release and a refusal came back on 2026-09-15: that light is
 * the TV's again, and dark read as "not connected".
 * ⓘ The notes below about a pad released mid-stream staying dark predate that:
 * the release paint in gesture_poll_one now covers it.
 *
 * ⭐⭐ A CABLED DUALSHOCK 4 GETS THESE SAME COLOURS, AND NEEDS NOTHING OF ITS OWN.
 * rhoquinn8217, 2026-09-15: "End Dark when in stream. In Aurora, used the colors
 * we decide for DS5". Read through the code that day, not yet watched:
 * - Nothing here or in gesture_poll_one asks what the controller is, and SDL's
 *   PS4 driver takes a lightbar colour on a cable just as its PS5 driver does.
 * - The DS4's release signal ends DARK. At the end of a stream it plays inside
 *   ctm_bridge_stop(), which session_stop_input calls just before this and
 *   which returns only once each release has played, so this colour lands
 *   after the dark -- the same order as a wired DualSense's yellow, which also
 *   ends at zero.
 * - In a stream nothing repaints, so a pad released there stays dark until the
 *   host paints the pad Moonlight hands back to it.
 * ⚠️ Except a release the chord began just before the stream ended: that runs on
 * the core's gesture worker, which ctm_bridge_stop() does not wait for, so its
 * dark can land after this colour.
 *
 * ⚠️ ONE WAY A DS4 DIFFERS, read in SDL 2.30.12's source and not measured. Its
 * PS4 driver sends the lightbar in EVERY effects report, rumble included, so
 * any rumble SDL sends a DS4 also re-sends the last colour SDL was given -- and
 * SDL repeats a running rumble every 2 s, for up to 65 s. After a bridge with
 * the light switch on, that colour is the pre-plug pulse's magenta until the
 * host sets another. ➡️ So a game's rumble through Moonlight could light a DS4
 * released mid-stream magenta, and a rumble running when a DS4 was bridged
 * could keep reaching it. A DualSense's rumble leaves its lightbar alone. */
void ctm_bridge_gesture_restore_player_colours(void) {
    for (int i = 0; i < MAX_WATCHED; ++i) {
        if (!s_watched[i].in_use) continue;
        SDL_GameController *gc = SDL_GameControllerFromInstanceID(s_watched[i].id);
        if (gc) paint_player_colour_for(&s_watched[i], gc);
    }
}

/* Is this side drawing a pattern on that controller right now?
 *
 * ⭐ Used to drop the HOST's lightbar writes while we draw -- see the note in
 * app_input_gamepad_set_controller_led. */
bool ctm_bridge_gesture_light_busy(SDL_GameController *controller) {
    if (!controller) return false;
    SDL_Joystick *js = SDL_GameControllerGetJoystick(controller);
    if (!js) return false;
    watched_t *w = watched_for(SDL_JoystickInstanceID(js));
    if (!w) return false;
    bool busy = w->prep_left > 0 || w->flash_left > 0;

    /* ⭐ COUNTED, because "it still flickers" cannot say whether the gate ran.
     *
     * ⛔ This is called only from the lightbar forward, so a true here IS a
     * dropped host colour. Silence in the log means the host's colour does not
     * arrive down the stream at all -- and with the relay's own hold logging
     * separately, BOTH quiet would mean the writes come from somewhere we do
     * not route: most likely the kernel's own PlayStation driver, which no
     * gate of ours can reach.
     *
     * ⓘ Once per app run, and it lands in logs/ctm-gesture.log beside the
     * core's own lines. */
    if (busy) {
        static int s_dropped;
        if (!s_dropped) {
            s_dropped = 1;
            gesture_log("lightbar: dropping the host's colour while a pattern plays");
        }
    }
    return busy;
}

/* --- T-179: a controller the core can see that SDL never heard about --------
 *
 * ⭐⭐ THE FAULT, MEASURED ON THE C3 2026-09-21. A pad is connected, the kernel
 * enumerates it, the core's scan lists it and it bridges -- and Aurora has no
 * input from it, no overlay chord, nothing, until it is unplugged and plugged
 * in again.
 *
 * ⛔ THE CAUSE IS AN ARRIVAL THAT NEVER LANDED. SDL takes the udev monitor path
 * on webOS (libudev is present and systemd-udevd runs), and that path has no
 * poll behind it: one dropped event is permanent. The core does not care,
 * because everything it does is a scan -- which is exactly why the panel can
 * list a pad the app cannot feel.
 *
 * ➡️ So this watches for the DISAGREEMENT rather than for a cause: a controller
 * the core can see, with no SDL player, is the fault by definition, however it
 * got there. That covers the two cases no log could tell apart -- a pad that
 * was never registered, and one that registered and quietly fell out.
 *
 * ⭐ WHEN IT LOOKS (rhoquinn8217, 2026-09-21: *"you should just check whenever
 * a device is connected or when the app starts"*). Not a poll:
 *   - once, shortly after the app starts, and
 *   - whenever a device node APPEARS, which is watched with inotify on /dev and
 *     /dev/input.
 * ⓘ inotify rather than udev on purpose: udev is the thing that dropped the
 * event, and the kernel creates the node either way. It is also what SDL's own
 * fallback uses on this platform, so it is proven here. One fd, no timer, and
 * nothing at all while the TV sits idle.
 *
 * ⛔⛔ AND IT DOES NOT DISTURB WHAT IS WORKING (rhoquinn8217, 2026-09-21:
 * *"don't disrupt any currently connected devices"*). The only way to make SDL
 * find a device it missed is to restart its joystick subsystem, which closes
 * and reopens EVERY pad's handle -- so that is done only when the broken pad is
 * the only one there is: no other controller open, nothing bridged. Otherwise
 * the state is logged and left alone, and the pad is reconnected by hand as
 * before. ⓘ A bridged pad's emulated twin on the host is retired by SDL player
 * index; shuffling those to rescue a pad nobody is holding would trade a known
 * fault for a worse one.
 *
 * ⓘ Logged into the core's own file rather than Aurora's, because the C3 and
 * the C1 have no /var/log/dbg-log at all and this has to be readable on every
 * set. */
#define ARRIVAL_SETTLE_MS   4000    /* after a node appears: SDL is allowed its own chance first */
#define ARRIVAL_START_MS    8000    /* after the app starts */

static int s_arrival_inotify = -1;
static uint32_t s_arrival_due;      /* SDL ticks at which to look; 0 = nothing pending */
static bool s_arrival_started;
/* ⭐ After a cure, the node it was for -- so the NEXT look can say whether it
 * worked. A fix nobody can confirm is a claim. */
static char s_arrival_verify[64];
/* 1 after upstream's scan, 2 after the driver re-announce. */
static int s_arrival_stage;

static void arrival_watch_init(void) {
    s_arrival_started = true;
    s_arrival_due = SDL_GetTicks() + ARRIVAL_START_MS;
    s_arrival_inotify = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (s_arrival_inotify < 0) {
        gesture_log("arrival watch: no inotify (%s) -- the check runs at start only", strerror(errno));
        return;
    }
    /* ⭐⭐ SAID ON EVERY START, because this watch is otherwise SILENT when
     * all is well -- and silence cannot be told from a watch that never ran.
     * One line at the top of each session is what makes the quiet afterwards
     * mean something. */
    gesture_log("arrival watch: armed -- watching /dev and /dev/input, first look in %u ms",
                (unsigned) ARRIVAL_START_MS);
    /* ⓘ Both directories: a pad arrives as a hidraw node and as js/event nodes,
     * and which one appears first is not ours to predict. IN_CREATE only --
     * a node going away is the ordinary path and needs nothing from us. */
    if (inotify_add_watch(s_arrival_inotify, "/dev", IN_CREATE) < 0) {
        gesture_log("arrival watch: cannot watch /dev (%s)", strerror(errno));
    }
    if (inotify_add_watch(s_arrival_inotify, "/dev/input", IN_CREATE) < 0) {
        gesture_log("arrival watch: cannot watch /dev/input (%s)", strerror(errno));
    }
}

/* True when a node that could be a controller has just appeared. Drains the
 * queue either way, so a burst of nodes from one dongle is one wake-up. */
static bool arrival_node_appeared(void) {
    if (s_arrival_inotify < 0) return false;
    char buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
    bool interesting = false;
    for (;;) {
        const ssize_t len = read(s_arrival_inotify, buf, sizeof buf);
        if (len <= 0) break;
        for (char *p = buf; p < buf + len;) {
            const struct inotify_event *ev = (const struct inotify_event *) p;
            if (ev->len > 0) {
                if (strncmp(ev->name, "hidraw", 6) == 0 || strncmp(ev->name, "js", 2) == 0 ||
                    strncmp(ev->name, "event", 5) == 0) {
                    interesting = true;
                }
            }
            p += sizeof(struct inotify_event) + ev->len;
        }
    }
    return interesting;
}

/* ⭐⭐ THE GENTLE CURE, and the one that will run almost every time
 * (rhoquinn8217, 2026-09-21, after the strict gate refused on the rooted
 * monitor because two Xbox pads sit connected there permanently).
 *
 * SDL claims a DualSense, an Edge and a DS4 through its HIDAPI drivers rather
 * than evdev -- `app.c` sets SDL_HINT_JOYSTICK_HIDAPI_PS5 -- and a driver
 * switched off and on again re-examines the devices it could claim.
 * ➡️ So a PlayStation pad can be made to announce itself WITHOUT touching
 * anything else: an Xbox pad on evdev never notices, bridged or not.
 *
 * ⓘ Which hint depends on the pad, so a missing DualSense does not disturb a
 * DS4 that is perfectly happy. ⚠️ The core's kind strings are cut to 8 bytes
 * ("ds5e_us"), hence the prefix test. */
static const char *arrival_hidapi_hint(const char *kind) {
    if (kind == NULL) return NULL;
    if (strncmp(kind, "ds5", 3) == 0) return SDL_HINT_JOYSTICK_HIDAPI_PS5;
    if (strncmp(kind, "ds4", 3) == 0) return SDL_HINT_JOYSTICK_HIDAPI_PS4;
    return NULL;   /* an Xbox or generic pad is on evdev; only the full pass reaches it */
}

static void arrival_rescan_driver(const char *hint) {
    SDL_SetHint(hint, "0");
    SDL_SetHint(hint, "1");
    gesture_log("arrival watch: asked %s to look again -- nothing else is touched", hint);
}


/* The look itself. Runs on the main thread, from the tick. */
/* The look itself, and the cure, in the order that disturbs least.
 *
 * ⭐⭐ UPSTREAM v1.2.10 ARRIVED WITH ITS OWN FIX FOR THIS FAULT, and it is a
 * better first move than anything here was: `app_input_scan_gamepads()` walks
 * SDL's joystick list and opens whatever the app is not already tracking.
 * Nothing that is working is touched, no subsystem is restarted, and there is
 * no state to get wrong -- so it needs no gate at all.
 *
 * ⓘ His reading of the cause differs from ours and is at least as likely:
 * SDL DOES see the pad and surfaces only SDL_CONTROLLERDEVICEADDED, which this
 * app ignored while acting on SDL_JOYDEVICEADDED alone. He opens from that
 * event too. Where that is what happened, his scan finds the pad; where the
 * arrival never reached SDL at all, it does not, and the mark below is what
 * says which of the two we are looking at.
 *
 * ➡️ So: scan first, look again, and only if the pad is STILL unknown ask its
 * HIDAPI driver to re-examine what it can claim. That second step is ours, and
 * it is the one with a gate, because it re-announces every pad on that driver.
 * ⛔ The subsystem restart that used to sit at the end is gone (2026-09-21,
 * merging v1.2.10): it was the only thing here that could disturb a pad in
 * play, and the two steps above cover what it covered. */
static void arrival_check(struct app_input_t *input) {
    /* ⛔⛔ THE QUIET LIST, ALWAYS. The full one wakes the bridge core and
     * broadcasts for a listener -- the settings pane uses the quiet one for
     * exactly that reason -- and this runs a few seconds after ANY device
     * node appears, which includes replugging a dongle while a pad is
     * bridged. A watchdog reads what is already known, and nothing else. */
    ctm_bridge_dev_t devs[16];
    const int n = ctm_bridge_list_quiet(devs, 16);
    if (n <= 0) return;

    bool bridged = false;
    int controllers = 0, matched = 0;
    for (int i = 0; i < n; ++i) {
        if (devs[i].plugged) bridged = true;
        if (!devs[i].controller || devs[i].node[0] == '\0') continue;
        ++controllers;
        if (ctm_bridge_gesture_player_for_node(devs[i].node) >= 0) ++matched;
    }

    /* ⭐ DID THE LAST CURE WORK? Asked on the look after it ran, and answered
     * either way -- the line that turns "it should recover" into something
     * readable off a set nobody was watching. */
    if (s_arrival_verify[0] != '\0') {
        char node[64];
        snprintf(node, sizeof node, "%s", s_arrival_verify);
        const int player = ctm_bridge_gesture_player_for_node(node);
        const int stage = s_arrival_stage;
        s_arrival_verify[0] = '\0';
        s_arrival_stage = 0;
        if (player >= 0) {
            gesture_log("arrival watch: %s is player %d now -- %s worked", node, player,
                        stage == 1 ? "the scan" : "the driver re-announce");
            return;
        }
        gesture_log("arrival watch: %s still has no player after %s", node,
                    stage == 1 ? "the scan" : "the driver re-announce");
        /* ⭐ A scan that found nothing means SDL never had the pad either, which
         * is the harder half of the fault. Escalate once, to the driver. */
        if (stage == 1) {
            const ctm_bridge_dev_t *d = NULL;
            for (int i = 0; i < n; ++i) {
                if (strcmp(devs[i].node, node) == 0) d = &devs[i];
            }
            const char *hint = d != NULL ? arrival_hidapi_hint(d->kind) : NULL;
            if (hint == NULL) {
                gesture_log("arrival watch: nothing left to try for %s -- reconnect it", node);
                return;
            }
            bool same_driver_bridged = false;
            for (int k = 0; k < n; ++k) {
                if (!devs[k].plugged || !devs[k].controller) continue;
                const char *other = arrival_hidapi_hint(devs[k].kind);
                if (other != NULL && strcmp(other, hint) == 0) same_driver_bridged = true;
            }
            if (same_driver_bridged) {
                gesture_log("arrival watch: leaving it alone -- another pad on that driver is "
                            "bridged and in play. Reconnect the controller to recover it");
                return;
            }
            snprintf(s_arrival_verify, sizeof s_arrival_verify, "%s", node);
            s_arrival_stage = 2;
            arrival_rescan_driver(hint);
            s_arrival_due = SDL_GetTicks() + 3000;
            return;
        }
        gesture_log("arrival watch: %s needs a reconnect by hand", node);
        return;
    }

    /* ⓘ One line per look, healthy or not. A look happens only after a device
     * node appears or at start, so it is rare -- and it is the proof the watch
     * is running at all. */
    gesture_log("arrival watch: looked -- %d device(s), %d controller(s), %d matched%s",
                n, controllers, matched, bridged ? ", something bridged" : "");
    if (controllers == matched) return;

    for (int i = 0; i < n; ++i) {
        const ctm_bridge_dev_t *d = &devs[i];
        if (!d->controller || d->node[0] == '\0') continue;
        if (ctm_bridge_gesture_player_for_node(d->node) >= 0) continue;

        gesture_log("arrival watch: %s (%s) on %s has no SDL player", d->name, d->kind, d->node);

        /* ⛔ A PAD THAT IS BRIDGED IS NOT THE ONE TO RESCUE. It already works
         * for the game -- the PC has the real controller -- and what it lacks is
         * only this side's chord and menus. */
        if (d->plugged) {
            gesture_log("arrival watch: %s is bridged, so the PC has it -- leaving it alone",
                        d->node);
            continue;
        }

        /* ⭐ Upstream's scan: it opens what SDL has and the app has not, and
         * disturbs nothing at all, so nothing gates it. */
        const int opened = app_input_scan_gamepads(input);
        gesture_log("arrival watch: scanned SDL's joysticks, %d opened", opened);
        snprintf(s_arrival_verify, sizeof s_arrival_verify, "%s", d->node);
        s_arrival_stage = 1;
        s_arrival_due = SDL_GetTicks() + 3000;
        return;
    }
}

/* Called from the tick. Cheap: a read on one fd, and a look only when something
 * has just appeared or the app has just started. */
static void arrival_watch_tick(struct app_input_t *input) {
    if (input == NULL) return;
    if (!s_arrival_started) {
        arrival_watch_init();
        return;
    }
    const uint32_t now = SDL_GetTicks();
    if (arrival_node_appeared()) {
        s_arrival_due = now + ARRIVAL_SETTLE_MS;
    }
    if (s_arrival_due == 0 || (int32_t) (now - s_arrival_due) < 0) return;
    s_arrival_due = 0;
    arrival_check(input);
}

/* The open SDL controller sitting on this core node, or NULL. */
static SDL_GameController *gesture_controller_for_node(const char *node) {
    if (!node || !node[0] || !s_gesture_input) {
        return NULL;
    }
    int n = (int) app_input_get_max_gamepads(s_gesture_input);
    for (int i = 0; i < n; ++i) {
        SDL_GameController *gc = s_gesture_input->gamepads[i].controller;
        if (!gc) continue;
        if (!controller_path_is_node(SDL_GameControllerPath(gc), node)) continue;
        return gc;
    }
    return NULL;
}

int ctm_bridge_gesture_player_for_node(const char *node) {
    SDL_GameController *gc = gesture_controller_for_node(node);
    return gc != NULL ? SDL_GameControllerGetPlayerIndex(gc) : -1;
}

/* ⛔⛔ THE EXCLUSION DOES NOT SURVIVE A STREAM STOP, AND A BRIDGE DOES.
 *
 * `moonlightExcludedMask` is what stops the app sending a bridged controller's
 * input to the host as well as the bridge sending the pad itself. It is set
 * when the bridge takes a controller and CLEARED WHOLESALE by
 * session_input_stopped(). ⚠️ So: stream up, pad bridged, stream stopped,
 * stream started again -- and that pad is no longer excluded, while the bridge
 * still owns it. Both halves then move the mouse, and the host sees the pad
 * twice.
 *
 * ➡️ The mask cannot be trusted across a session, so at session start it is
 * rebuilt from the thing that IS still true: which controllers the core says
 * are bridged. ⓘ Cheap, and it runs once per stream.
 *
 * ⓘ Found 2026-09-21 while answering "can Aurora's touchpad and a bridged
 * touchpad both run at once" -- they cannot, EXCEPT through this hole. */
void ctm_bridge_gesture_reexclude_bridged(void) {
    if (!s_stream_input || !s_gesture_input) {
        return;
    }
    ctm_bridge_dev_t devs[16];
    const int n = ctm_bridge_list_quiet(devs, 16);
    int done = 0;
    for (int i = 0; i < n; ++i) {
        if (!devs[i].plugged || !devs[i].controller || !devs[i].node[0]) continue;
        SDL_GameController *gc = gesture_controller_for_node(devs[i].node);
        if (gc == NULL) continue;
        gesture_moonlight_set_excluded(gc, true);
        ++done;
    }
    if (done > 0) {
        gesture_log("session start: re-excluded %d bridged controller(s) -- "
                    "the mask does not survive a stop", done);
    }
}

bool ctm_bridge_gesture_mac_for_node(const char *node, char *out, size_t out_len) {
    if (!node || !node[0] || !out || out_len == 0 || !s_gesture_input) {
        return false;
    }
#if SDL_VERSION_ATLEAST(2, 0, 14)
    int n = (int) app_input_get_max_gamepads(s_gesture_input);
    for (int i = 0; i < n; ++i) {
        SDL_GameController *gc = s_gesture_input->gamepads[i].controller;
        if (!gc) continue;
        if (!controller_path_is_node(SDL_GameControllerPath(gc), node)) continue;
        const char *serial = SDL_JoystickGetSerial(SDL_GameControllerGetJoystick(gc));
        if (!serial || !serial[0]) return false;
        snprintf(out, out_len, "%s", serial);
        return true;
    }
#else
    (void) out_len;
#endif
    return false;
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
        if (!controller_path_is_node(SDL_GameControllerPath(gc), node)) {
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
    w->prep_left = !ctm_bridge_signals_enabled()
                       ? 1 : PREP_STEPS;
        w->prep_next = SDL_GetTicks();
        w->fired = true;
        gesture_log("panel asked to bridge %s : pulsing before handover", node);
        return true;
    }
    gesture_log("panel asked to bridge %s, but no controller is behind it", node);
    return false;
}

/* ⭐ A bridged keyboard pressed Ctrl+Alt+Shift+O (rhoquinn8217, 2026-09-13).
 * When: the keyboard's input thread, so it only posts: the overlay opens on the
 * main thread, exactly as it does for a keyboard the TV reads. */
static void gesture_overlay_requested(void) {
    bus_pushevent(USER_OPEN_OVERLAY, NULL, NULL);
}

void ctm_bridge_gesture_tick(struct app_input_t *input, struct session_t *session,
                             bool overlay_open) {
    /* ⭐ T-179: has a controller arrived that SDL never heard about? See the
     * long note beside arrival_watch_tick. Cheap, and silent until a device
     * node appears. */
    arrival_watch_tick(input);
    {
        static bool s_overlay_request_set = false;
        if (!s_overlay_request_set) {
            bridge_set_overlay_request(gesture_overlay_requested);
            s_overlay_request_set = true;
        }
    }
    /* ⭐⭐ RELEASE ANYTHING WHOSE HOST HAS GONE. T-127, 2026-08-23.
     *
     * ⛔ Close the listener's window and the controller used to stay claimed by
     * a host that no longer exists. The core gives up after fifteen seconds and
     * raises a flag; this is the safe thread that acts on it, because the core
     * cannot release itself -- stopping a session joins the session thread.
     *
     * ⭐ Nothing else is needed: once the session stops, the device reports
     * unplugged, and the watcher further down does the full app-side release --
     * the pad retired, moonlight restored, the row updated, the yellow pulse.
     *
     * ⚠️ FIRST IN THE TICK, deliberately. Everything below reasons about which
     * controllers are bridged, and a controller whose host has gone is not one.
     *
     * ⓘ Cheap by design -- a walk of the session table, no enumeration. */
    {
        const int reaped = ctm_bridge_reap_gone_hosts();
        if (reaped > 0) {
            gesture_log("host gone: released %d controller(s) -- the USB server stopped answering", reaped);
        }
    }

    /* ⭐⭐ HOLD A BRIDGED CONTROLLER'S INPUT WHILE THE OVERLAY IS UP.
     *
     * ⛔ THE FAULT: with the overlay open, a bridged controller's presses still
     * reached the game behind it. An unbridged one's do not -- the app's event
     * filter stops forwarding them. A bridged controller produces no SDL events
     * at all; its reports go TV -> USB/IP -> PC and pass nothing.
     *
     * ⚠️ TWO EARLIER ATTEMPTS GOT THE "IS THE OVERLAY OPEN" PART WRONG, so this
     * one SAYS WHAT IT DECIDED rather than being trusted. ⛔ The first hung it
     * on app_ui_open/app_ui_close -- which sound right and are not: app_ui_open
     * runs ONCE, from main.c at boot, and is the launcher. The hold latched on
     * at startup and no controller reached any game.
     *
     * ⓘ Re-asserted every pass on purpose: whatever happened last pass, this
     * pass corrects it, so it cannot latch again. */
    {
        const bool held = overlay_open && session != NULL;
        static int s_last = -1;
        if ((int) held != s_last) {
            s_last = (int) held;
            gesture_log("input hold: %s (overlay=%d, session=%d)",
                        held ? "ON -- the game sees nothing pressed" : "off",
                        (int) overlay_open, session != NULL);
            ctm_bridge_set_input_held(held);
        }
    }

    /* ⭐ THE SETTING GATES THE WHOLE THING, at the one place it runs.
     *
     * ⓘ Returning here means no chord is ever detected: no pre-plug pulse, no
     * plug, no watching.
     *
     * ⓘ THE UNBRIDGE HALF IS GATED IN THE CORE, not here -- a bridged
     * controller's touchpad reports go through the bridge, so this side cannot
     * see them. Both halves read the same setting.
     *
     * ⭐ An earlier version deliberately left the unbridge chord alive, to
     * avoid a controller being stuck on the PC with gestures off. ⛔ rhoquinn8217,
     * 2026-08-19: that cannot happen. The setting is read when a stream starts
     * and cannot be changed during one, and leaving a stream by ANY route --
     * app switch, disconnect, crash -- unbridges everything. ➡️ So off means
     * off, in both directions, and the asymmetry was solving a problem that
     * does not exist. */
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
