/* A controller at Windows' lock screen. The why is in the header. */

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
#include "stream/input/session_virt_mouse.h"
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
/* A press after this long with none: a lock that came while nobody was playing is
 * known at once, not at the probe's next ask. Not a probe per press in a game.
 * ⓘ The probe asks every second during a stream since 2026-11-09, so this gains a
 * second at most now. */
#define LOCK_IDLE_MS 30000
/* As the on-screen keyboard reads L2, and well below that to let go. */
#define LOCK_TRIGGER_DOWN 16000
#define LOCK_TRIGGER_UP 8000
/* The most a bridged pad stays held after a prompt, waiting for what was down on it
 * to be let go: a reading stuck down must not leave it dead in the game. */
#define LOCK_QUIET_MS 5000
/* Options held this long with nothing else turns this off, as Select held as long
 * pins the stats (GAMEPAD_HOLD_STATS_MS). */
#define LOCK_DISMISS_MS 4000

/* The triggers, as two more buttons, while they type. */
#define LOCK_L2 (SDL_CONTROLLER_BUTTON_MAX)
#define LOCK_R2 (SDL_CONTROLLER_BUTTON_MAX + 1)
#define LOCK_BIT(what) (1u << (unsigned) (what))

/* The overlay's chord (QUIT_BUTTONS in session_gamepad.c). */
#define LOCK_CHORD (LOCK_BIT(SDL_CONTROLLER_BUTTON_START) | LOCK_BIT(SDL_CONTROLLER_BUTTON_BACK) | \
                    LOCK_BIT(SDL_CONTROLLER_BUTTON_LEFTSHOULDER) | LOCK_BIT(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER))

typedef enum {
    LOCK_ACT_NONE = 0,
    LOCK_ACT_KEY,
    LOCK_ACT_KEYBOARD,
    LOCK_ACT_NUMPAD_OPEN,
    LOCK_ACT_NUMPAD_CLOSE,
} lock_act_t;

/* ⭐ THE ORDER OF THE PRESSES IS THE ORDER OF THE DIGITS. With the numpad open, three
 * of the chord's four buttons are also a 7, an 8 and Backspace, so they cannot act
 * until they are let go, or holding the chord would type. The rest act as they go
 * down. A press waits here behind an earlier one not decided yet, so 7 then 1 rolled
 * together types 71 and not 17. What a press does is settled as it goes down. */
typedef struct {
    uint8_t what;
    uint8_t act;
    short key;
    bool decided;
} lock_press_t;

typedef struct {
    SDL_JoystickID instance_id;
    uint32_t down;   /* what has gone down since this began and not come up */
    uint32_t options_since;  /* when Options went down alone, held since; 0 if not */
    bool chord;      /* the chord was made: nothing acts until it is let go */
    lock_press_t queue[LOCK_QUEUE];
    int queued;
} lock_pad_t;

static lock_pad_t s_pads[LOCK_SLOTS];
static lock_keys_mode_t s_mode;
static bool s_numpad_open;
static bool s_vmouse_was;
static bool s_interface_had_input;
static unsigned s_typed;
/* After a prompt, until when the bridged pads may stay held; 0 when they are not. */
static uint32_t s_quiet_until;
static int s_fake_prompt = -1;
/* Turned off by hand, and whether at a prompt (else at the lock). */
static bool s_dismissed;
static bool s_dismissed_prompt;
#if defined(TARGET_WEBOS)
static uint32_t s_last_ask;
static uint32_t s_last_press;
#endif

static bool mode_is_prompt(lock_keys_mode_t mode) {
    return mode == LOCK_KEYS_PROMPT || mode == LOCK_KEYS_PROMPT_PASSWORD;
}

bool lock_keys_active(void) {
    return s_mode != LOCK_KEYS_OFF;
}

lock_keys_mode_t lock_keys_mode(void) {
    return s_mode;
}

bool lock_keys_numpad_open(void) {
    return s_mode == LOCK_KEYS_LOCK && s_numpad_open;
}

bool lock_keys_triggers_type(void) {
    return lock_keys_numpad_open();
}

bool lock_keys_triggers_swapped(void) {
    return s_mode != LOCK_KEYS_OFF && !s_numpad_open;
}

unsigned lock_keys_typed(void) {
    return s_typed;
}

void lock_keys_fake_prompt(int kind) {
    s_fake_prompt = kind < 0 ? -1 : (kind > 2 ? 2 : kind);
}

int lock_keys_faked_prompt(void) {
    return s_fake_prompt;
}

void lock_keys_dismiss(const char *how) {
    if (s_mode == LOCK_KEYS_OFF || s_dismissed) {
        return;
    }
    s_dismissed = true;
    s_dismissed_prompt = mode_is_prompt(s_mode);
#if defined(TARGET_WEBOS)
    ctm_bridge_gesture_note("lock keys: turned off by hand (%s): every controller is a plain controller until "
                            "the next %s", how != NULL ? how : "?",
                            s_dismissed_prompt ? "User Account Control prompt or lock" : "lock or prompt");
#else
    (void) how;
#endif
}

bool lock_keys_dismissed(void) {
    return s_dismissed;
}

static bool pad_on_stream(const stream_input_t *input, const app_gamepad_state_t *gamepad) {
    return gamepad != NULL && gamepad->controller != NULL && gamepad->gs_id >= 0 && gamepad->gs_id < LOCK_SLOTS &&
           (input->moonlightExcludedMask & (1u << gamepad->gs_id)) == 0;
}

/* ⭐ WHICH CONTROLLERS THIS TAKES. At Windows' lock, those on the stream only: a
 * bridged one types the PIN with Windows' own map (the header). At a User Account
 * Control prompt, every one, bridged or not: a bridged pad's buttons may be bound to
 * keys on the PC that answer nothing there, so it is read here through SDL and its
 * reports to the PC are held at rest meanwhile (lock_keys_holds_bridged). */
static bool pad_taken(const stream_input_t *input, const app_gamepad_state_t *gamepad) {
    if (mode_is_prompt(s_mode)) {
        return gamepad != NULL && gamepad->controller != NULL && gamepad->gs_id >= 0 && gamepad->gs_id < LOCK_SLOTS;
    }
    return pad_on_stream(input, gamepad);
}

/* Is anything down on a bridged pad: a button, or a trigger past the point a
 * trigger lets go? Read off SDL, which reads a bridged pad as well. */
static bool bridged_pad_held(const stream_input_t *input, const app_gamepad_state_t *gamepad) {
    if (gamepad == NULL || gamepad->controller == NULL || gamepad->gs_id < 0 || gamepad->gs_id >= LOCK_SLOTS ||
        (input->moonlightExcludedMask & (1u << gamepad->gs_id)) == 0) {
        return false;
    }
    for (int b = 0; b < SDL_CONTROLLER_BUTTON_MAX; ++b) {
        if (SDL_GameControllerGetButton(gamepad->controller, (SDL_GameControllerButton) b)) {
            return true;
        }
    }
    return SDL_GameControllerGetAxis(gamepad->controller, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > LOCK_TRIGGER_UP ||
           SDL_GameControllerGetAxis(gamepad->controller, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > LOCK_TRIGGER_UP;
}

/* ⭐ HELD AFTER THE PROMPT UNTIL LET GO. The Enter that answered it is often still
 * down as the prompt goes, and the hold's end hands the pad's real state to the PC
 * at once: it would press Cross in the game. So the hold stays until nothing is down
 * on any bridged pad, at most LOCK_QUIET_MS. */
bool lock_keys_holds_bridged(void) {
    if (mode_is_prompt(s_mode)) {
        return true;
    }
    if (s_quiet_until == 0) {
        return false;
    }
    bool held = false;
    if ((int32_t) (SDL_GetTicks() - s_quiet_until) < 0 && global != NULL && global->session != NULL) {
        const stream_input_t *input = session_get_input(global->session);
        const short max = input != NULL ? app_input_get_max_gamepads(&global->input) : 0;
        for (short i = 0; i < max && !held; ++i) {
            held = bridged_pad_held(input, app_input_gamepad_state_by_index(&global->input, i));
        }
    }
    if (!held) {
        s_quiet_until = 0;
    }
    return held;
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
        if (pad_taken(input, app_input_gamepad_state_by_index(&global->input, i))) {
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
    pad->options_since = 0;
    pad->chord = false;
    pad->queued = 0;
    if (SDL_GameControllerGetAxis(gamepad->controller, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > LOCK_TRIGGER_UP) {
        pad->down |= LOCK_BIT(LOCK_L2);
    }
    if (SDL_GameControllerGetAxis(gamepad->controller, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > LOCK_TRIGGER_UP) {
        pad->down |= LOCK_BIT(LOCK_R2);
    }
}

static void pads_forget(stream_input_t *input) {
    const short max = app_input_get_max_gamepads(input->input);
    for (short i = 0; i < max; ++i) {
        app_gamepad_state_t *gamepad = app_input_gamepad_state_by_index(input->input, i);
        if (pad_taken(input, gamepad)) {
            pad_forget(pad_for(gamepad), gamepad);
        }
    }
}

/* What a press does, in the state it went down in. */
static lock_act_t action_for(uint8_t what, bool numpad_open, short *key) {
    *key = 0;
    if (numpad_open) {
        switch (what) {
            case SDL_CONTROLLER_BUTTON_DPAD_UP:
                *key = VK_1;
                break;
            case SDL_CONTROLLER_BUTTON_DPAD_LEFT:
                *key = VK_2;
                break;
            case SDL_CONTROLLER_BUTTON_DPAD_DOWN:
                *key = VK_3;
                break;
            case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:
                *key = VK_4;
                break;
            case LOCK_L2:
                *key = VK_5;
                break;
            case LOCK_R2:
                *key = VK_6;
                break;
            case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:
                *key = VK_7;
                break;
            case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER:
                *key = VK_8;
                break;
            case SDL_CONTROLLER_BUTTON_Y:
                *key = VK_9;
                break;
            case SDL_CONTROLLER_BUTTON_X:
                *key = VK_0;
                break;
            case SDL_CONTROLLER_BUTTON_A:
                *key = VK_RETURN;
                break;
            case SDL_CONTROLLER_BUTTON_BACK:
                *key = VK_BACK;
                break;
            case SDL_CONTROLLER_BUTTON_B:
                return LOCK_ACT_NUMPAD_CLOSE;
            default:
                return LOCK_ACT_NONE;
        }
        return LOCK_ACT_KEY;
    }
    switch (what) {
        case SDL_CONTROLLER_BUTTON_DPAD_UP:
            *key = VK_UP;
            break;
        case SDL_CONTROLLER_BUTTON_DPAD_LEFT:
            *key = VK_LEFT;
            break;
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN:
            *key = VK_DOWN;
            break;
        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:
            *key = VK_RIGHT;
            break;
        case SDL_CONTROLLER_BUTTON_A:
            *key = VK_RETURN;
            break;
        case SDL_CONTROLLER_BUTTON_B:
            *key = VK_ESCAPE;
            break;
        case SDL_CONTROLLER_BUTTON_BACK:
            /* Tab (rhoquinn8217, 2026-11-10: "create create button binded to tab"), at
             * the lock and at every prompt. ⓘ Create is one of the overlay's chord, so it
             * acts as it goes up (pad_press), and holding the chord sends no Tab. */
            *key = VK_TAB;
            break;
        case SDL_CONTROLLER_BUTTON_X:
            /* ⭐ The keyboard at every prompt as well as at the lock (rhoquinn8217,
             * 2026-11-10: "lets just add square to all UAC prompts. there's no downside
             * of having it"). Until then only where a prompt could ask for a password or
             * a PIN. */
            return LOCK_ACT_KEYBOARD;
        case SDL_CONTROLLER_BUTTON_Y:
            /* And no numpad at a prompt. */
            return s_mode == LOCK_KEYS_LOCK ? LOCK_ACT_NUMPAD_OPEN : LOCK_ACT_NONE;
        default:
            return LOCK_ACT_NONE;
    }
    return LOCK_ACT_KEY;
}

static void numpad_set(stream_input_t *input, bool open) {
    s_numpad_open = open;
    /* What is held now was held for the other state. */
    pads_forget(input);
    if (open) {
        /* A click held goes up: the triggers type now. */
        vmouse_set_trigger(&input->vmouse, 0, 0);
    }
}

static void act(stream_input_t *input, const lock_press_t *press) {
    switch (press->act) {
        case LOCK_ACT_KEY:
            stream_input_send_key_event(input, press->key, true, 0);
            stream_input_send_key_event(input, press->key, false, 0);
            s_typed++;
            break;
        case LOCK_ACT_KEYBOARD:
            bus_pushevent(USER_OPEN_SOFT_KEYBOARD, NULL, NULL);
            break;
        case LOCK_ACT_NUMPAD_OPEN:
            numpad_set(input, true);
            break;
        case LOCK_ACT_NUMPAD_CLOSE:
            numpad_set(input, false);
            break;
        default:
            break;
    }
}

/* What is decided, from the front, up to the first that is not. ⓘ One at a time and
 * off the queue first: a press that changes the state empties it. */
static void queue_run(stream_input_t *input, lock_pad_t *pad) {
    while (pad->queued > 0 && pad->queue[0].decided) {
        const lock_press_t press = pad->queue[0];
        pad->queued--;
        memmove(pad->queue, pad->queue + 1, (size_t) pad->queued * sizeof pad->queue[0]);
        act(input, &press);
    }
}

static void pad_press(stream_input_t *input, lock_pad_t *pad, uint8_t what) {
    if (pad->down & LOCK_BIT(what)) {
        return;
    }
    pad->down |= LOCK_BIT(what);
    /* Options alone starts the hold that turns this off; anything with it ends it,
     * the chord to the overlay included. */
    if (what == SDL_CONTROLLER_BUTTON_START && pad->down == LOCK_BIT(what)) {
        pad->options_since = SDL_GetTicks() | 1u;
    } else {
        pad->options_since = 0;
    }
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
    short key;
    const lock_act_t todo = action_for(what, s_numpad_open, &key);
    if (pad->chord || todo == LOCK_ACT_NONE || pad->queued == LOCK_QUEUE) {
        return;
    }
    lock_press_t *press = &pad->queue[pad->queued++];
    press->what = what;
    press->act = (uint8_t) todo;
    press->key = key;
    press->decided = (LOCK_BIT(what) & LOCK_CHORD) == 0;
    queue_run(input, pad);
}

static void pad_release(stream_input_t *input, lock_pad_t *pad, uint8_t what) {
    /* ⓘ Not down as far as this knows: it went down before this began, or while the
     * overlay or the keyboard had the input. */
    if ((pad->down & LOCK_BIT(what)) == 0) {
        return;
    }
    pad->down &= ~LOCK_BIT(what);
    if (what == SDL_CONTROLLER_BUTTON_START) {
        pad->options_since = 0;
    }
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
        if (s_mode == LOCK_KEYS_OFF && now - s_last_press >= LOCK_IDLE_MS && ctm_bridge_active()) {
            ctm_bridge_agent_recheck();
        }
        s_last_press = now;
#endif
    }
    /* ⓘ The touchpad's click stays the stream's: as the mouse, it clicks. */
    if (s_mode == LOCK_KEYS_OFF || !pad_taken(input, gamepad) || event->button == SDL_CONTROLLER_BUTTON_TOUCHPAD) {
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
    if (s_mode == LOCK_KEYS_OFF || !pad_taken(input, gamepad)) {
        return;
    }
    uint8_t what;
    if (event->axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT) {
        what = LOCK_L2;
    } else if (event->axis == SDL_CONTROLLER_AXIS_TRIGGERRIGHT) {
        what = LOCK_R2;
    } else {
        /* ⛔ Asked here as well as by the stream's handling, which asks only while a
         * stick or trigger is off rest: a stick dropping straight to rest would leave
         * the mouse moving. */
        vmouse_set_vector(&input->vmouse, gamepad->rightStickX, gamepad->rightStickY);
        vmouse_set_scroll(&input->vmouse, gamepad->leftStickX, gamepad->leftStickY);
        return;
    }
    if (!s_numpad_open) {
        /* ⛔ And a trigger dropping straight to rest would leave the mouse's button
         * down on the host. ⓘ Its first is the left button: R2's, at a lock. */
        vmouse_set_trigger(&input->vmouse, gamepad->rightTrigger, gamepad->leftTrigger);
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

static void lock_begin(stream_input_t *input, lock_keys_mode_t mode) {
    s_mode = mode;
    s_numpad_open = false;
    s_interface_had_input = false;
    s_quiet_until = 0;
    memset(s_pads, 0, sizeof s_pads);
    s_vmouse_was = session_input_is_vmouse_active(&input->vmouse);
    session_input_set_vmouse_active(&input->vmouse, true);
    const short max = app_input_get_max_gamepads(input->input);
    for (short i = 0; i < max; ++i) {
        app_gamepad_state_t *gamepad = app_input_gamepad_state_by_index(input->input, i);
        if (!pad_taken(input, gamepad)) {
            continue;
        }
        pad_forget(pad_for(gamepad), gamepad);
        /* ⓘ A bridged pad is not the stream's to rest: its reports are held instead. */
        if (!pad_on_stream(input, gamepad)) {
            continue;
        }
        gamepad->buttons = 0;
        LiSendMultiControllerEvent(gamepad->gs_id, input->input->activeGamepadMask, 0, 0, 0, 0, 0, 0, 0);
    }
#if defined(TARGET_WEBOS)
    ctm_bridge_gesture_note(mode == LOCK_KEYS_LOCK
                                ? "lock keys: Windows is locked, a controller on the stream is a mouse, or types"
                                : mode == LOCK_KEYS_PROMPT
                                ? "lock keys: a User Account Control prompt, Yes or No: every controller is a mouse, "
                                  "or keys, the bridged ones held from the PC"
                                : "lock keys: a User Account Control prompt that can ask for a password or PIN: every "
                                  "controller is a mouse, or keys, Square the keyboard, the bridged ones held from the PC");
#endif
}

static void lock_end(stream_input_t *input) {
    const bool prompt = mode_is_prompt(s_mode);
    s_mode = LOCK_KEYS_OFF;
    s_numpad_open = false;
    memset(s_pads, 0, sizeof s_pads);
    /* The bridged pads stay held until what is down on them is let go. */
    s_quiet_until = prompt ? SDL_GetTicks() + LOCK_QUIET_MS : 0;
#if defined(TARGET_WEBOS)
    ctm_bridge_gesture_note(prompt ? "lock keys: the prompt is gone, the controllers are the stream's and the bridge's again"
                                   : "lock keys: Windows is unlocked, the controllers are the stream's again");
#endif
    if (input == NULL) {
        return;
    }
    /* ⛔ Before anything can ask whether the stream is up: the setting follows Virtual
     * Mouse (bridge_override_touchpad_tick), and would keep it on. Letting it go also
     * lets go a mouse button a trigger held. */
    session_input_set_vmouse_active(&input->vmouse, s_vmouse_was);
    if (!keyboard_input_available(input)) {
        return;
    }
    const short max = app_input_get_max_gamepads(input->input);
    for (short i = 0; i < max; ++i) {
        app_gamepad_state_t *gamepad = app_input_gamepad_state_by_index(input->input, i);
        if (!pad_on_stream(input, gamepad)) {
            continue;
        }
        /* What is held now stays out of the stream until it is let go: the Enter that
         * unlocked must not press anything in a game. */
        gamepad->buttons = 0;
        bridge_switch_hold_keep_quiet(input, gamepad);
        bridge_switch_hold_send(gamepad, input->input->activeGamepadMask, false);
    }
}

void lock_keys_tick(app_t *app) {
    stream_input_t *input = app->session != NULL ? session_get_input(app->session) : NULL;
    lock_keys_mode_t wanted = LOCK_KEYS_OFF;
#if defined(TARGET_WEBOS)
    if (input != NULL && keyboard_input_available(input) && ctm_bridge_active()) {
        if (ctm_bridge_agent_locked()) {
            wanted = LOCK_KEYS_LOCK;
        } else {
            const int kind = s_fake_prompt >= 0 ? s_fake_prompt : (int) ctm_bridge_agent_uac();
            wanted = kind == CTM_BRIDGE_UAC_CONSENT       ? LOCK_KEYS_PROMPT
                     : kind == CTM_BRIDGE_UAC_CREDENTIALS ? LOCK_KEYS_PROMPT_PASSWORD
                                                          : LOCK_KEYS_OFF;
        }
    }
#endif
    /* ⭐ EACH HAS ITS SWITCH (bridge_settings.h: lock_rebinds, uac_rebinds), both on
     * by default: switched off, the controllers stay plain controllers then. */
    if (app_configuration != NULL &&
        ((wanted == LOCK_KEYS_LOCK && !app_configuration->bridge.lock_rebinds) ||
         (mode_is_prompt(wanted) && !app_configuration->bridge.uac_rebinds))) {
        wanted = LOCK_KEYS_OFF;
    }
    /* ⭐ OFF BY HAND UNTIL THE NEXT ONE (lock_keys_dismiss): until DS5-USBIP says
     * neither a lock nor a prompt, or says the other of the two. */
    if (wanted == LOCK_KEYS_OFF || (s_dismissed && mode_is_prompt(wanted) != s_dismissed_prompt)) {
        s_dismissed = false;
    }
    if (s_dismissed) {
        wanted = LOCK_KEYS_OFF;
    }
    if (wanted != s_mode) {
        if (mode_is_prompt(wanted) && mode_is_prompt(s_mode)) {
            /* The prompt's kind said again, otherwise: only what Square does changes. */
            s_mode = wanted;
        } else {
            if (s_mode != LOCK_KEYS_OFF) {
                lock_end(input);
            }
            if (wanted != LOCK_KEYS_OFF) {
                lock_begin(input, wanted);
            }
        }
    }
    if (s_mode == LOCK_KEYS_OFF) {
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
     * here. So everything is forgotten as they take it, and as they give it back. */
    const bool interface_has_input = ui_should_block_input();
    if (interface_has_input != s_interface_had_input) {
        s_interface_had_input = interface_has_input;
        pads_forget(input);
        /* Nor does the mouse go on moving, or stay down, behind them. */
        vmouse_set_vector(&input->vmouse, 0, 0);
        vmouse_set_scroll(&input->vmouse, 0, 0);
        vmouse_set_trigger(&input->vmouse, 0, 0);
    }
    /* Options held LOCK_DISMISS_MS alone, on any pad this takes: off by hand. Ended
     * on the next pass, as an unlock is. */
    const uint32_t held_at = SDL_GetTicks();
    for (int i = 0; i < LOCK_SLOTS; ++i) {
        if (s_pads[i].options_since != 0 && held_at - s_pads[i].options_since >= LOCK_DISMISS_MS) {
            s_pads[i].options_since = 0;
            lock_keys_dismiss("Options held 4 s");
            break;
        }
    }
}
