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

/* The left stick as the d-pad, as Aurora's own screens take it
 * (lv_drv_sdl_key.c): an arrow each time it is pushed past half way. ⓘ Only
 * once it has been seen short of that in this pop-up, so a stick already held
 * as the pop-up comes moves nothing. */
static void stick_as_arrow(const SDL_ControllerAxisEvent *axis) {
    int across;
    if (axis->axis == SDL_CONTROLLER_AXIS_LEFTX) {
        across = 1;
    } else if (axis->axis == SDL_CONTROLLER_AXIS_LEFTY) {
        across = 0;
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
    int *held = &s_sticks[i].held[across ? 0 : 1];
    const int dir = axis->value <= -STICK_PUSH ? -1 : (axis->value >= STICK_PUSH ? 1 : 0);
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
    if (dir < 0) {
        keep(across ? REMOTE_LEFT : REMOTE_UP, across ? "the left stick's left" : "the left stick's up");
    } else if (dir > 0) {
        keep(across ? REMOTE_RIGHT : REMOTE_DOWN, across ? "the left stick's right" : "the left stick's down");
    }
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
        if (state == WINDOW_POPUP) {
            stick_as_arrow(&event->caxis);
        }
        /* A stick or trigger back at rest goes on, as SDL let it through. */
        return event->caxis.value <= -AXIS_REST || event->caxis.value >= AXIS_REST;
    }
    if (event->type != SDL_CONTROLLERBUTTONDOWN || state != WINDOW_POPUP) {
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
    /* ⓘ Kept by the filter in this same pass. ⛔ Not once the pop-up has gone:
     * the key would reach the app. */
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
    if (state == s_state) {
        return;
    }
    const Uint32 now = SDL_GetTicks();
    const Uint32 lasted = now - s_since;
    const window_state_t was = s_state;
    s_state = state;
    s_since = now;
    s_sticks_n = 0;   /* each pop-up sees every stick at rest first */
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

bool webos_popup_send(const char *key, char *said, size_t len) {
    (void) key;
    snprintf(said, len, "ERR only on webOS");
    return false;
}

void webos_popup_describe(char *out, size_t len) {
    snprintf(out, len, "popup=no (not webOS)");
}

#endif
