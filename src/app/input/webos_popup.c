/* A webOS pop-up answered from a controller. The why is in the header. */

#include "webos_popup.h"

#include <stdio.h>

#if defined(TARGET_WEBOS)

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include "stream/input/vk.h"
#include "ctm_bridge_glue.h"   /* a bridged pad, read by the core while input is held */
#include "ctm_bridge_gesture.h"   /* which pad of SDL's is a bridged one */
#include "lunasynccall.h"     /* a test pop-up, raised by the app itself */

#define WEBOS_POPUP_LOG "webos-popup.log"
FILE *ctm_log_open(const char *name, const char *mode);

/* The kernel's input event as it is laid out for a program. ⓘ Written out
 * rather than taken from linux/input.h, as mouse_seen.c does, for the same
 * reason. */
struct evdev_event {
    unsigned long sec;
    unsigned long usec;
    unsigned short type;
    unsigned short code;
    int value;
};
#define EVDEV_TYPE_SYN 0
#define EVDEV_SYN_REPORT 0
#define EVDEV_TYPE_KEY 1
/* linux/input.h's EVIOCGNAME and EVIOCGBIT(EV_KEY). */
#define EVDEV_GET_NAME(len) _IOC(_IOC_READ, 'E', 0x06, len)
#define EVDEV_GET_KEYS(len) _IOC(_IOC_READ, 'E', 0x20 + EVDEV_TYPE_KEY, len)

/* The remote's keys (the header says where they come from). */
#define REMOTE_OK 28
#define REMOTE_UP 103
#define REMOTE_LEFT 105
#define REMOTE_RIGHT 106
#define REMOTE_DOWN 108
#define REMOTE_BACK 412
/* The highest key the C1's kernel knows: LG's own go past linux's 767, to 0x4ff. */
#define REMOTE_KEY_MAX 1279

#define REMOTE_NODE_NUMBERS 64   /* /dev/input/event0 to event63 */
#define AXIS_REST 8000           /* a stick or trigger nearer its rest than this has been let go */
#define PENDING_MAX 8            /* presses kept between the filter and the tick */
#define STICK_PUSH 16000         /* the left stick is an arrow past half way, as in lv_drv_sdl_key.c */
#define STICK_UNSEEN 2           /* a stick not yet seen short of that in this pop-up */
#define PADS_MAX 8

typedef enum {
    WINDOW_FOCUSED,   /* the app has the focus: everything as ever */
    WINDOW_POPUP,     /* shown, and webOS has the focus */
    WINDOW_AWAY,      /* hidden or minimized: another app in front */
} window_state_t;

static SDL_Window *s_window = NULL;
static bool s_had_focus = false;          /* until the app has had the focus once, nothing is a pop-up */
static window_state_t s_state = WINDOW_FOCUSED;
static Uint32 s_since = 0;                /* when that state began */
static struct {
    int key;
    const char *from;
} s_pending[PENDING_MAX];
static int s_pending_n = 0;
static struct {
    SDL_JoystickID which;
    int held[2];                          /* the left stick across and down: -1, 0, 1 or STICK_UNSEEN */
} s_sticks[PADS_MAX];
static int s_sticks_n = 0;
/* A bridged pad, read by the bridge core while input is held: what it held at
 * the last reading, by its node. ⓘ Forgotten as each pop-up comes, so the first
 * reading in one only says what is held already. */
static struct {
    char node[64];
    bool keys[6];                         /* up, down, left, right, Cross (A), Circle (B) */
    int held[2];                          /* its left stick, as s_sticks */
} s_bridged[PADS_MAX];
static int s_bridged_n = 0;
/* ⛔ A BRIDGED PAD ANSWERS FROM THE CORE ONLY. SDL can still read one (the Edge
 * on its DS5Dongle, bridged on the C1: overlay_volume.c), and each press would
 * be sent twice. So a pad of SDL's at a node the core reads is left out of the
 * filter. ⓘ Matched as that list changes and every quarter second, not every
 * pass: the match can look in /sys. */
static SDL_JoystickID s_core_ids[PADS_MAX];
static int s_core_ids_n = 0;
static unsigned s_core_ids_sig = 0;
static Uint32 s_core_ids_at = 0;
/* ⓘ A keyboard's reader is a thread of its own: it reads whether a pop-up is
 * up from here, and leaves its keys here for the tick. */
static SDL_atomic_t s_up_flag;
static SDL_SpinLock s_keys_lock = 0;
static struct {
    int key;
    const char *from;
} s_keys[PENDING_MAX];
static int s_keys_n = 0;
static int s_sent = 0;                    /* keys sent in this pop-up */
static int s_sent_all = 0;                /* and since the app started */
static int s_last_key = 0;
static char s_node[24] = "";              /* the remote's node, once found */
static char s_node_name[64] = "";
static char s_failed[96] = "";            /* why the last key was not sent */

static void popup_log(const char *fmt, ...) {
    FILE *f = ctm_log_open(WEBOS_POPUP_LOG, "a");
    if (f == NULL) {
        return;
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

/* ⓘ SDL's own record, as current as the last pump: the focus is what SDL
 * drops a controller's presses by. */
static window_state_t window_state(void) {
    if (s_window == NULL || SDL_GetKeyboardFocus() != NULL) {
        return WINDOW_FOCUSED;
    }
    if ((SDL_GetWindowFlags(s_window) & (SDL_WINDOW_HIDDEN | SDL_WINDOW_MINIMIZED)) != 0) {
        return WINDOW_AWAY;
    }
    return s_had_focus ? WINDOW_POPUP : WINDOW_FOCUSED;
}

static bool has_key(const unsigned char *keys, size_t len, int key) {
    const size_t byte = (size_t) key / 8;
    return byte < len && (keys[byte] & (1u << (key % 8))) != 0;
}

/* Which input nodes are the remote's, best first: the IR remote's ("LGE RCU"
 * on the C1, which lists every key), the Magic Remote's ("LGE M-RCU - Builtin
 * [0]"), then any other remote's. 0: not a remote's. */
static int remote_rank(const char *name) {
    if (strcmp(name, "LGE RCU") == 0) {
        return 3;
    }
    if (strstr(name, "M-RCU") != NULL) {
        return 2;
    }
    return strstr(name, "RCU") != NULL ? 1 : 0;
}

/* The remote's own input node, by name, and only one that lists the six keys.
 * ⓘ Logged, so another set's names can be read back. */
static bool find_remote(void) {
    static const int k_needed[] = {REMOTE_OK, REMOTE_UP, REMOTE_DOWN, REMOTE_LEFT, REMOTE_RIGHT, REMOTE_BACK};
    int best = 0;
    for (int number = 0; number < REMOTE_NODE_NUMBERS; number++) {
        char path[24];
        snprintf(path, sizeof(path), "/dev/input/event%d", number);
        const int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) {
            continue;
        }
        char name[64] = "";
        (void) ioctl(fd, EVDEV_GET_NAME(sizeof(name) - 1), name);
        const int rank = remote_rank(name);
        if (rank > best) {
            unsigned char keys[(REMOTE_KEY_MAX + 1) / 8] = {0};
            const int got = ioctl(fd, EVDEV_GET_KEYS(sizeof(keys)), keys);
            bool all = got > 0;
            for (size_t i = 0; all && i < sizeof(k_needed) / sizeof(k_needed[0]); i++) {
                all = has_key(keys, (size_t) got, k_needed[i]);
            }
            if (all) {
                best = rank;
                snprintf(s_node, sizeof(s_node), "%s", path);
                snprintf(s_node_name, sizeof(s_node_name), "%s", name);
            }
        }
        close(fd);
    }
    if (best == 0) {
        popup_log("no remote's input node lists the keys: a controller cannot answer a pop-up on this TV");
        return false;
    }
    popup_log("the remote's input node: %s (%s)", s_node, s_node_name);
    return true;
}

/* ⓘ The kernel hands what is written to an input node to everything reading
 * it, as if the device had sent it. Down and up in one write. */
static bool remote_press(int key) {
    if (s_node[0] == '\0' && !find_remote()) {
        snprintf(s_failed, sizeof(s_failed), "no remote's input node found");
        return false;
    }
    const struct evdev_event press[4] = {
            {0, 0, EVDEV_TYPE_KEY, (unsigned short) key, 1},
            {0, 0, EVDEV_TYPE_SYN, EVDEV_SYN_REPORT, 0},
            {0, 0, EVDEV_TYPE_KEY, (unsigned short) key, 0},
            {0, 0, EVDEV_TYPE_SYN, EVDEV_SYN_REPORT, 0},
    };
    const int fd = open(s_node, O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        snprintf(s_failed, sizeof(s_failed), "%s would not open (%s)", s_node, strerror(errno));
        s_node[0] = '\0';   /* looked for again at the next press */
        return false;
    }
    const ssize_t wrote = write(fd, press, sizeof(press));
    const int error = errno;
    close(fd);
    if (wrote != (ssize_t) sizeof(press)) {
        snprintf(s_failed, sizeof(s_failed), "%s refused it (%s)", s_node, wrote < 0 ? strerror(error) : "short");
        return false;
    }
    s_sent++;
    s_sent_all++;
    s_last_key = key;
    return true;
}

static const char *key_name(int key) {
    switch (key) {
        case REMOTE_OK:
            return "OK";
        case REMOTE_BACK:
            return "Back";
        case REMOTE_UP:
            return "up";
        case REMOTE_DOWN:
            return "down";
        case REMOTE_LEFT:
            return "left";
        case REMOTE_RIGHT:
            return "right";
        default:
            return "a key";
    }
}

/* A press to send on as the remote's, once SDL's walk is over. */
static void keep(int key, const char *from) {
    if (s_pending_n < PENDING_MAX) {
        s_pending[s_pending_n].key = key;
        s_pending[s_pending_n].from = from;
        s_pending_n++;
    }
}

/* One axis of a left stick as the d-pad, as Aurora's own screens take it
 * (lv_drv_sdl_key.c): an arrow each time it is pushed past half way. ⓘ Only
 * once it has been seen short of that in this pop-up, so a stick already held
 * as the pop-up comes moves nothing. */
static void stick_step(int *held, int value, bool across, bool bridged) {
    static const int k_keys[4] = {REMOTE_LEFT, REMOTE_RIGHT, REMOTE_UP, REMOTE_DOWN};
    static const char *const k_from[2][4] = {
            {"the left stick's left", "the left stick's right", "the left stick's up", "the left stick's down"},
            {"a bridged pad's left stick, left", "a bridged pad's left stick, right",
             "a bridged pad's left stick, up", "a bridged pad's left stick, down"},
    };
    const int dir = value <= -STICK_PUSH ? -1 : (value >= STICK_PUSH ? 1 : 0);
    if (*held == STICK_UNSEEN) {
        if (dir == 0) {
            *held = 0;
        }
        return;
    }
    if (dir == *held) {
        return;
    }
    *held = dir;
    if (dir != 0) {
        const int which = (across ? 0 : 2) + (dir > 0 ? 1 : 0);
        keep(k_keys[which], k_from[bridged ? 1 : 0][which]);
    }
}

/* A pad of SDL's: its left stick, from the filter. */
static void stick_as_arrow(const SDL_ControllerAxisEvent *axis) {
    bool across;
    if (axis->axis == SDL_CONTROLLER_AXIS_LEFTX) {
        across = true;
    } else if (axis->axis == SDL_CONTROLLER_AXIS_LEFTY) {
        across = false;
    } else {
        return;
    }
    int i = 0;
    while (i < s_sticks_n && s_sticks[i].which != axis->which) {
        i++;
    }
    if (i == s_sticks_n) {
        if (s_sticks_n >= PADS_MAX) {
            return;
        }
        s_sticks[i].which = axis->which;
        s_sticks[i].held[0] = STICK_UNSEEN;
        s_sticks[i].held[1] = STICK_UNSEEN;
        s_sticks_n++;
    }
    stick_step(&s_sticks[i].held[across ? 0 : 1], axis->value, across, false);
}

/* ⭐ A BRIDGED PAD ANSWERS TOO. While a pop-up is up the bridge core holds a
 * bridged pad's input, as for the overlay (bridge_app.c), and reads its d-pad,
 * Cross and Circle and its left stick from the pad's own reports (the core's
 * held_nav.inl): a key down now and not at the last reading is the pop-up's, as
 * a pad of SDL's is in the filter. */
static void bridged_pads_answer(void) {
    static const int k_keys[6] = {REMOTE_UP, REMOTE_DOWN, REMOTE_LEFT, REMOTE_RIGHT, REMOTE_OK, REMOTE_BACK};
    static const char *const k_from[6] = {
            "a bridged pad's d-pad up", "a bridged pad's d-pad down", "a bridged pad's d-pad left",
            "a bridged pad's d-pad right", "a bridged pad's Cross (A)", "a bridged pad's Circle (B)",
    };
    ctm_bridge_nav_t navs[PADS_MAX];
    const int n = ctm_bridge_held_navs(navs, PADS_MAX);
    unsigned sig = (unsigned) n + 1u;
    for (int i = 0; i < n; i++) {
        sig = sig * 31u + (navs[i].reading ? (unsigned) i + 1u : 0u);
    }
    const Uint32 ticks = SDL_GetTicks();
    if (sig != s_core_ids_sig || SDL_TICKS_PASSED(ticks, s_core_ids_at + 250u)) {
        s_core_ids_sig = sig;
        s_core_ids_at = ticks;
        s_core_ids_n = 0;
        const int joysticks = SDL_NumJoysticks();
        for (int j = 0; j < joysticks && s_core_ids_n < PADS_MAX; j++) {
            const SDL_JoystickID id = SDL_JoystickGetDeviceInstanceID(j);
            SDL_GameController *controller = SDL_GameControllerFromInstanceID(id);
            const char *path = controller != NULL ? SDL_GameControllerPath(controller) : NULL;
            for (int b = 0; path != NULL && b < n; b++) {
                if (navs[b].reading && ctm_bridge_gesture_path_is_node(path, navs[b].node)) {
                    s_core_ids[s_core_ids_n++] = id;
                    break;
                }
            }
        }
    }
    for (int i = 0; i < n; i++) {
        if (!navs[i].reading) {
            continue;
        }
        const bool now[6] = {navs[i].up, navs[i].down, navs[i].left, navs[i].right, navs[i].south, navs[i].east};
        int j = 0;
        while (j < s_bridged_n && strcmp(s_bridged[j].node, navs[i].node) != 0) {
            j++;
        }
        if (j == s_bridged_n) {
            if (s_bridged_n >= PADS_MAX) {
                continue;
            }
            snprintf(s_bridged[j].node, sizeof(s_bridged[j].node), "%s", navs[i].node);
            s_bridged[j].held[0] = STICK_UNSEEN;
            s_bridged[j].held[1] = STICK_UNSEEN;
            s_bridged_n++;
        } else {
            for (int k = 0; k < 6; k++) {
                if (now[k] && !s_bridged[j].keys[k]) {
                    keep(k_keys[k], k_from[k]);
                }
            }
        }
        memcpy(s_bridged[j].keys, now, sizeof(now));
        stick_step(&s_bridged[j].held[0], navs[i].lx, true, true);
        stick_step(&s_bridged[j].held[1], navs[i].ly, false, true);
    }
}

bool webos_popup_take_key(short vk, bool down) {
    if (!down || SDL_AtomicGet(&s_up_flag) == 0) {
        return false;
    }
    int key = 0;
    const char *from = NULL;
    switch (vk) {
        case VK_UP:
            key = REMOTE_UP;
            from = "the keyboard's up";
            break;
        case VK_DOWN:
            key = REMOTE_DOWN;
            from = "the keyboard's down";
            break;
        case VK_LEFT:
            key = REMOTE_LEFT;
            from = "the keyboard's left";
            break;
        case VK_RIGHT:
            key = REMOTE_RIGHT;
            from = "the keyboard's right";
            break;
        case VK_RETURN:
            key = REMOTE_OK;
            from = "the keyboard's Enter";
            break;
        case VK_ESCAPE:
            key = REMOTE_BACK;
            from = "the keyboard's Esc";
            break;
        case VK_BACK:
            key = REMOTE_BACK;
            from = "the keyboard's Backspace";
            break;
        default:
            return true;
    }
    SDL_AtomicLock(&s_keys_lock);
    if (s_keys_n < PENDING_MAX) {
        s_keys[s_keys_n].key = key;
        s_keys[s_keys_n].from = from;
        s_keys_n++;
    }
    SDL_AtomicUnlock(&s_keys_lock);
    return true;
}

void webos_popup_init(SDL_Window *window) {
    s_window = window;
    s_state = WINDOW_FOCUSED;
    s_since = SDL_GetTicks();
    popup_log("the app started: a controller answers a webOS pop-up over it as the remote");
}

bool webos_popup_up(void) {
    return window_state() == WINDOW_POPUP;
}

/* Is this pad of SDL's a bridged one the core reads? Then the core answers. */
static bool read_by_core(SDL_JoystickID which) {
    for (int i = 0; i < s_core_ids_n; i++) {
        if (s_core_ids[i] == which) {
            return true;
        }
    }
    return false;
}

bool webos_popup_take(const SDL_Event *event) {
    switch (event->type) {
        case SDL_CONTROLLERBUTTONDOWN:
        case SDL_CONTROLLERAXISMOTION:
        case SDL_CONTROLLERTOUCHPADDOWN:
        case SDL_CONTROLLERTOUCHPADMOTION:
        case SDL_CONTROLLERSENSORUPDATE:
            break;
        default:
            /* ⓘ A button let go and a finger lifted go on, as SDL let them
             * through without the focus: what went down before a pop-up comes
             * up again where it went down. */
            return false;
    }
    const window_state_t state = window_state();
    if (state == WINDOW_FOCUSED) {
        return false;
    }
    if (event->type == SDL_CONTROLLERAXISMOTION) {
        if (state == WINDOW_POPUP && !read_by_core(event->caxis.which)) {
            stick_as_arrow(&event->caxis);
        }
        /* A stick or trigger back at rest goes on, as SDL let it through. */
        return event->caxis.value <= -AXIS_REST || event->caxis.value >= AXIS_REST;
    }
    if (event->type != SDL_CONTROLLERBUTTONDOWN || state != WINDOW_POPUP || read_by_core(event->cbutton.which)) {
        return true;
    }
    int key = 0;
    const char *from = NULL;
    switch (event->cbutton.button) {
        case SDL_CONTROLLER_BUTTON_DPAD_UP:
            key = REMOTE_UP;
            from = "the d-pad's up";
            break;
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN:
            key = REMOTE_DOWN;
            from = "the d-pad's down";
            break;
        case SDL_CONTROLLER_BUTTON_DPAD_LEFT:
            key = REMOTE_LEFT;
            from = "the d-pad's left";
            break;
        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:
            key = REMOTE_RIGHT;
            from = "the d-pad's right";
            break;
        case SDL_CONTROLLER_BUTTON_A:
            key = REMOTE_OK;
            from = "Cross (A)";
            break;
        case SDL_CONTROLLER_BUTTON_B:
            key = REMOTE_BACK;
            from = "Circle (B)";
            break;
        default:
            return true;
    }
    keep(key, from);
    return true;
}

void webos_popup_tick(void) {
    if (s_window == NULL) {
        return;
    }
    if (!s_had_focus && SDL_GetKeyboardFocus() != NULL) {
        s_had_focus = true;
    }
    const window_state_t state = window_state();
    if (state != s_state) {
        const Uint32 now = SDL_GetTicks();
        const Uint32 lasted = now - s_since;
        const window_state_t was = s_state;
        s_state = state;
        s_since = now;
        s_sticks_n = 0;    /* each pop-up sees every stick at rest first */
        s_bridged_n = 0;   /* and a bridged pad's first reading only says what is held */
        s_core_ids_n = 0;
        s_core_ids_sig = 0;
        SDL_AtomicSet(&s_up_flag, state == WINDOW_POPUP ? 1 : 0);
        /* ⭐ SDL keeps a controller's presses without the focus for exactly as long
         * as a pop-up has it: in the background they stay dropped, as ever. */
        SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, state == WINDOW_POPUP ? "1" : "0");
        if (was == WINDOW_POPUP) {
            popup_log("the pop-up went after %u ms, %d key(s) sent as the remote; the app %s", lasted, s_sent,
                      state == WINDOW_FOCUSED ? "has the focus again" : "is in the background");
        }
        if (state == WINDOW_POPUP) {
            s_sent = 0;
            popup_log("a webOS pop-up has the focus%s: a controller answers it as the remote",
                      was == WINDOW_AWAY ? ", as the app came back" : "");
        } else if (state == WINDOW_AWAY && was == WINDOW_FOCUSED) {
            popup_log("the app is in the background");
        } else if (state == WINDOW_FOCUSED && was == WINDOW_AWAY) {
            popup_log("the app is back with the focus, after %u ms", lasted);
        }
    }
    if (state == WINDOW_POPUP) {
        bridged_pads_answer();
    }
    SDL_AtomicLock(&s_keys_lock);
    for (int i = 0; i < s_keys_n; i++) {
        keep(s_keys[i].key, s_keys[i].from);
    }
    s_keys_n = 0;
    SDL_AtomicUnlock(&s_keys_lock);
    /* ⓘ Kept by the filter in this same pass, by a keyboard's reader since the
     * last, and from a bridged pad just now. ⛔ Not once the pop-up has gone: the
     * key would reach the app. */
    for (int i = 0; i < s_pending_n; i++) {
        if (state != WINDOW_POPUP) {
            popup_log("%s: %s not sent, the pop-up had gone", s_pending[i].from, key_name(s_pending[i].key));
        } else if (remote_press(s_pending[i].key)) {
            popup_log("%s: %s sent as the remote", s_pending[i].from, key_name(s_pending[i].key));
        } else {
            popup_log("%s: %s NOT sent: %s", s_pending[i].from, key_name(s_pending[i].key), s_failed);
        }
    }
    s_pending_n = 0;
}

/* ⓘ A test pop-up's call, on a thread of its own: webOS answers in its own
 * time, and the control port's commands run inside SDL's walk of its queue. */
static SDL_SpinLock s_alert_lock = 0;
static char s_alert_id[96] = "";

static int alert_call(void *data) {
    char *payload = data;
    const bool closing = strncmp(payload, "{\"alertId\"", 10) == 0;
    char *reply = NULL;
    const bool called = HLunaServiceCallSync(closing ? "luna://com.webos.notification/closeAlert"
                                                     : "luna://com.webos.notification/createAlert",
                                             payload, true, &reply);
    popup_log("%s: %s", closing ? "the test pop-up, closed from the control port" : "a test pop-up, from the control port",
              called && reply != NULL ? reply : "no answer");
    const char *id = !closing && reply != NULL ? strstr(reply, "\"alertId\"") : NULL;
    const char *colon = id != NULL ? strchr(id + 9, ':') : NULL;
    const char *open = colon != NULL ? strchr(colon, '"') : NULL;
    const char *close = open != NULL ? strchr(open + 1, '"') : NULL;
    if (close != NULL && (size_t) (close - open - 1) < sizeof(s_alert_id)) {
        SDL_AtomicLock(&s_alert_lock);
        memcpy(s_alert_id, open + 1, (size_t) (close - open - 1));
        s_alert_id[close - open - 1] = '\0';
        SDL_AtomicUnlock(&s_alert_lock);
    }
    free(reply);
    free(payload);
    return 0;
}

bool webos_popup_test(const char *what, char *said, size_t len) {
    char *payload = NULL;
    if (what != NULL && strcasecmp(what, "test") == 0) {
        payload = strdup("{\"sourceId\":\"com.aurora.gamestream.rhqn\","
                         "\"message\":\"Aurora's test pop-up: answer it with a controller, a keyboard or the remote.\","
                         "\"buttons\":[{\"label\":\"OK\"},{\"label\":\"Cancel\"}]}");
    } else if (what != NULL && strcasecmp(what, "close") == 0) {
        char id[96];
        SDL_AtomicLock(&s_alert_lock);
        snprintf(id, sizeof(id), "%s", s_alert_id);
        s_alert_id[0] = '\0';
        SDL_AtomicUnlock(&s_alert_lock);
        if (id[0] == '\0') {
            snprintf(said, len, "ERR no test pop-up from the control port to close");
            return false;
        }
        char buf[160];
        snprintf(buf, sizeof(buf), "{\"alertId\":\"%s\"}", id);
        payload = strdup(buf);
    } else {
        snprintf(said, len, "ERR popup [test|close]");
        return false;
    }
    SDL_Thread *thread = payload != NULL ? SDL_CreateThread(alert_call, "popup-test", payload) : NULL;
    if (thread == NULL) {
        free(payload);
        snprintf(said, len, "ERR the call could not be started");
        return false;
    }
    SDL_DetachThread(thread);
    snprintf(said, len, "OK asked webOS; its answer goes to logs/webos-popup.log");
    return true;
}

bool webos_popup_send(const char *key, char *said, size_t len) {
    static const struct {
        const char *name;
        int code;
    } k_keys[] = {
            {"up", REMOTE_UP}, {"down", REMOTE_DOWN}, {"left", REMOTE_LEFT},
            {"right", REMOTE_RIGHT}, {"ok", REMOTE_OK}, {"back", REMOTE_BACK},
    };
    int code = 0;
    for (size_t i = 0; key != NULL && i < sizeof(k_keys) / sizeof(k_keys[0]); i++) {
        if (strcasecmp(key, k_keys[i].name) == 0) {
            code = k_keys[i].code;
        }
    }
    if (code == 0 && key != NULL && key[0] != '\0') {
        char *end = NULL;
        const long number = strtol(key, &end, 10);
        if (*end == '\0' && number > 0 && number <= REMOTE_KEY_MAX) {
            code = (int) number;
        }
    }
    if (code == 0) {
        snprintf(said, len, "ERR remote <up|down|left|right|ok|back|a key's number>");
        return false;
    }
    if (window_state() != WINDOW_POPUP) {
        snprintf(said, len, "ERR no webOS pop-up has the focus, and a key now would reach the app: nothing sent");
        return false;
    }
    if (!remote_press(code)) {
        popup_log("key %d from the control port NOT sent: %s", code, s_failed);
        snprintf(said, len, "ERR key %d not sent: %s", code, s_failed);
        return false;
    }
    popup_log("key %d (%s) sent as the remote from the control port", code, key_name(code));
    snprintf(said, len, "OK key %d sent as the remote on %s (%s)", code, s_node, s_node_name);
    return true;
}

void webos_popup_describe(char *out, size_t len) {
    static const char *const k_states[] = {"focused", "popup", "background"};
    const window_state_t state = window_state();
    snprintf(out, len, "popup=%s window=%s for=%ums hint=%s sent=%d sent_all=%d last=%d node=%s name=\"%s\"",
             state == WINDOW_POPUP ? "yes" : "no", k_states[state], SDL_GetTicks() - s_since,
             SDL_GetHintBoolean(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, SDL_FALSE) ? "on" : "off", s_sent,
             s_sent_all, s_last_key, s_node[0] != '\0' ? s_node : "-", s_node_name);
}

#else

void webos_popup_init(SDL_Window *window) {
    (void) window;
}

bool webos_popup_up(void) {
    return false;
}

bool webos_popup_take(const SDL_Event *event) {
    (void) event;
    return false;
}

void webos_popup_tick(void) {
}

bool webos_popup_take_key(short vk, bool down) {
    (void) vk;
    (void) down;
    return false;
}

bool webos_popup_send(const char *key, char *said, size_t len) {
    (void) key;
    snprintf(said, len, "ERR only on webOS");
    return false;
}

void webos_popup_describe(char *out, size_t len) {
    snprintf(out, len, "popup=no (not webOS)");
}

bool webos_popup_test(const char *what, char *said, size_t len) {
    (void) what;
    snprintf(said, len, "ERR only on webOS");
    return false;
}

#endif
