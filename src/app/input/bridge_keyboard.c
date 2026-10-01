/* A bridged keyboard is the bridge's. The why is in the header. */

#include "bridge_keyboard.h"

#if defined(TARGET_WEBOS)

#include <SDL.h>

#include "app.h"            /* global */
#include "logging.h"
#include "ctm_bridge_glue.h"
#include "ctm_bridge_gesture.h"
#include "bridge_override.h"
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
    if (down && vk == VK_O && (modifiers & chord) == chord) {
        /* The host saw Ctrl, Alt and Shift go down, and will not see them
         * come up: the overlay has the keyboard from here. */
        stream_input_flush_pressed_keys(input);
        bus_pushevent(USER_OPEN_OVERLAY, NULL, NULL);
        commons_log_info("Input", "Keyboard evdev: overlay shortcut pressed");
        return true;
    }
    return false;
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
    if (s_look_again_at == 0 || !SDL_TICKS_PASSED(SDL_GetTicks(), s_look_again_at)) {
        return;
    }
    s_look_again_at = 0;
    if (input == NULL) {
        return;
    }
    /* Let go of everything, then take again: the scan skips what is bridged
     * (bridge_keyboard_node_is_bridged) and finds what has been released.
     * ⓘ Bridge Override keeps the TV's grab off altogether. */
    session_input_set_keyboard_grab(input, false);
    if (!bridge_override_active()) {
        session_input_set_keyboard_grab(input, true);
        /* Taken with the overlay open: they are the overlay's until it closes. */
        if (s_overlay_open) {
            session_input_hold_keyboard_grab(input, true);
        }
    }
    commons_log_info("Input", "Keyboard grab looked again after a bridge change (override %s)",
                     bridge_override_active() ? "on, grab left off" : "off");
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
