/* The buttons that type at Windows' lock screen. The why is in the header. */

#include "lock_keys.h"

#include <stdint.h>
#include <string.h>

#include <Limelight.h>

#include "app.h"
#include "input/app_input.h"
#include "input/input_gamepad.h"
#include "stream/input/bridge_switch_hold.h"
#include "stream/input/keyboard_input.h"
#include "stream/input/session_input.h"
#include "stream/input/vk.h"
#include "stream/session.h"
#include "ui/common/pad_marks.h"
#include "ui/root.h"
#include "util/bus.h"
#include "util/user_event.h"
#if defined(TARGET_WEBOS)
#include "ctm_bridge_glue.h"
#include "input/ctm_bridge_gesture.h"
#endif

#define LOCK_SLOTS 16
#define LOCK_QUEUE 8
/* How often DS5-USBIP is asked while Windows is locked: it looks twice a second
 * itself, so the unlock is known within about a second. */
#define LOCK_ASK_MS 500
/* A press after this long with none: a lock that came while nobody was playing
 * is known at once, not at the probe's next ten seconds. Not a probe per press
 * in a game. */
#define LOCK_IDLE_MS 30000
/* As the on-screen keyboard reads L2, and well below that to let go. */
#define LOCK_TRIGGER_DOWN 16000
#define LOCK_TRIGGER_UP 8000

/* The triggers, as two more buttons. */
#define LOCK_L2 (SDL_CONTROLLER_BUTTON_MAX)
#define LOCK_R2 (SDL_CONTROLLER_BUTTON_MAX + 1)
#define LOCK_BIT(what) (1u << (unsigned) (what))

/* The overlay's chord (QUIT_BUTTONS in session_gamepad.c). */
#define LOCK_CHORD (LOCK_BIT(SDL_CONTROLLER_BUTTON_START) | LOCK_BIT(SDL_CONTROLLER_BUTTON_BACK) | \
                    LOCK_BIT(SDL_CONTROLLER_BUTTON_LEFTSHOULDER) | LOCK_BIT(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER))

/* ⭐ THE ORDER OF THE PRESSES IS THE ORDER OF THE DIGITS. The chord's four buttons
 * are also a 7, an 8, Backspace and the keyboard, so they cannot act until they
 * are let go, or holding the chord would type. The rest act as they go down. A
 * press waits here behind an earlier one not decided yet, so 7 then 1 rolled
 * together types 71 and not 17. */
typedef struct {
    uint8_t what;
    bool decided;
} lock_press_t;

typedef struct {
    SDL_JoystickID instance_id;
    uint32_t down;   /* what has gone down since this began and not come up */
    bool chord;      /* the chord was made: nothing acts until it is let go */
    lock_press_t queue[LOCK_QUEUE];
    int queued;
} lock_pad_t;

static lock_pad_t s_pads[LOCK_SLOTS];
static bool s_active;
static bool s_hidden;
static bool s_interface_had_input;
static unsigned s_typed;
#if defined(TARGET_WEBOS)
static uint32_t s_last_ask;
static uint32_t s_last_press;
#endif

bool lock_keys_active(void) {
    return s_active;
}

bool lock_keys_numpad_hidden(void) {
    return s_hidden;
}

unsigned lock_keys_typed(void) {
    return s_typed;
}

static bool pad_on_stream(const stream_input_t *input, const app_gamepad_state_t *gamepad) {
    return gamepad != NULL && gamepad->controller != NULL && gamepad->gs_id >= 0 && gamepad->gs_id < LOCK_SLOTS &&
           (input->moonlightExcludedMask & (1u << gamepad->gs_id)) == 0;
}

bool lock_keys_pad_on_stream(void) {
    if (global == NULL || global->session == NULL) {
        return false;
    }
    const stream_input_t *input = session_get_input(global->session);
    if (input == NULL) {
        return false;
    }
    const short max = app_input_get_max_gamepads(&global->input);
    for (short i = 0; i < max; ++i) {
        if (pad_on_stream(input, app_input_gamepad_state_by_index(&global->input, i))) {
            return true;
        }
    }
    return false;
}

/* This pad's place, emptied if another pad had it. */
static lock_pad_t *pad_for(const app_gamepad_state_t *gamepad) {
    lock_pad_t *pad = &s_pads[gamepad->gs_id];
    if (pad->instance_id != gamepad->instance_id) {
        memset(pad, 0, sizeof *pad);
        pad->instance_id = gamepad->instance_id;
    }
    return pad;
}

/* A trigger already held does not type: it counts as down, and has to be let go. */
static void pad_forget(lock_pad_t *pad, const app_gamepad_state_t *gamepad) {
    pad->down = 0;
    pad->chord = false;
    pad->queued = 0;
    if (SDL_GameControllerGetAxis(gamepad->controller, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > LOCK_TRIGGER_UP) {
        pad->down |= LOCK_BIT(LOCK_L2);
    }
    if (SDL_GameControllerGetAxis(gamepad->controller, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > LOCK_TRIGGER_UP) {
        pad->down |= LOCK_BIT(LOCK_R2);
    }
}

static short key_for(uint8_t what) {
    switch (what) {
        case SDL_CONTROLLER_BUTTON_DPAD_UP:
            return VK_1;
        case SDL_CONTROLLER_BUTTON_DPAD_LEFT:
            return VK_2;
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN:
            return VK_3;
        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:
            return VK_4;
        case LOCK_L2:
            return VK_5;
        case LOCK_R2:
            return VK_6;
        case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:
            return VK_7;
        case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER:
            return VK_8;
        case SDL_CONTROLLER_BUTTON_Y:
            return VK_9;
        case SDL_CONTROLLER_BUTTON_X:
            return VK_0;
        case SDL_CONTROLLER_BUTTON_A:
            return VK_RETURN;
        case SDL_CONTROLLER_BUTTON_BACK:
            return VK_BACK;
        default:
            return 0;
    }
}

static bool does_something(uint8_t what) {
    return key_for(what) != 0 || what == SDL_CONTROLLER_BUTTON_START || what == SDL_CONTROLLER_BUTTON_B;
}

static void act(stream_input_t *input, uint8_t what) {
    const short key = key_for(what);
    if (key != 0) {
        stream_input_send_key_event(input, key, true, 0);
        stream_input_send_key_event(input, key, false, 0);
        s_typed++;
    } else if (what == SDL_CONTROLLER_BUTTON_START) {
        bus_pushevent(USER_OPEN_SOFT_KEYBOARD, NULL, NULL);
    } else if (what == SDL_CONTROLLER_BUTTON_B) {
        s_hidden = !s_hidden;
    }
}

/* What is decided, from the front, up to the first that is not. */
static void queue_run(stream_input_t *input, lock_pad_t *pad) {
    int done = 0;
    while (done < pad->queued && pad->queue[done].decided) {
        act(input, pad->queue[done].what);
        done++;
    }
    if (done > 0) {
        memmove(pad->queue, pad->queue + done, (size_t) (pad->queued - done) * sizeof pad->queue[0]);
        pad->queued -= done;
    }
}

static void pad_press(stream_input_t *input, lock_pad_t *pad, uint8_t what) {
    if (pad->down & LOCK_BIT(what)) {
        return;
    }
    pad->down |= LOCK_BIT(what);
    if ((pad->down & LOCK_CHORD) == LOCK_CHORD) {
        /* The chord: its four buttons were not presses after all. */
        pad->chord = true;
        int kept = 0;
        for (int i = 0; i < pad->queued; ++i) {
            if ((LOCK_BIT(pad->queue[i].what) & LOCK_CHORD) == 0) {
                pad->queue[kept++] = pad->queue[i];
            }
        }
        pad->queued = kept;
        queue_run(input, pad);
        return;
    }
    if (pad->chord || !does_something(what)) {
        return;
    }
    if (pad->queued == LOCK_QUEUE) {
        return;   /* more than anyone holds: not a PIN */
    }
    pad->queue[pad->queued].what = what;
    pad->queue[pad->queued].decided = (LOCK_BIT(what) & LOCK_CHORD) == 0;
    pad->queued++;
    queue_run(input, pad);
}

static void pad_release(stream_input_t *input, lock_pad_t *pad, uint8_t what) {
    /* ⓘ Not down as far as this knows: it went down before this began, or while
     * the overlay or the keyboard had the input. */
    if ((pad->down & LOCK_BIT(what)) == 0) {
        return;
    }
    pad->down &= ~LOCK_BIT(what);
    for (int i = 0; i < pad->queued; ++i) {
        if (pad->queue[i].what == what && !pad->queue[i].decided) {
            pad->queue[i].decided = true;
            break;
        }
    }
    queue_run(input, pad);
    if (pad->chord && (pad->down & LOCK_CHORD) == 0) {
        pad->chord = false;
        bus_pushevent(USER_OPEN_OVERLAY, NULL, NULL);
    }
}

bool lock_keys_button(stream_input_t *input, app_gamepad_state_t *gamepad, const SDL_ControllerButtonEvent *event) {
    const bool down = event->type == SDL_CONTROLLERBUTTONDOWN;
    if (down) {
        pad_family_note(gamepad->instance_id);
#if defined(TARGET_WEBOS)
        const uint32_t now = SDL_GetTicks();
        if (!s_active && now - s_last_press >= LOCK_IDLE_MS && ctm_bridge_active()) {
            ctm_bridge_agent_recheck();
        }
        s_last_press = now;
#endif
    }
    /* ⓘ The touchpad's click stays the stream's: as the mouse, it clicks. */
    if (!s_active || !pad_on_stream(input, gamepad) || event->button == SDL_CONTROLLER_BUTTON_TOUCHPAD) {
        return false;
    }
    lock_pad_t *pad = pad_for(gamepad);
    if (down) {
        pad_press(input, pad, event->button);
    } else {
        pad_release(input, pad, event->button);
    }
    return true;
}

void lock_keys_axis(stream_input_t *input, app_gamepad_state_t *gamepad, const SDL_ControllerAxisEvent *event) {
    if (!s_active || !pad_on_stream(input, gamepad)) {
        return;
    }
    uint8_t what;
    if (event->axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT) {
        what = LOCK_L2;
    } else if (event->axis == SDL_CONTROLLER_AXIS_TRIGGERRIGHT) {
        what = LOCK_R2;
    } else {
        return;
    }
    lock_pad_t *pad = pad_for(gamepad);
    if (event->value > LOCK_TRIGGER_DOWN) {
        if ((pad->down & LOCK_BIT(what)) == 0) {
            pad_family_note(gamepad->instance_id);
        }
        pad_press(input, pad, what);
    } else if (event->value < LOCK_TRIGGER_UP) {
        pad_release(input, pad, what);
    }
}

static void lock_begin(stream_input_t *input) {
    s_active = true;
    s_hidden = false;
    s_interface_had_input = false;
    memset(s_pads, 0, sizeof s_pads);
    const short max = app_input_get_max_gamepads(input->input);
    for (short i = 0; i < max; ++i) {
        app_gamepad_state_t *gamepad = app_input_gamepad_state_by_index(input->input, i);
        if (!pad_on_stream(input, gamepad)) {
            continue;
        }
        gamepad->buttons = 0;
        pad_forget(pad_for(gamepad), gamepad);
        LiSendMultiControllerEvent(gamepad->gs_id, input->input->activeGamepadMask, 0, 0, 0, 0, 0, 0, 0);
    }
#if defined(TARGET_WEBOS)
    ctm_bridge_gesture_note("lock keys: Windows is locked, a controller on the stream types");
#endif
}

static void lock_end(stream_input_t *input) {
    s_active = false;
    memset(s_pads, 0, sizeof s_pads);
#if defined(TARGET_WEBOS)
    ctm_bridge_gesture_note("lock keys: Windows is unlocked, the controllers are the stream's again");
#endif
    if (input == NULL || !keyboard_input_available(input)) {
        return;
    }
    const short max = app_input_get_max_gamepads(input->input);
    for (short i = 0; i < max; ++i) {
        app_gamepad_state_t *gamepad = app_input_gamepad_state_by_index(input->input, i);
        if (!pad_on_stream(input, gamepad)) {
            continue;
        }
        /* What is held now stays out of the stream until it is let go: the Enter
         * that unlocked must not press anything in a game. */
        gamepad->buttons = 0;
        bridge_switch_hold_keep_quiet(input, gamepad);
        bridge_switch_hold_send(gamepad, input->input->activeGamepadMask, false);
    }
}

void lock_keys_tick(app_t *app) {
    stream_input_t *input = app->session != NULL ? session_get_input(app->session) : NULL;
    bool locked = false;
#if defined(TARGET_WEBOS)
    locked = input != NULL && keyboard_input_available(input) && ctm_bridge_active() && ctm_bridge_agent_locked();
#endif
    if (locked != s_active) {
        if (locked) {
            lock_begin(input);
        } else {
            lock_end(input);
        }
    }
    if (!s_active) {
        return;
    }
#if defined(TARGET_WEBOS)
    const uint32_t now = SDL_GetTicks();
    if (now - s_last_ask >= LOCK_ASK_MS) {
        s_last_ask = now;
        ctm_bridge_agent_recheck();
    }
#endif
    /* ⓘ While the overlay or the keyboard has the input, what comes up is not seen
     * here. So everything is forgotten as they take it. */
    const bool interface_has_input = ui_should_block_input();
    if (interface_has_input != s_interface_had_input) {
        s_interface_had_input = interface_has_input;
        const short max = app_input_get_max_gamepads(input->input);
        for (short i = 0; i < max; ++i) {
            app_gamepad_state_t *gamepad = app_input_gamepad_state_by_index(input->input, i);
            if (pad_on_stream(input, gamepad)) {
                pad_forget(pad_for(gamepad), gamepad);
            }
        }
    }
}
