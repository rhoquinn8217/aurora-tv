/* A bridged keyboard is the bridge's. The why is in the header. */

#include "bridge_keyboard.h"

#if defined(TARGET_WEBOS)

#include <stdarg.h>
#include <stdio.h>
#include <time.h>

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

/* When the look-again is due, in SDL ticks. 0: nothing is due. */
static Uint32 s_look_again_at = 0;

/* The overlay is open: the TV's keyboards are let go for it, and the reader
 * sends the host nothing. ⓘ Written by the app's loop, read by the reader
 * thread; a key that crosses the change either way is harmless. */
static volatile bool s_overlay_open = false;

/* A shortcut the reader thread found, waiting for the app's loop to act on it:
 * ending a stream and opening the overlay both belong to that loop. ⓘ One
 * slot is enough, since the loop comes round every millisecond. */
enum {
    SHORTCUT_NONE = 0,
    SHORTCUT_OVERLAY,   /* Ctrl+Alt+Shift+O, ours */
    SHORTCUT_STATS,     /* Ctrl+Alt+Shift+S, Moonlight's; here it opens the overlay */
    SHORTCUT_QUIT,      /* Ctrl+Alt+Shift+Q, Moonlight's: ends the stream */
};
static SDL_atomic_t s_shortcut;

/* The record a person can read on any set: logs/tv-keyboard.log, beside the
 * bridge core's own logs and stamped with the same clock. ⓘ commons_log goes
 * to PmLog, which a C1 does not let the developer account read.
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

bool bridge_keyboard_node_is_bridged(const char *event_path) {
    /* ⛔ Not before the bridge is running: nothing can be bridged outside a
     * stream, and asking the glue would bring the core up early. */
    if (event_path == NULL || !ctm_bridge_active()) {
        return false;
    }
    return ctm_bridge_gesture_event_is_bridged(event_path);
}

void bridge_keyboard_changed(void) {
    s_look_again_at = SDL_GetTicks() + LOOK_AGAIN_MS;
    if (s_look_again_at == 0) {
        s_look_again_at = 1;
    }
}

void bridge_keyboard_before_plug(void) {
    stream_input_t *input = live_input();
    if (input == NULL) {
        return;
    }
    /* ⓘ Joins the TV's reader thread, which wakes at least every 200 ms. With
     * nothing grabbed it returns at once, so several parts of one device cost
     * this once. */
    session_input_set_keyboard_grab(input, false);
    bridge_keyboard_changed();
}

bool bridge_keyboard_evdev_key(stream_input_t *input, short vk, bool down, char modifiers) {
    if (s_overlay_open) {
        return true;   /* the overlay has the keyboard */
    }
    const char chord = MODIFIER_CTRL | MODIFIER_ALT | MODIFIER_SHIFT;
    if (!down || (modifiers & chord) != chord) {
        return false;
    }
    int shortcut;
    switch (vk) {
        case VK_O:
            shortcut = SHORTCUT_OVERLAY;
            break;
        case VK_S:
            shortcut = SHORTCUT_STATS;
            break;
        case VK_Q:
            shortcut = SHORTCUT_QUIT;
            break;
        default:
            /* ⓘ Moonlight's Z, X, M, C and D only write a log line in this app
             * (performPendingSpecialKeyCombo), so they stay the host's. */
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

/* What a shortcut does, the same as on the keyboard path SDL feeds
 * (performPendingSpecialKeyCombo in session_keyboard.c). */
static void act_on_shortcut(int shortcut) {
    switch (shortcut) {
        case SHORTCUT_OVERLAY:
        case SHORTCUT_STATS: {
            const char key = shortcut == SHORTCUT_OVERLAY ? 'O' : 'S';
            commons_log_info("Input", "Keyboard evdev: Ctrl+Alt+Shift+%c, opening the overlay", key);
            keyboard_log("shortcut Ctrl+Alt+Shift+%c on a keyboard the TV has: opening the overlay", key);
            bus_pushevent(USER_OPEN_OVERLAY, NULL, NULL);
            break;
        }
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

void bridge_keyboard_tick(bool overlay_shown) {
    stream_input_t *input = live_input();
    if (overlay_shown != s_overlay_open) {
        s_overlay_open = overlay_shown;
        /* ⓘ Let go and taken again through the reader's own handles, with the
         * reader left running: stopping it would hold the overlay up for as
         * long as its thread takes to notice. */
        if (input != NULL) {
            session_input_hold_keyboard_grab(input, overlay_shown);
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
    if (s_look_again_at == 0 || !SDL_TICKS_PASSED(SDL_GetTicks(), s_look_again_at)) {
        return;
    }
    s_look_again_at = 0;
    if (input == NULL) {
        return;
    }
    /* Let go of everything, then take again: the scan skips what is bridged
     * (bridge_keyboard_node_is_bridged) and finds what has been released.
     * ⓘ Whatever Bridge Override says: it leaves the keyboard alone. */
    session_input_set_keyboard_grab(input, false);
    session_input_set_keyboard_grab(input, true);
    /* Taken with the overlay open: they are the overlay's until it closes. */
    if (s_overlay_open) {
        session_input_hold_keyboard_grab(input, true);
    }
    const bool holding = input->keyboard_evdev != NULL;
    commons_log_info("Input", "Keyboard grab looked again after a bridge change: %s",
                     holding ? "holding the keyboards that are not bridged" : "no keyboard to hold");
    keyboard_log("grab looked again after a bridge change: %s",
                 holding ? "holding the keyboards that are not bridged" : "no keyboard to hold");
}

#else /* !TARGET_WEBOS */

bool bridge_keyboard_node_is_bridged(const char *event_path) {
    (void) event_path;
    return false;
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

#endif /* TARGET_WEBOS */
