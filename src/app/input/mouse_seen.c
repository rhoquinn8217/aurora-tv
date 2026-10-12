/* Which mice the TV has seen move. The why is in the header. */

#include "mouse_seen.h"

#if defined(TARGET_WEBOS)

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <SDL.h>

#include "ctm_bridge_gesture.h"
#include "tv_cursor.h"

#define MOUSE_NODES_MAX 8          /* pointers watched at once */
#define MOUSE_SEEN_MAX 8           /* pointers remembered as having moved */
#define MOUSE_NODE_NUMBERS 64      /* /dev/input/event0 to event63 */
#define MOUSE_SCAN_MS 1000u        /* how often the device list is asked whether it changed */

/* The kernel's input event as it is laid out for a program, and what a
 * pointer's movement is in it. ⓘ Written out rather than taken from
 * linux/input.h, as bridge_keyboard.c does, for the same reason. */
struct evdev_event {
    unsigned long sec;
    unsigned long usec;
    unsigned short type;
    unsigned short code;
    int value;
};
#define EVDEV_TYPE_REL 2
#define EVDEV_REL_X 0
#define EVDEV_REL_Y 1
/* linux/input.h's EVIOCGNAME and EVIOCGBIT(EV_REL). */
#define EVDEV_GET_NAME(len) _IOC(_IOC_READ, 'E', 0x06, len)
#define EVDEV_GET_RELS(len) _IOC(_IOC_READ, 'E', 0x20 + EVDEV_TYPE_REL, len)

static int s_fds[MOUSE_NODES_MAX];
static char s_paths[MOUSE_NODES_MAX][24];
static int s_n = 0;
static char s_seen[MOUSE_SEEN_MAX][24];
static int s_seen_n = 0;
static Uint32 s_scan_at = 0;
static Uint32 s_hash = 0;
static bool s_bridged_moving = false;

/* Every input device the kernel has, as one number that changes when one
 * arrives, goes or comes back: the names in /sys/class/input, as
 * bridge_keyboard.c reads them. 0 when they cannot be listed. */
static Uint32 devices_hash(void) {
    DIR *dir = opendir("/sys/class/input");
    if (dir == NULL) {
        return 0;
    }
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
        sum += h;
    }
    closedir(dir);
    return sum | 1u;
}

static bool seen(const char *path) {
    for (int i = 0; i < s_seen_n; i++) {
        if (strcmp(s_seen[i], path) == 0) {
            return true;
        }
    }
    return false;
}

/* A pointer: it moves on X and Y. */
static bool is_pointer(int fd) {
    unsigned long rels[1] = {0};
    if (ioctl(fd, EVDEV_GET_RELS(sizeof(rels)), rels) < 0) {
        return false;
    }
    return (rels[0] & (1ul << EVDEV_REL_X)) != 0 && (rels[0] & (1ul << EVDEV_REL_Y)) != 0;
}

static void close_all(void) {
    for (int i = 0; i < s_n; i++) {
        close(s_fds[i]);
    }
    s_n = 0;
}

/* Every pointer not yet seen moving, opened to be read and never taken: webOS
 * and the bridge go on reading them as before. */
static void watch_again(void) {
    close_all();
    for (int number = 0; number < MOUSE_NODE_NUMBERS && s_n < MOUSE_NODES_MAX; number++) {
        char path[24];
        snprintf(path, sizeof(path), "/dev/input/event%d", number);
        if (seen(path)) {
            continue;
        }
        const int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) {
            continue;
        }
        if (!is_pointer(fd)) {
            close(fd);
            continue;
        }
        s_fds[s_n] = fd;
        snprintf(s_paths[s_n], sizeof(s_paths[s_n]), "%s", path);
        s_n++;
    }
}

/* The node at i is done with: the last one takes its place. */
static void drop(int i) {
    close(s_fds[i]);
    s_n--;
    if (i != s_n) {
        s_fds[i] = s_fds[s_n];
        memcpy(s_paths[i], s_paths[s_n], sizeof(s_paths[i]));
    }
}

void mouse_seen_tick(void) {
    const Uint32 now = SDL_GetTicks();
    if (s_scan_at == 0 || SDL_TICKS_PASSED(now, s_scan_at)) {
        s_scan_at = now + MOUSE_SCAN_MS;
        if (s_scan_at == 0) {
            s_scan_at = 1;
        }
        const Uint32 hash = devices_hash();
        if (hash != s_hash) {
            /* ⓘ A node's number can pass to another device: what moved is
             * forgotten, and every pointer is watched again. */
            s_hash = hash;
            s_seen_n = 0;
            watch_again();
        }
    }
    for (int i = 0; i < s_n; i++) {
        /* ⓘ A bounded burst, so a node with a great deal to say cannot hold
         * the app's loop; and the first move closes it anyway. */
        for (int burst = 0; burst < 64; burst++) {
            struct evdev_event ev;
            const ssize_t got = read(s_fds[i], &ev, sizeof(ev));
            if (got != (ssize_t) sizeof(ev)) {
                if (got < 0 && errno == ENODEV) {
                    drop(i);
                    i--;
                }
                break;
            }
            if (ev.type != EVDEV_TYPE_REL || (ev.code != EVDEV_REL_X && ev.code != EVDEV_REL_Y) || ev.value == 0) {
                continue;
            }
            char name[96] = "";
            (void) ioctl(s_fds[i], EVDEV_GET_NAME(sizeof(name) - 1), name);
            if (s_seen_n < MOUSE_SEEN_MAX) {
                memcpy(s_seen[s_seen_n++], s_paths[i], sizeof(s_seen[0]));
            }
            tv_cursor_log("the TV saw %s (%s) move: bridged, it asks about the TV cursor at once",
                          s_paths[i], name[0] != '\0' ? name : "no name");
            drop(i);
            i--;
            break;
        }
    }
}

void mouse_seen_plugged(const ctm_bridge_dev_t *dev) {
    if (dev == NULL || strcmp(dev->type, "mouse") != 0) {
        return;
    }
    for (int i = 0; i < s_seen_n; i++) {
        if (ctm_bridge_gesture_event_on_node(s_seen[i], dev->node)) {
            s_bridged_moving = true;
            tv_cursor_log("a mouse the TV saw move is bridged (%s, %s): the pop-up is due", s_seen[i], dev->node);
            return;
        }
    }
    tv_cursor_log("a mouse part the TV never saw move is bridged (%s): the pop-up waits for its first use",
                  dev->node);
}

bool mouse_seen_take_bridged(void) {
    const bool due = s_bridged_moving;
    s_bridged_moving = false;
    return due;
}

#else

void mouse_seen_tick(void) {
}

void mouse_seen_plugged(const ctm_bridge_dev_t *dev) {
    (void) dev;
}

bool mouse_seen_take_bridged(void) {
    return false;
}

#endif
