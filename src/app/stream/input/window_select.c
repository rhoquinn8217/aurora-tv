/* Window Select, L1 and R1 in Desktop Mode. The why is in the header. */

#include "window_select.h"

#include <SDL.h>

#include <Limelight.h>

#include "app.h"
#include "stream/session.h"
#include "stream/input/session_input.h"
#include "stream/input/vk.h"
#if defined(TARGET_WEBOS)
#include "input/ctm_bridge_gesture.h"
#endif

/* 1 while Alt is held down by L1 or R1. */
static SDL_atomic_t s_held;
/* 1 when a keyboard's key let Alt go from its own thread: logged on the app's loop. */
static SDL_atomic_t s_let_go_by_key;

static void note(const char *what, const char *why) {
#if defined(TARGET_WEBOS)
    ctm_bridge_gesture_note("window select: Alt %s (%s)", what, why);
#else
    (void) what;
    (void) why;
#endif
}

static stream_input_t *current_input(void) {
    return global != NULL && global->session != NULL ? session_get_input(global->session) : NULL;
}

/* Alt let go if held, exactly once whichever thread asks. */
static bool let_go(stream_input_t *input) {
    if (!SDL_AtomicCAS(&s_held, 1, 0)) {
        return false;
    }
    if (input != NULL) {
        stream_input_send_key_event(input, VK_LMENU, false, 0);
    }
    return true;
}

bool window_select_held(void) {
    return SDL_AtomicGet(&s_held) != 0;
}

bool window_select_keeps(short vk) {
    switch (vk) {
        case VK_TAB:
        case VK_SHIFT:
        case VK_LSHIFT:
        case VK_RSHIFT:
        case VK_LEFT:
        case VK_RIGHT:
        case VK_UP:
        case VK_DOWN:
            return true;
        default:
            return false;
    }
}

void window_select_step(stream_input_t *input, bool back) {
    if (input == NULL) {
        return;
    }
    const char alt = (char) MODIFIER_ALT;
    const char alt_shift = (char) (MODIFIER_ALT | MODIFIER_SHIFT);
    if (SDL_AtomicCAS(&s_held, 0, 1)) {
        stream_input_send_key_event(input, VK_LMENU, true, alt);
        note("held", back ? "L1" : "R1");
    }
    if (back) {
        stream_input_send_key_event(input, VK_LSHIFT, true, alt_shift);
        stream_input_send_key_event(input, VK_TAB, true, alt_shift);
        stream_input_send_key_event(input, VK_TAB, false, alt_shift);
        stream_input_send_key_event(input, VK_LSHIFT, false, alt);
    } else {
        stream_input_send_key_event(input, VK_TAB, true, alt);
        stream_input_send_key_event(input, VK_TAB, false, alt);
    }
}

void window_select_let_go(stream_input_t *input, const char *why) {
    if (let_go(input)) {
        note(input != NULL ? "let go" : "forgotten, no stream", why);
    }
}

void window_select_cancel(stream_input_t *input) {
    if (!window_select_held() || input == NULL) {
        return;
    }
    stream_input_send_key_event(input, VK_ESCAPE, true, (char) MODIFIER_ALT);
    stream_input_send_key_event(input, VK_ESCAPE, false, (char) MODIFIER_ALT);
    window_select_let_go(input, "Circle: the picker cancelled");
}

void window_select_before_key(stream_input_t *input, short vk) {
    if (window_select_held() && !window_select_keeps(vk) && let_go(input)) {
        SDL_AtomicSet(&s_let_go_by_key, 1);
    }
}

void window_select_after_click(void) {
    if (window_select_held()) {
        window_select_let_go(current_input(), "a click went through first");
    }
}

void window_select_tick(void) {
    if (SDL_AtomicCAS(&s_let_go_by_key, 1, 0)) {
        note("let go", "a key on a keyboard");
    }
}
