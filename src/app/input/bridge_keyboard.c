/* A bridged keyboard is the bridge's. The why is in the header. */

#include "bridge_keyboard.h"

#include <stdio.h>

#if defined(TARGET_WEBOS)

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include <SDL.h>

#include "app.h"            /* global */
#include "logging.h"
#include "ctm_bridge_glue.h"
#include "ctm_bridge_gesture.h"
#include "stream/session.h"
#include "stream/session_priv.h"
#include "stream/input/session_input.h"
#include "stream/input/vk.h"
#include "util/bus.h"
#include "util/user_event.h"

/* How long after the last bridge or release the TV's grab looks again. Long
 * enough for every part of one device to be plugged or let go (they follow each
 * other about 130 ms apart), short enough that nobody waits for a keyboard. */
#define LOOK_AGAIN_MS 600u
/* How often a look-again waiting on a release asks again, and the longest it
 * waits for one before looking anyway. */
#define RELEASE_POLL_MS 200u
#define RELEASE_WAIT_MAX_MS 10000u

/* When the look-again is due, in SDL ticks. 0: nothing is due. */
static Uint32 s_look_again_at = 0;
static Uint32 s_look_waiting_since = 0;   /* 0 while not waiting on a release */

/* The overlay is open: the reader gives it the keys it understands and sends
 * the host nothing. ⓘ Written by the app's loop, read by the reader thread; a
 * key that crosses the change either way is harmless. */
static volatile bool s_overlay_open = false;

/* A shortcut the reader thread found, waiting for the app's loop to act on it:
 * ending a stream and opening the overlay both belong to that loop. ⓘ One
 * slot is enough, since the loop comes round every millisecond. */
enum {
    SHORTCUT_NONE = 0,
    SHORTCUT_OVERLAY,   /* Ctrl+Alt+Shift+S, Moonlight's stats shortcut: here it opens the overlay */
    SHORTCUT_QUIT,      /* Ctrl+Alt+Shift+Q, Moonlight's: ends the stream */
};
static SDL_atomic_t s_shortcut;

/* The keys the interface understands from a keyboard (read_keyboard() in
 * lvgl/input/lv_drv_sdl_key.c), as the grab's reader names them and as SDL
 * does. These are the ones the reader feeds the overlay itself. */
typedef struct {
    short vk;
    SDL_Keycode sym;
    SDL_Scancode scancode;
    unsigned short code;    /* the kernel's number for the key: linux/input-event-codes.h */
} nav_key_t;

static const nav_key_t k_nav[] = {
        {VK_UP,     SDLK_UP,        SDL_SCANCODE_UP,        103},
        {VK_DOWN,   SDLK_DOWN,      SDL_SCANCODE_DOWN,      108},
        {VK_LEFT,   SDLK_LEFT,      SDL_SCANCODE_LEFT,      105},
        {VK_RIGHT,  SDLK_RIGHT,     SDL_SCANCODE_RIGHT,     106},
        {VK_RETURN, SDLK_RETURN,    SDL_SCANCODE_RETURN,    28},
        {VK_ESCAPE, SDLK_ESCAPE,    SDL_SCANCODE_ESCAPE,    1},
        {VK_TAB,    SDLK_TAB,       SDL_SCANCODE_TAB,       15},
        {VK_BACK,   SDLK_BACKSPACE, SDL_SCANCODE_BACKSPACE, 14},
        {VK_DELETE, SDLK_DELETE,    SDL_SCANCODE_DELETE,    111},
        {VK_HOME,   SDLK_HOME,      SDL_SCANCODE_HOME,      102},
        {VK_END,    SDLK_END,       SDL_SCANCODE_END,       107},
};
#define NAV_KEYS ((int) (sizeof(k_nav) / sizeof(k_nav[0])))
#define NAV_CODE_KP_ENTER 96    /* the keypad's Enter is the same key to the interface */

/* What marks a key event as one the reader fed, in SDL's window field: no
 * window has this number, and nothing in the app reads the field. ⓘ The
 * second marks a key read from a BRIDGED keyboard's own nodes, the one source
 * webOS can also deliver; see bridge_keyboard_sdl_key. */
#define FED_WINDOW_ID 0x6b626466u
#define FED_BRIDGED_WINDOW_ID 0x6b626467u

/* How long a key taken from one source waits for the same key from the other
 * before it is forgotten. ⓘ Long, and counted rather than timed: the app's loop
 * can stall for a second on the very key press that asked for a bridge, and a
 * copy that turned up after a short window was taken as a second press
 * (build 454 on the C3: one Enter bridged a pad and released it again). */
#define TWIN_MS 2000u
#define TWINS_MAX 4

/* The keys whose press was fed, so their release is too, and whether the
 * overlay was open at the last key the reader saw. ⓘ The reader thread's, and
 * the app's loop's only while there is no reader (let_go, and between streams). */
static bool s_fed_down[NAV_KEYS];
static bool s_reader_in_overlay = false;

/* A bridged keyboard's own input nodes, opened by the app's loop while the
 * overlay is up and read there without being taken, and the keys of theirs
 * that are down. App's loop only. */
#define BRIDGED_NODES_MAX 8
#define BRIDGED_NODE_NUMBERS 64
static int s_bridged_fds[BRIDGED_NODES_MAX];
static int s_bridged_n = 0;
static bool s_bridged_down[NAV_KEYS];

/* The kernel's input event as it is laid out for a program, and its type for a
 * key. ⓘ Written out rather than taken from linux/input.h: that header's KEY_
 * and BTN_ names have no business meeting the rest of what this file includes. */
struct evdev_event {
    unsigned long sec;
    unsigned long usec;
    unsigned short type;
    unsigned short code;
    int value;
};
#define EVDEV_TYPE_KEY 1
#define EVDEV_VALUE_REPEAT 2
/* Two questions for a node, asked of the kernel: its name, and which keys it
 * can send. ⓘ Written out for the same reason: they are linux/input.h's
 * EVIOCGNAME and EVIOCGBIT(EV_KEY). */
#define EVDEV_GET_NAME(len) _IOC(_IOC_READ, 'E', 0x06, len)
#define EVDEV_GET_KEYS(len) _IOC(_IOC_READ, 'E', 0x20 + EVDEV_TYPE_KEY, len)
#define NAV_CODE_MAX 111    /* the highest of the numbers in k_nav */
/* Left Ctrl (29) and A (30) in the lowest word of a device's key list: what the
 * grab's own test asks of a keyboard first (is_usb_keyboard in
 * platform/webos/keyboard_evdev.c). */
#define KEYBOARD_LOW_KEYS 0x60000000ull

/* Keys taken from one source and not yet matched by their twin from the other:
 * how many, and when the last was taken. By source (fed, webOS), by press or
 * release, by key. App's loop only. */
static int s_owed[2][2][NAV_KEYS];
static Uint32 s_owed_at[2][2][NAV_KEYS];

/* Counts for the control port and the log. The first two are the reader
 * thread's; the rest belong to the app's loop. */
static SDL_atomic_t s_evdev_keys;
static SDL_atomic_t s_fed;
static int s_sdl_keys = 0;
static int s_sdl_nav = 0;
static int s_echoes = 0;
static int s_open_fed = 0, s_open_nav = 0, s_open_echoes = 0;

/* How often the input devices are looked at for one arriving or going. */
#define NODES_POLL_MS 500u
static Uint32 s_nodes_poll_at = 0;
static Uint32 s_nodes_hash = 0;
static Uint32 s_keyboards_hash = 0;
static bool s_nodes_known = false;

/* The record a person can read on any set: logs/tv-keyboard.log, beside the
 * bridge core's own logs and stamped with the same clock. ⓘ commons_log goes
 * to PmLog, which a C1 or a C3 does not let the developer account read.
 * ⛔ From the app's loop only: the core builds the path in one shared buffer. */
#define KEYBOARD_LOG "tv-keyboard.log"
#define KEYBOARD_LOG_MAX_BYTES (128L * 1024L)
FILE *ctm_log_open(const char *name, const char *mode);

static void keyboard_log(const char *fmt, ...) {
    FILE *f = ctm_log_open(KEYBOARD_LOG, "a");
    if (f == NULL) {
        return;
    }
    /* Started again rather than left to grow: it is a few lines a stream. */
    if (fseek(f, 0, SEEK_END) == 0 && ftell(f) > KEYBOARD_LOG_MAX_BYTES) {
        fclose(f);
        f = ctm_log_open(KEYBOARD_LOG, "w");
        if (f == NULL) {
            return;
        }
    }
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    fprintf(f, "%lld.%03ld ", (long long) ts.tv_sec, ts.tv_nsec / 1000000L);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

static stream_input_t *live_input(void) {
    if (global == NULL || global->session == NULL) {
        return NULL;
    }
    return session_get_input(global->session);
}

static int nav_slot_for_vk(short vk) {
    for (int i = 0; i < NAV_KEYS; i++) {
        if (k_nav[i].vk == vk) {
            return i;
        }
    }
    return -1;
}

static int nav_slot_for_sym(SDL_Keycode sym) {
    if (sym == SDLK_KP_ENTER || sym == SDLK_RETURN2) {
        sym = SDLK_RETURN;
    }
    for (int i = 0; i < NAV_KEYS; i++) {
        if (k_nav[i].sym == sym) {
            return i;
        }
    }
    return -1;
}

bool bridge_keyboard_node_is_bridged(const char *event_path) {
    /* ⛔ Not before the bridge is running: nothing can be bridged outside a
     * stream, and asking the glue would bring the core up early. */
    if (event_path == NULL || !ctm_bridge_active()) {
        return false;
    }
    if (!ctm_bridge_gesture_event_is_bridged(event_path)) {
        return false;
    }
    keyboard_log("the TV's grab leaves %s to the bridge", event_path);
    return true;
}

void bridge_keyboard_took(const char *event_path, const char *name) {
    keyboard_log("the TV's grab holds %s (%s)", event_path != NULL ? event_path : "?",
                 name != NULL && name[0] != '\0' ? name : "no name");
}

void bridge_keyboard_changed(void) {
    s_look_again_at = SDL_GetTicks() + LOOK_AGAIN_MS;
    if (s_look_again_at == 0) {
        s_look_again_at = 1;
    }
}

static void feed_overlay(int slot, bool down, char modifiers);
static void feed_overlay_from(int slot, bool down, char modifiers, bool bridged);

/* The grab lets go of every keyboard, and the host lets go of every key.
 *
 * ⛔ THE TWO GO TOGETHER. Until the grab has looked again nobody reads the
 * keyboards it held, so a key released in that time would never come up on the
 * host, and the host repeats a held key by itself. Whatever the host holds
 * comes up here instead; a key still down afterwards is simply pressed again.
 * ⓘ Joins the TV's reader thread, which wakes at least every 200 ms, so the
 * list of keys the host holds is this thread's to touch. With nothing grabbed
 * it returns at once, so several parts of one device cost this once. */
static void let_go(stream_input_t *input) {
    if (input->keyboard_evdev == NULL) {
        return;
    }
    session_input_set_keyboard_grab(input, false);
    stream_input_flush_pressed_keys(input);
    /* The interface as well: a key the reader had given it would never come
     * up there either, and it repeats a held arrow by itself. */
    for (int slot = 0; slot < NAV_KEYS; slot++) {
        if (s_fed_down[slot]) {
            s_fed_down[slot] = false;
            feed_overlay(slot, false, 0);
        }
    }
}

void bridge_keyboard_before_plug(void) {
    stream_input_t *input = live_input();
    if (input == NULL) {
        return;
    }
    let_go(input);
    bridge_keyboard_changed();
}

/* Reader thread: one key for the overlay, as an SDL key event of the kind
 * webOS would have sent. ⓘ `bridged` when it was read from a bridged
 * keyboard's own nodes. */
static void feed_overlay_from(int slot, bool down, char modifiers, bool bridged) {
    SDL_Event event;
    SDL_zero(event);
    event.type = down ? SDL_KEYDOWN : SDL_KEYUP;
    event.key.windowID = bridged ? FED_BRIDGED_WINDOW_ID : FED_WINDOW_ID;
    event.key.state = down ? SDL_PRESSED : SDL_RELEASED;
    event.key.keysym.scancode = k_nav[slot].scancode;
    event.key.keysym.sym = k_nav[slot].sym;
    event.key.keysym.mod = (modifiers & MODIFIER_SHIFT) ? KMOD_LSHIFT : KMOD_NONE;
    SDL_PushEvent(&event);
    SDL_AtomicAdd(&s_fed, 1);
}

static void feed_overlay(int slot, bool down, char modifiers) {
    feed_overlay_from(slot, down, modifiers, false);
}

static int nav_slot_for_code(unsigned short code) {
    if (code == NAV_CODE_KP_ENTER) {
        code = 28;
    }
    for (int i = 0; i < NAV_KEYS; i++) {
        if (k_nav[i].code == code) {
            return i;
        }
    }
    return -1;
}

/* ⭐ A BRIDGED KEYBOARD WORKS THE OVERLAY TOO (rhoquinn8217, 2026-10-01: "If a
 * bridged keyboard enters the overlay, results in no control and I have to
 * rely on a different input"). The bridge core holds such a keyboard for the
 * stream and lets it go while the overlay is open, sending the host nothing;
 * it left webOS to hand the keys to the app, which a C3 did not do. So while
 * the overlay is open the app's loop opens the keyboard's own input nodes and
 * reads them, WITHOUT taking them. ⓘ Nothing can reach the host this way:
 * the nodes are closed again as the overlay closes, and while the core holds
 * them a reader that has not taken them hears nothing anyway. */
static void close_bridged_nodes(void) {
    /* The interface must not be left holding a key the core is about to take
     * back unseen. */
    for (int slot = 0; slot < NAV_KEYS; slot++) {
        if (s_bridged_down[slot]) {
            s_bridged_down[slot] = false;
            feed_overlay(slot, false, 0);
        }
    }
    for (int i = 0; i < s_bridged_n; i++) {
        close(s_bridged_fds[i]);
    }
    s_bridged_n = 0;
}

/* Can this node send any key the interface reads? ⓘ A bridged controller has
 * input nodes too (a DualSense has three) and none of them can. Read here
 * they would only take the places a keyboard needs: build 456 on the C3 opened
 * a DualSense's three beside the keyboard's four, and there are eight. */
static bool node_has_nav_key(int fd) {
    const unsigned word_bits = 8u * (unsigned) sizeof(unsigned long);
    unsigned long keys[NAV_CODE_MAX / (8 * sizeof(unsigned long)) + 1];
    memset(keys, 0, sizeof(keys));
    if (ioctl(fd, EVDEV_GET_KEYS(sizeof(keys)), keys) < 0) {
        return true;   /* it cannot be asked: read it, as before */
    }
    for (int i = 0; i <= NAV_KEYS; i++) {
        const unsigned code = i < NAV_KEYS ? k_nav[i].code : NAV_CODE_KP_ENTER;
        if ((keys[code / word_bits] & (1ul << (code % word_bits))) != 0) {
            return true;
        }
    }
    return false;
}

static void open_bridged_nodes(void) {
    close_bridged_nodes();
    if (!ctm_bridge_active()) {
        return;
    }
    for (int number = 0; number < BRIDGED_NODE_NUMBERS && s_bridged_n < BRIDGED_NODES_MAX; number++) {
        char path[32];
        snprintf(path, sizeof(path), "/dev/input/event%d", number);
        if (!ctm_bridge_gesture_event_is_bridged(path)) {
            continue;
        }
        const int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) {
            continue;
        }
        if (!node_has_nav_key(fd)) {
            close(fd);
            continue;
        }
        char name[96] = "";
        (void) ioctl(fd, EVDEV_GET_NAME(sizeof(name) - 1), name);
        s_bridged_fds[s_bridged_n++] = fd;
        keyboard_log("the overlay reads %s (%s) itself: it is bridged", path, name[0] != '\0' ? name : "no name");
    }
}

static void read_bridged_nodes(void) {
    for (int i = 0; i < s_bridged_n; i++) {
        /* ⓘ A bounded burst, so a node with a great deal to say cannot hold
         * the app's loop. */
        for (int burst = 0; burst < 64; burst++) {
            struct evdev_event ev;
            const ssize_t got = read(s_bridged_fds[i], &ev, sizeof(ev));
            if (got != (ssize_t) sizeof(ev)) {
                if (got < 0 && errno == ENODEV) {
                    /* The device went: its place is taken by the last one. */
                    close(s_bridged_fds[i]);
                    s_bridged_fds[i] = s_bridged_fds[--s_bridged_n];
                    i--;
                }
                break;
            }
            if (ev.type != EVDEV_TYPE_KEY || ev.value == EVDEV_VALUE_REPEAT) {
                continue;
            }
            const int slot = nav_slot_for_code(ev.code);
            const bool down = ev.value != 0;
            if (slot < 0 || down == s_bridged_down[slot]) {
                continue;
            }
            s_bridged_down[slot] = down;
            feed_overlay_from(slot, down, 0, true);
        }
    }
}

bool bridge_keyboard_evdev_key(stream_input_t *input, short vk, bool down, char modifiers) {
    SDL_AtomicAdd(&s_evdev_keys, 1);
    const bool overlay = s_overlay_open;
    if (overlay && !s_reader_in_overlay) {
        /* ⛔ The first key since the overlay opened: whatever the host still
         * holds comes up now. From here the overlay swallows every key, the
         * releases too, so a key that was down as the overlay opened (from a
         * controller or the remote, with a key held) would stay down on the
         * host, which repeats a held key by itself. ⓘ A shortcut does this
         * itself, below; here on this thread for the same reason. */
        stream_input_flush_pressed_keys(input);
    }
    s_reader_in_overlay = overlay;
    const int slot = nav_slot_for_vk(vk);
    if (slot >= 0) {
        if (down && overlay) {
            s_fed_down[slot] = true;
            feed_overlay(slot, true, modifiers);
            return true;
        }
        if (!down && s_fed_down[slot]) {
            /* Its release follows it even when the overlay closed in between
             * (Escape does exactly that), so the interface never keeps a key
             * down. */
            s_fed_down[slot] = false;
            feed_overlay(slot, false, modifiers);
            return true;
        }
    }
    if (overlay) {
        return true;   /* the overlay has the keyboard */
    }
    const char chord = MODIFIER_CTRL | MODIFIER_ALT | MODIFIER_SHIFT;
    if (!down || (modifiers & chord) != chord) {
        return false;
    }
    int shortcut;
    switch (vk) {
        case VK_S:
            shortcut = SHORTCUT_OVERLAY;
            break;
        case VK_Q:
            shortcut = SHORTCUT_QUIT;
            break;
        default:
            /* ⓘ Moonlight's Z, X, M, C and D only write a log line in this app
             * (performPendingSpecialKeyCombo), so they stay the host's. O is
             * a letter like any other again. */
            return false;
    }
    /* The host saw Ctrl, Alt and Shift go down and will not see them come up
     * in time: the overlay takes the keyboard, or the stream ends. ⓘ Here and
     * not on the app's loop, because this thread is the one that keeps the
     * list of keys the host holds. */
    stream_input_flush_pressed_keys(input);
    SDL_AtomicSet(&s_shortcut, shortcut);
    return true;
}

int bridge_keyboard_sdl_key(const struct SDL_KeyboardEvent *event) {
    const bool fromBridged = event->windowID == FED_BRIDGED_WINDOW_ID;
    const bool ours = event->windowID == FED_WINDOW_ID || fromBridged;
    if (!ours) {
        s_sdl_keys++;
    }
    const int slot = nav_slot_for_sym(event->keysym.sym);
    if (slot < 0) {
        return BRIDGE_KEYBOARD_KEY_PASS;
    }
    const int state = event->type == SDL_KEYUP ? 1 : 0;
    const int source = ours ? 0 : 1;
    const int other = 1 - source;
    const Uint32 now = SDL_GetTicks();
    /* Is this the twin of a key already taken from the other source? ⓘ A key
     * the grab fed never is: webOS has no copy of a held keyboard's keys. */
    if ((!ours || fromBridged) && s_owed[other][state][slot] > 0) {
        if (now - s_owed_at[other][state][slot] < TWIN_MS) {
            s_owed[other][state][slot]--;
            s_echoes++;
            return BRIDGE_KEYBOARD_KEY_DROP;
        }
        s_owed[other][state][slot] = 0;   /* too old to be this key again */
    }
    if (ours && !s_overlay_open) {
        /* Fed for an overlay that has closed since. A release still has to
         * reach the interface, or the key stays down in it. */
        return state == 1 ? BRIDGE_KEYBOARD_KEY_RELEASE : BRIDGE_KEYBOARD_KEY_DROP;
    }
    if (!s_overlay_open) {
        /* An ordinary key for the stream: nothing is fed outside the overlay,
         * so nothing is owed for it. */
        return BRIDGE_KEYBOARD_KEY_PASS;
    }
    /* Taken, and its twin from the other source is owed, if that source
     * delivers this keyboard at all.
     * ⭐ AND ONLY WHERE A TWIN CAN EXIST (code review, 2026-10-05). Every key
     * owed one, so the Magic Remote's Down within two seconds of a keyboard's
     * Down was dropped as its copy, and the other way round: up to four
     * presses lost. The one keyboard both sources deliver is a BRIDGED one,
     * read from its own nodes while webOS hands its keys over as well (the C1
     * does; build 458). A keyboard the grab holds stays held while the overlay
     * is open, so webOS has no copy of its keys to send. ➡️ So a fed key owes
     * a twin only when it came from a bridged keyboard's nodes, and a webOS
     * key only while such nodes are being read. */
    const bool twinPossible = ours ? fromBridged : s_bridged_n > 0;
    if (!twinPossible) {
        if (!ours) {
            s_sdl_nav++;
        }
        return BRIDGE_KEYBOARD_KEY_PASS;
    }
    if (s_owed[source][state][slot] > 0 && now - s_owed_at[source][state][slot] >= TWIN_MS) {
        s_owed[source][state][slot] = 0;
    }
    if (s_owed[source][state][slot] < TWINS_MAX) {
        s_owed[source][state][slot]++;
    }
    s_owed_at[source][state][slot] = now;
    if (!ours) {
        s_sdl_nav++;
    }
    return BRIDGE_KEYBOARD_KEY_PASS;
}

void bridge_keyboard_counts(char *buf, size_t len) {
    if (buf == NULL || len == 0) {
        return;
    }
    snprintf(buf, len, "evdev_keys=%d fed=%d sdl_keys=%d sdl_nav_in_overlay=%d echoes=%d",
             SDL_AtomicGet(&s_evdev_keys), SDL_AtomicGet(&s_fed), s_sdl_keys, s_sdl_nav, s_echoes);
}

/* What a shortcut does, the same as on the keyboard path SDL feeds
 * (performPendingSpecialKeyCombo in session_keyboard.c). */
static void act_on_shortcut(int shortcut) {
    switch (shortcut) {
        case SHORTCUT_OVERLAY:
            commons_log_info("Input", "Keyboard evdev: Ctrl+Alt+Shift+S, opening the overlay");
            keyboard_log("shortcut Ctrl+Alt+Shift+S on a keyboard the TV has: opening the overlay");
            bus_pushevent(USER_OPEN_OVERLAY, NULL, NULL);
            break;
        case SHORTCUT_QUIT:
            commons_log_info("Input", "Keyboard evdev: Ctrl+Alt+Shift+Q, ending the stream");
            keyboard_log("shortcut Ctrl+Alt+Shift+Q on a keyboard the TV has: ending the stream");
            /* The overlay's own Disconnect: the host keeps the game running. */
            session_interrupt(global->session, false, STREAMING_INTERRUPT_USER);
            break;
        default:
            break;
    }
}

/* Every input device the kernel has, as one number that changes when one
 * arrives, goes, or comes back. ⓘ From the kernel's own list and not from
 * /dev/input: a C3 keeps all 32 event nodes there from boot, so neither that
 * folder nor its entries change when a keyboard connects (measured
 * 2026-10-01). In /sys/class/input a device that comes back gets a new
 * inputNN name even when its event number is the same. */
static Uint32 input_devices_hash(const char **source) {
    DIR *dir = opendir("/sys/class/input");
    if (dir != NULL) {
        Uint32 sum = 0;
        struct dirent *ent;
        while ((ent = readdir(dir)) != NULL) {
            if (ent->d_name[0] == '.') {
                continue;
            }
            Uint32 h = 2166136261u;
            for (const char *p = ent->d_name; *p != '\0'; ++p) {
                h = (h ^ (unsigned char) *p) * 16777619u;
            }
            sum += h;   /* a sum, so the order they are listed in does not matter */
        }
        closedir(dir);
        *source = "/sys/class/input";
        return sum | 1u;
    }
    FILE *f = fopen("/proc/bus/input/devices", "r");
    if (f != NULL) {
        Uint32 h = 2166136261u;
        int c;
        while ((c = fgetc(f)) != EOF) {
            h = (h ^ (unsigned char) c) * 16777619u;
        }
        fclose(f);
        *source = "/proc/bus/input/devices";
        return h | 1u;
    }
    *source = NULL;
    return 0;
}

/* The keyboards among them, as one number: every input device with letter keys
 * and a left Ctrl. ⓘ Wider than the grab's own test, which also goes by name;
 * looking again once too often costs little, and missing a keyboard costs the
 * keyboard. 0 when they cannot be listed. */
static Uint32 keyboards_hash(void) {
    DIR *dir = opendir("/sys/class/input");
    if (dir == NULL) {
        return 0;
    }
    static char keys[4096];   /* app's loop only; the kernel gives a page at most */
    Uint32 sum = 0;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (strncmp(ent->d_name, "input", 5) != 0) {
            continue;
        }
        char path[320];
        snprintf(path, sizeof(path), "/sys/class/input/%s/capabilities/key", ent->d_name);
        FILE *f = fopen(path, "r");
        if (f == NULL) {
            continue;
        }
        size_t len = fread(keys, 1, sizeof(keys) - 1, f);
        fclose(f);
        while (len > 0 && (keys[len - 1] == '\n' || keys[len - 1] == ' ')) {
            len--;
        }
        keys[len] = '\0';
        /* Words in hexadecimal, the lowest keys in the LAST one. */
        const char *last = strrchr(keys, ' ');
        last = last != NULL ? last + 1 : keys;
        if ((strtoull(last, NULL, 16) & KEYBOARD_LOW_KEYS) != KEYBOARD_LOW_KEYS) {
            continue;
        }
        Uint32 h = 2166136261u;
        for (const char *p = ent->d_name; *p != '\0'; ++p) {
            h = (h ^ (unsigned char) *p) * 16777619u;
        }
        sum += h;
    }
    closedir(dir);
    return sum | 1u;
}

/* ⭐ A KEYBOARD THAT CONNECTS MID-STREAM IS TAKEN TOO (rhoquinn8217, 2026-10-01:
 * "connecting a keyboard while the stream has already started doesn't
 * register"). Upstream's grab looks for keyboards once, as the stream starts,
 * so one that arrives later, or a wireless one that slept and came back as a
 * new device, was nobody's. A keyboard that goes leaves the reader a dead
 * handle, and its loop then spins on it; looking again drops that as well.
 *
 * ⛔ ONLY FOR A KEYBOARD. Looking again leaves every keyboard unread for about
 * half a second, and a key pressed in that time is lost. A controller that
 * switches itself off while someone is typing must not cost them that. */
static void watch_input_devices(void) {
    const Uint32 now = SDL_GetTicks();
    if (s_nodes_known && !SDL_TICKS_PASSED(now, s_nodes_poll_at)) {
        return;
    }
    s_nodes_poll_at = now + NODES_POLL_MS;
    const char *source = NULL;
    const Uint32 hash = input_devices_hash(&source);
    if (hash == 0) {
        if (!s_nodes_known) {
            keyboard_log("the input devices cannot be listed here: a keyboard that connects"
                         " mid-stream waits for the next bridge, release or stream start");
            s_nodes_known = true;
            s_nodes_hash = 0;
        }
        return;
    }
    if (!s_nodes_known) {
        keyboard_log("watching %s for a keyboard arriving or going", source);
        s_keyboards_hash = keyboards_hash();
    } else if (hash != s_nodes_hash) {
        const Uint32 keyboards = keyboards_hash();
        if (keyboards == 0 || keyboards != s_keyboards_hash) {
            keyboard_log("a keyboard connected or went away: the grab looks again");
            bridge_keyboard_changed();
        } else {
            keyboard_log("an input device that is not a keyboard connected or went away:"
                         " the grab stays as it is");
        }
        s_keyboards_hash = keyboards;
    }
    s_nodes_hash = hash;
    s_nodes_known = true;
}

void bridge_keyboard_tick(bool overlay_shown) {
    stream_input_t *input = live_input();
    if (overlay_shown != s_overlay_open) {
        /* ⓘ Nothing is let go: the keyboards the grab holds stay held and its
         * reader feeds the overlay from them. A bridged keyboard is read from
         * here for as long as the overlay is up. */
        s_overlay_open = overlay_shown;
        if (overlay_shown) {
            open_bridged_nodes();
        } else {
            close_bridged_nodes();
        }
        const int fed = SDL_AtomicGet(&s_fed);
        if (overlay_shown) {
            s_open_fed = fed;
            s_open_nav = s_sdl_nav;
            s_open_echoes = s_echoes;
        } else if (fed != s_open_fed || s_sdl_nav != s_open_nav || s_echoes != s_open_echoes) {
            /* The record of who gave the overlay its keys: webOS hands some
             * keyboards' keys to the app and not others. */
            keyboard_log("overlay closed: the reader fed it %d key event(s); %d came from webOS"
                         " (a keyboard or the remote), and %d more were the same key twice",
                         fed - s_open_fed, s_sdl_nav - s_open_nav, s_echoes - s_open_echoes);
        }
    }
    const int shortcut = SDL_AtomicGet(&s_shortcut);
    if (shortcut != SHORTCUT_NONE) {
        SDL_AtomicSet(&s_shortcut, SHORTCUT_NONE);
        /* ⓘ Not once the stream it was pressed in has gone. */
        if (input != NULL) {
            act_on_shortcut(shortcut);
        }
    }
    if (s_overlay_open) {
        read_bridged_nodes();
    }
    if (input == NULL) {
        /* No stream: nothing is held, so nothing is watched or owed, and no
         * reader is left to remember a key from the stream before. */
        s_nodes_known = false;
        s_look_again_at = 0;
        s_look_waiting_since = 0;
        memset(s_fed_down, 0, sizeof(s_fed_down));
        s_reader_in_overlay = false;
        return;
    }
    watch_input_devices();
    if (s_look_again_at == 0 || !SDL_TICKS_PASSED(SDL_GetTicks(), s_look_again_at)) {
        return;
    }
    /* ⭐ NOT WHILE A RELEASE IS STILL RUNNING (code review, 2026-10-05). A
     * release finishes on a thread of its own now, and can take longer than
     * the wait above, a slow link's BRIDGE_STOP alone up to a second. Looked
     * at then, a keyboard still being released is skipped as bridged, and when
     * its release ends nothing looks again: held by nobody for the rest of
     * the stream. So the look waits for every release in flight, ten seconds
     * at most. */
    if (ctm_bridge_releases_in_flight() > 0) {
        const Uint32 now = SDL_GetTicks();
        if (s_look_waiting_since == 0) {
            s_look_waiting_since = now ? now : 1;
        }
        if (!SDL_TICKS_PASSED(now, s_look_waiting_since + RELEASE_WAIT_MAX_MS)) {
            s_look_again_at = now + RELEASE_POLL_MS;
            if (s_look_again_at == 0) {
                s_look_again_at = 1;
            }
            return;
        }
    }
    s_look_waiting_since = 0;
    s_look_again_at = 0;
    /* Let go of everything, then look again: the scan skips what is bridged
     * (bridge_keyboard_node_is_bridged) and takes what has been released or
     * has just connected. ⓘ Whatever Bridge Override says: it leaves the
     * keyboard alone. */
    let_go(input);
    session_input_set_keyboard_grab(input, true);
    const bool holding = input->keyboard_evdev != NULL;
    commons_log_info("Input", "Keyboard grab looked again: %s",
                     holding ? "holding the keyboards that are not bridged" : "no keyboard to hold");
    keyboard_log("grab looked again: %s",
                 holding ? "holding the keyboards that are not bridged" : "no keyboard to hold");
    /* What is bridged may just have changed, from the overlay's own panel. */
    if (s_overlay_open) {
        open_bridged_nodes();
    }
}

#else /* !TARGET_WEBOS */

bool bridge_keyboard_node_is_bridged(const char *event_path) {
    (void) event_path;
    return false;
}

void bridge_keyboard_took(const char *event_path, const char *name) {
    (void) event_path;
    (void) name;
}

void bridge_keyboard_before_plug(void) {
}

void bridge_keyboard_changed(void) {
}

void bridge_keyboard_tick(bool overlay_shown) {
    (void) overlay_shown;
}

bool bridge_keyboard_evdev_key(stream_input_t *input, short vk, bool down, char modifiers) {
    (void) input;
    (void) vk;
    (void) down;
    (void) modifiers;
    return false;
}

int bridge_keyboard_sdl_key(const struct SDL_KeyboardEvent *event) {
    (void) event;
    return BRIDGE_KEYBOARD_KEY_PASS;
}

void bridge_keyboard_counts(char *buf, size_t len) {
    if (buf != NULL && len != 0) {
        snprintf(buf, len, "n/a");
    }
}

#endif /* TARGET_WEBOS */
