/* Press and drag at the sign-in, a prompt and in Desktop Mode. The why is in the header. */

#include "touchpad_drag_lock.h"

#include <stddef.h>

#include <Limelight.h>

#include "stream/input/lock_keys.h"
#include "stream/input/window_select.h"
#if defined(TARGET_WEBOS)
#include "input/ctm_bridge_gesture.h"
#define drag_note(...) ctm_bridge_gesture_note("press and drag: " __VA_ARGS__)
#else
#define drag_note(...) ((void) 0)
#endif

/* The touchpads whose left button is kept down, by their state; one per pad on the
 * stream, and the app takes four at most. */
#define DRAG_SLOTS 8

static const void *s_held[DRAG_SLOTS];

static int slot_of(const void *state) {
    for (int i = 0; i < DRAG_SLOTS; ++i) {
        if (s_held[i] == state) {
            return i;
        }
    }
    return -1;
}

static bool hold(const void *state) {
    if (slot_of(state) >= 0) {
        return true;
    }
    for (int i = 0; i < DRAG_SLOTS; ++i) {
        if (s_held[i] == NULL) {
            s_held[i] = state;
            return true;
        }
    }
    return false;   /* no room: the release goes as it always did */
}

static bool let_go(const void *state) {
    const int i = slot_of(state);
    if (i < 0) {
        return false;
    }
    s_held[i] = NULL;
    return true;
}

bool touchpad_drag_lock_click(const void *state, bool down, uint8_t fingers) {
    if (state == NULL) {
        return false;
    }
    if (down) {
        return slot_of(state) >= 0;
    }
    if (lock_keys_active() && fingers != 0 && hold(state)) {
        drag_note("the click let go with a finger on the pad: the left button stays down until it lifts");
        return true;
    }
    let_go(state);   /* let go for real: the caller sends it */
    return false;
}

void touchpad_drag_lock_finger_up(const void *state, uint8_t fingers) {
    if (fingers != 0 || !let_go(state)) {
        return;
    }
    LiSendMouseButtonEvent(BUTTON_ACTION_RELEASE, BUTTON_LEFT);
    /* The click is over: Window Select's Alt goes after it, as after any click
     * (stream/input/window_select.h). */
    window_select_after_click();
    drag_note("the last finger lifted: the left button up");
}

void touchpad_drag_lock_drop(const void *state) {
    if (!let_go(state)) {
        return;
    }
    LiSendMouseButtonEvent(BUTTON_ACTION_RELEASE, BUTTON_LEFT);
    drag_note("the touchpad was reset: the left button up");
}
