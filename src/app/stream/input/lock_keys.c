/* A controller at Windows' lock screen. The why is in the header. */

#include "lock_keys.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <Limelight.h>

#include "app.h"
#include "input/app_input.h"
#include "input/input_gamepad.h"
#include "input/touchpad_pointer.h"
#include "input/tv_cursor.h"
#include "stream/input/bridge_switch_hold.h"
#include "stream/input/bridged_touchpad.h"
#include "stream/input/keyboard_input.h"
#include "stream/input/session_input.h"
#include "stream/input/session_virt_mouse.h"
#include "stream/input/vk.h"
#include "stream/input/window_select.h"
#include "stream/session.h"
#include "ui/common/pad_marks.h"
#include "ui/root.h"
#include "ui/streaming/desktop_mode.h"
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
/* Options held this long with nothing else asks to turn this off (rhoquinn8217,
 * 2026-11-10: "change the 4 seconds to 2 seconds"). Until then 4 s, as long as Select
 * is held to pin the stats (GAMEPAD_HOLD_STATS_MS). */
#define LOCK_DISMISS_MS 2000

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
    LOCK_ACT_ANSWER_OK,
    LOCK_ACT_ANSWER_CANCEL,
    LOCK_ACT_WINDOW_BACK,   /* L1: Window Select, back */
    LOCK_ACT_WINDOW_NEXT,   /* R1: Window Select, forward */
    LOCK_ACT_LEGEND,        /* an Xbox pad's Share button: the list over the screen, or hidden */
    LOCK_ACT_CURSOR,        /* L3: the TV cursor on, with its alignment, or off */
    LOCK_ACT_LET_GO,        /* any other button: Window Select's Alt let go */
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
    uint32_t two_since;  /* a two-finger press on its touchpad began then; 0 if not */
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
/* ⭐ THE LIST OF WHAT THE BUTTONS DO, over the screen unless hidden: by its Hide or by
 * the PS button, which brings it back too (rhoquinn8217, 2026-11-12: "close need to
 * change to hide", "PS button sould be the legend button"). Over the screen again as
 * each lock, prompt and Desktop Mode begins ("Over the screen like window lock and
 * UAC"). */
static bool s_list_hidden;
/* Turned off by hand, and whether at a prompt (else at the lock). */
static bool s_dismissed;
static bool s_dismissed_prompt;
/* A close's question waiting for its answer, and how it was asked. */
static bool s_asking;
static const char *s_asking_how;
/* R2 down while the question waits, and whether its press went to Aurora's button. */
static bool s_r2_down;
static bool s_r2_auroras;
/* A trigger's click is down on the host, so its release lets go of Window Select's Alt. */
static bool s_trigger_clicked;
#if defined(TARGET_WEBOS)
static uint32_t s_last_ask;
static uint32_t s_last_press;
#endif

static bool mode_is_prompt(lock_keys_mode_t mode) {
    return mode == LOCK_KEYS_PROMPT || mode == LOCK_KEYS_PROMPT_PASSWORD;
}

/* Every controller, bridged ones too, their reports held from the PC: at a prompt, and
 * in Desktop Mode. */
/* Every controller, bridged ones too: at a prompt, in Desktop Mode, and since
 * 2026-11-11 at the lock as well (pad_taken says why). */
static bool mode_takes_all(lock_keys_mode_t mode) {
    return mode == LOCK_KEYS_LOCK || mode_is_prompt(mode) || mode == LOCK_KEYS_DESKTOP;
}

static bool desktop_mode_wanted(void) {
    return desktop_mode_on();
}

bool lock_keys_active(void) {
    return s_mode != LOCK_KEYS_OFF;
}

lock_keys_mode_t lock_keys_mode(void) {
    return s_mode;
}

bool lock_keys_desktop_wanted(void) {
    return desktop_mode_wanted();
}

bool lock_keys_numpad_open(void) {
    return s_mode != LOCK_KEYS_OFF && s_numpad_open;
}

bool lock_keys_list_hidden(void) {
    return s_list_hidden;
}

static void list_note(const char *how) {
#if defined(TARGET_WEBOS)
    ctm_bridge_gesture_note("lock keys: the list %s (%s)", s_list_hidden ? "hidden" : "over the screen",
                            how != NULL ? how : "?");
#else
    (void) how;
#endif
}

void lock_keys_list_toggle(const char *how) {
    if (s_mode == LOCK_KEYS_OFF) {
        return;
    }
    s_list_hidden = !s_list_hidden;
    list_note(how);
}

void lock_keys_list_hide(const char *how) {
    if (s_mode == LOCK_KEYS_OFF || s_list_hidden) {
        return;
    }
    s_list_hidden = true;
    list_note(how);
}

bool lock_keys_triggers_type(void) {
    return lock_keys_numpad_open() || (s_asking && s_mode != LOCK_KEYS_OFF);
}

bool lock_keys_triggers_swapped(void) {
    return s_mode != LOCK_KEYS_OFF && !s_numpad_open;
}

bool lock_keys_stick_moves_tv_cursor(void) {
    return s_mode != LOCK_KEYS_OFF && tv_cursor_mode_on();
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

/* R2's press while the question waited, let go: Aurora's pointer is not left
 * pressed when the question goes another way. */
static void r2_let_go(void) {
    s_r2_down = false;
    if (s_r2_auroras) {
        touchpad_pointer_stream_press(&s_r2_auroras, true, false);
    }
}

void lock_keys_dismiss(const char *how) {
    /* ⓘ Not Desktop Mode: its question's OK turns Desktop Mode itself off
     * (lock_keys_answer). */
    if (s_mode == LOCK_KEYS_OFF || s_mode == LOCK_KEYS_DESKTOP || s_dismissed) {
        return;
    }
    s_dismissed = true;
    s_dismissed_prompt = mode_is_prompt(s_mode);
    s_asking = false;
    r2_let_go();
#if defined(TARGET_WEBOS)
    ctm_bridge_gesture_note("lock keys: turned off by hand (%s): %s until the next %s", how != NULL ? how : "?",
                            desktop_mode_wanted() ? "Desktop Mode again" : "every controller is a plain controller",
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

/* ⭐ WHICH CONTROLLERS THIS TAKES: every one, bridged or not, read here through SDL,
 * a bridged one's reports to the PC held at rest meanwhile (lock_keys_holds_bridged).
 * At a User Account Control prompt, as a bridged pad's buttons may be bound to keys on
 * the PC that answer nothing there; in Desktop Mode, over its config on the PC (the
 * header); and ⭐ at Windows' lock since 2026-11-11 (rhoquinn8217, a bridged DualSense
 * at a lock: "triangle is opening aurora-tv keyboard when it should be the numpad.
 * Square does notthing when it should be the full keyboard"). Until then the lock took
 * the stream's pads only and left a bridged one to type the PIN with Windows' own map,
 * which a lock reached by a new stream ignores; and the list showed controls its
 * buttons did not have. */
static bool pad_taken(const stream_input_t *input, const app_gamepad_state_t *gamepad) {
    if (mode_takes_all(s_mode)) {
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
    if (mode_takes_all(s_mode)) {
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

/* The question has the pads now, or has just given them back: what is down on them
 * is forgotten, and a click the triggers hold is let go. */
static void pads_rest(void) {
    stream_input_t *input = global != NULL && global->session != NULL ? session_get_input(global->session) : NULL;
    if (input == NULL) {
        return;
    }
    pads_forget(input);
    vmouse_set_trigger(&input->vmouse, 0, 0);
}

void lock_keys_ask_dismiss(const char *how) {
    if (s_mode == LOCK_KEYS_OFF || s_dismissed || s_asking) {
        return;
    }
    s_asking = true;
    s_asking_how = how != NULL ? how : "?";
    pads_rest();
#if defined(TARGET_WEBOS)
    ctm_bridge_gesture_note("lock keys: %s: asking whether to turn the rebinds off", s_asking_how);
#endif
}

bool lock_keys_asking(void) {
    return s_asking && s_mode != LOCK_KEYS_OFF;
}

void lock_keys_answer(bool ok, const char *by) {
    if (!lock_keys_asking()) {
        return;
    }
    s_asking = false;
    r2_let_go();
    pads_rest();
    char how[128];
    snprintf(how, sizeof how, "%s, then %s %s", s_asking_how, ok ? "OK" : "Cancel", by != NULL ? by : "?");
    if (ok) {
        /* ⭐ Desktop Mode asks too since 2026-11-12 (rhoquinn8217: "keep the question"),
         * and its OK turns Desktop Mode off: this ends on the next pass. */
        if (s_mode == LOCK_KEYS_DESKTOP) {
            desktop_mode_set(false, how);
            return;
        }
        lock_keys_dismiss(how);
        return;
    }
#if defined(TARGET_WEBOS)
    ctm_bridge_gesture_note("lock keys: kept on (%s)", how);
#endif
}

/* What a press does, in the state it went down in. */
static lock_act_t action_for(uint8_t what, bool numpad_open, short *key) {
    *key = 0;
    if (s_asking) {
        /* ⭐ A close's question has the pad: Cross is its OK and Circle its Cancel (A
         * and B on an Xbox pad), and nothing else acts. */
        return what == SDL_CONTROLLER_BUTTON_A   ? LOCK_ACT_ANSWER_OK
               : what == SDL_CONTROLLER_BUTTON_B ? LOCK_ACT_ANSWER_CANCEL
                                                 : LOCK_ACT_NONE;
    }
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
            case SDL_CONTROLLER_BUTTON_MISC1:
                /* An Xbox pad's Share button hides the numpad's list as well; its keys
                 * still type. */
                return pad_family_current() == PAD_FAMILY_XBOX ? LOCK_ACT_LEGEND : LOCK_ACT_NONE;
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
            /* ⭐ THE SAME AT THE LOCK, AT A PROMPT AND IN DESKTOP MODE since 2026-11-12
             * (rhoquinn8217: "Lets consolidate windows lock, UAC and Desktop mode. It
             * doesn't make sense to make them different."): Triangle opens the numpad
             * ("Triangle should always open the numpad"). Until then the numpad was the
             * lock's alone, and Triangle the TV cursor in Desktop Mode. */
            return LOCK_ACT_NUMPAD_OPEN;
        case SDL_CONTROLLER_BUTTON_LEFTSTICK:
            /* L3 the TV cursor: off, or on with its alignment bar up ("Lets move the
             * cursor->alignment button to L3"; ui/streaming/desktop_mode.h). */
            return LOCK_ACT_CURSOR;
        case SDL_CONTROLLER_BUTTON_MISC1:
            /* ⭐ An Xbox pad's Share button the list, over the screen or hidden
             * (rhoquinn8217, 2026-11-13: "xbox share button in brings up the legend";
             * lock_keys_list_toggle); a DualSense's is its two-finger press
             * (two_press_tick). A DualSense's MISC1 is its microphone button: nothing, as
             * before. ⓘ PS and the Xbox button go to Windows (lock_keys_button); the PS
             * button was the list's until then. */
            return pad_family_current() == PAD_FAMILY_XBOX ? LOCK_ACT_LEGEND : LOCK_ACT_LET_GO;
        case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:
        case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER:
            /* ⭐ Window Select: L1 back and R1 forward through Windows' window picker
             * (stream/input/window_select.h), Desktop Mode's alone until 2026-11-12. ⓘ
             * Both are of the overlay's chord, so they act as they go up (pad_press). */
            return what == SDL_CONTROLLER_BUTTON_LEFTSHOULDER ? LOCK_ACT_WINDOW_BACK : LOCK_ACT_WINDOW_NEXT;
        default:
            /* Any other button still lets go of Window Select's Alt ("any other button"),
             * Options as well: its tap does nothing else, and its hold asks
             * (lock_keys_tick). */
            return LOCK_ACT_LET_GO;
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
    /* ⭐ WINDOW SELECT'S ALT (stream/input/window_select.h), settled as the press acts and
     * not as it went down, since a press can wait behind L1 or R1: Cross picks the window
     * and Circle cancels the picker, neither then doing its own job; the d-pad and Tab
     * step on with Alt held; anything else lets Alt go first, then does its job. */
    if (window_select_held()) {
        if (press->act == LOCK_ACT_KEY && press->key == VK_RETURN) {
            window_select_let_go(input, "Cross: the window picked");
            return;
        }
        if (press->act == LOCK_ACT_KEY && press->key == VK_ESCAPE) {
            window_select_cancel(input);
            return;
        }
        const bool keeps = press->act == LOCK_ACT_WINDOW_BACK || press->act == LOCK_ACT_WINDOW_NEXT ||
                           (press->act == LOCK_ACT_KEY && window_select_keeps(press->key));
        if (!keeps) {
            window_select_let_go(input, "another button");
        }
    }
    switch (press->act) {
        case LOCK_ACT_KEY: {
            /* Said with Alt while it is held: an arrow or Tab through the picker. */
            const char mods = window_select_held() ? (char) MODIFIER_ALT : 0;
            stream_input_send_key_event(input, press->key, true, mods);
            stream_input_send_key_event(input, press->key, false, mods);
            s_typed++;
            break;
        }
        case LOCK_ACT_WINDOW_BACK:
            window_select_step(input, true);
            break;
        case LOCK_ACT_WINDOW_NEXT:
            window_select_step(input, false);
            break;
        case LOCK_ACT_LEGEND:
            lock_keys_list_toggle("PS");
            break;
        case LOCK_ACT_CURSOR:
            desktop_mode_cursor_toggle();
            break;
        case LOCK_ACT_LET_GO:
            /* Window Select's Alt, above: nothing more. */
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
        case LOCK_ACT_ANSWER_OK:
            lock_keys_answer(true, "on a pad");
            break;
        case LOCK_ACT_ANSWER_CANCEL:
            lock_keys_answer(false, "on a pad");
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
    /* ⭐ PS AND THE XBOX BUTTON GO TO WINDOWS (rhoquinn8217, 2026-11-13: "PS button should
     * go to windows(start steam) / Xbox button should go to windows(start steam)"), as
     * they do outside these controls ("I'm pressing it outside of Desktop Mode and it
     * brings up steam"): a pad on the stream presses the host's own, with nothing else
     * pressed; a bridged one's goes by the bridge, which keeps it through the hold
     * (ctm_bridge_set_home_passes in input/ctm_bridge_gesture.c). ⓘ A bridged Xbox pad's
     * cannot: its report has no byte for it on a cable, and DS5-USBIP's Bluetooth map
     * does not carry it. */
    if (event->button == SDL_CONTROLLER_BUTTON_GUIDE) {
        if (down) {
            window_select_let_go(input, "PS");
        }
        if (pad_on_stream(input, gamepad)) {
            LiSendMultiControllerEvent(gamepad->gs_id, input->input->activeGamepadMask, down ? SPECIAL_FLAG : 0, 0,
                                       0, 0, 0, 0, 0);
        }
        return true;
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
         * the mouse moving. ⓘ Not the right stick while it moves the TV cursor
         * (stick_moves_tv_cursor): Virtual Mouse would move the host's pointer away
         * from it. */
        if (lock_keys_stick_moves_tv_cursor()) {
            vmouse_set_vector(&input->vmouse, 0, 0);
        } else {
            vmouse_set_vector(&input->vmouse, gamepad->rightStickX, gamepad->rightStickY);
        }
        vmouse_set_scroll(&input->vmouse, gamepad->leftStickX, gamepad->leftStickY);
        return;
    }
    if (s_asking) {
        /* ⭐ A close's question has the pad: the triggers click nothing of the host's,
         * and R2 clicks only what of Aurora's the TV cursor is on, the question's OK or
         * Cancel (input/touchpad_pointer.h), as the touchpad does. */
        if (what == LOCK_R2) {
            if (!s_r2_down && event->value > LOCK_TRIGGER_DOWN) {
                s_r2_down = true;
                touchpad_pointer_stream_press(&s_r2_auroras, true, true);
            } else if (s_r2_down && event->value < LOCK_TRIGGER_UP) {
                r2_let_go();
            }
        }
        vmouse_set_trigger(&input->vmouse, 0, 0);
        return;
    }
    if (!s_numpad_open) {
        /* ⛔ And a trigger dropping straight to rest would leave the mouse's button
         * down on the host. ⓘ Its first is the left button: R2's, at a lock; on
         * Aurora's own button it is Aurora's (stream/input/session_virt_mouse.c). */
        vmouse_set_trigger(&input->vmouse, gamepad->rightTrigger, gamepad->leftTrigger);
        /* ⭐ Window Select: a click goes through first, and Alt is let go after it
         * (rhoquinn8217, 2026-11-11: "3. click first"; stream/input/window_select.h). */
        if (input->vmouse.state.l || input->vmouse.state.r) {
            s_trigger_clicked = true;
        } else if (s_trigger_clicked) {
            s_trigger_clicked = false;
            window_select_after_click();
        }
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
    /* Its list over the screen again (s_list_hidden). */
    s_list_hidden = false;
    s_asking = false;
    r2_let_go();
    s_interface_had_input = false;
    s_quiet_until = 0;
    memset(s_pads, 0, sizeof s_pads);
    /* ⓘ Desktop Mode gives Virtual Mouse back OFF, whatever it was: turning it on
     * turns Virtual Mouse off (the header). */
    s_vmouse_was = mode == LOCK_KEYS_DESKTOP ? false : session_input_is_vmouse_active(&input->vmouse);
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
                                ? "lock keys: Windows is locked: every controller is a mouse, or types, the bridged "
                                  "ones held from the PC"
                                : mode == LOCK_KEYS_PROMPT
                                ? "lock keys: a User Account Control prompt, Yes or No: every controller is a mouse, "
                                  "or keys, the bridged ones held from the PC"
                                : mode == LOCK_KEYS_DESKTOP
                                ? "lock keys: Desktop Mode: every controller is a mouse, or keys, Square the keyboard, "
                                  "the bridged ones held from the PC"
                                : "lock keys: a User Account Control prompt that can ask for a password or PIN: every "
                                  "controller is a mouse, or keys, Square the keyboard, the bridged ones held from the PC");
#endif
}

static void lock_end(stream_input_t *input) {
    const lock_keys_mode_t was = s_mode;
    const bool held = mode_takes_all(was);
    /* Window Select's Alt goes up with it (stream/input/window_select.h). */
    window_select_let_go(input, was == LOCK_KEYS_DESKTOP ? "Desktop Mode ended" : "the rebinds ended");
    s_trigger_clicked = false;
    s_mode = LOCK_KEYS_OFF;
    s_numpad_open = false;
    s_asking = false;
    r2_let_go();
    memset(s_pads, 0, sizeof s_pads);
    /* The bridged pads stay held until what is down on them is let go. */
    s_quiet_until = held ? SDL_GetTicks() + LOCK_QUIET_MS : 0;
#if defined(TARGET_WEBOS)
    ctm_bridge_gesture_note(was == LOCK_KEYS_DESKTOP ? "lock keys: Desktop Mode ends, the controllers are the stream's "
                                                       "and the bridge's again"
                            : was == LOCK_KEYS_LOCK ? "lock keys: Windows is unlocked, the controllers are the stream's "
                                                      "and the bridge's again"
                                                    : "lock keys: the prompt is gone, the controllers are the stream's and "
                                                      "the bridge's again");
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

/* How fast the right stick moves the TV cursor, in the interface's pixels a
 * millisecond: Virtual Mouse's own curve (stream/input/session_virt_mouse.c), no
 * movement under 4096 and the square root of the rest, at its pace of a step of
 * that over 28 every 5 ms. Pushed all the way, about 1.2 pixels a millisecond. */
static float stick_speed(short axis) {
    const int a = axis > 0 ? axis : -axis;
    if (a < 4096) {
        return 0;
    }
    const float v = (float) (SDL_sqrt((double) (a - 4096)) / 140.0);
    return axis > 0 ? v : -v;
}

/* ⭐ THE RIGHT STICK MOVES THE TV CURSOR (rhoquinn8217, 2026-11-10: "during windows
 * log in and UAC, right stick to mouse isn't using aurora-tv mouse and can't click
 * the close button"), with the TV cursor on, as the touchpad does: so R2, the left
 * click here, reaches the list's Close and its question (input/touchpad_pointer.h).
 * Once a pass, by how far the pad pushed furthest is pushed and how long the pass
 * took: a stick held still sends no events. */
static void stick_moves_tv_cursor(stream_input_t *input) {
    static Uint32 s_at;
    const Uint32 now = SDL_GetTicks();
    Uint32 dt = s_at != 0 ? now - s_at : 0;
    s_at = now;
    if (dt > 50) {
        dt = 50;   /* a long pass, or the first in a while: no leap */
    }
    if (input == NULL || dt == 0 || !lock_keys_stick_moves_tv_cursor() || ui_should_block_input()) {
        return;
    }
    float vx = 0, vy = 0;
    const short max = app_input_get_max_gamepads(input->input);
    for (short i = 0; i < max; ++i) {
        const app_gamepad_state_t *gamepad = app_input_gamepad_state_by_index(input->input, i);
        if (!pad_taken(input, gamepad)) {
            continue;
        }
        /* The stream's Y is up; the screen's is down. */
        const float x = stick_speed(gamepad->rightStickX);
        const float y = stick_speed((short) -SDL_max(gamepad->rightStickY, -32767));
        if (x * x + y * y > vx * vx + vy * vy) {
            vx = x;
            vy = y;
        }
    }
    if (vx != 0 || vy != 0) {
        touchpad_pointer_stream_stick(input, vx * (float) dt, vy * (float) dt);
    }
}

/* ⭐ A DUALSENSE'S TWO-FINGER PRESS SHOWS OR HIDES THE LIST (rhoquinn8217, 2026-11-13:
 * "DualSense two finger press in brings up the legend"): two fingers on the touchpad and
 * the pad pressed in, let go within LOCK_TWO_PRESS_MS. Longer is the bridge gesture's (a
 * second bridges). Read from SDL for a pad on the stream, and from the core for a
 * bridged one (ctm_bridge_held_touches), the only reading of its touchpad the TV has
 * while its input is held. ⓘ The stream's own handling makes no right click of it
 * meanwhile (stream/input/session_gamepad.c). */
#define LOCK_TWO_PRESS_MS 900

/* One touchpad this pass: `fingers` on it and whether it is pressed in. True when a
 * two-finger press was let go in time just now. */
static bool two_press_step(uint32_t *since, int fingers, bool click, uint32_t now) {
    if (click) {
        if (fingers >= 2 && *since == 0) {
            *since = now | 1u;
        }
        return false;
    }
    if (*since == 0) {
        return false;
    }
    const bool quick = now - *since < LOCK_TWO_PRESS_MS;
    *since = 0;
    return quick;
}

#if defined(TARGET_WEBOS)
/* A bridged pad's, kept by its node. */
static struct {
    char node[64];
    uint32_t since;
} s_bridged_two[LOCK_SLOTS];
#endif

static void two_press_tick(stream_input_t *input) {
    const uint32_t now = SDL_GetTicks();
    bool toggle = false;
    const short max = app_input_get_max_gamepads(input->input);
    for (short i = 0; i < max; ++i) {
        app_gamepad_state_t *gamepad = app_input_gamepad_state_by_index(input->input, i);
        if (!pad_taken(input, gamepad) || !pad_on_stream(input, gamepad) || gamepad->controller == NULL) {
            continue;
        }
        int fingers = 0;
        const int slots = SDL_GameControllerGetNumTouchpadFingers(gamepad->controller, 0);
        for (int f = 0; f < slots && f < 2; ++f) {
            Uint8 state = 0;
            float x = 0, y = 0, pressure = 0;
            if (SDL_GameControllerGetTouchpadFinger(gamepad->controller, 0, f, &state, &x, &y, &pressure) == 0 &&
                state) {
                fingers++;
            }
        }
        const bool click = SDL_GameControllerGetButton(gamepad->controller, SDL_CONTROLLER_BUTTON_TOUCHPAD) != 0;
        toggle = two_press_step(&pad_for(gamepad)->two_since, fingers, click, now) || toggle;
    }
#if defined(TARGET_WEBOS)
    ctm_bridge_touch_t touches[LOCK_SLOTS];
    const int n = ctm_bridge_held_touches(touches, LOCK_SLOTS);
    bool seen[LOCK_SLOTS] = {false};
    for (int t = 0; t < n; ++t) {
        if (!touches[t].reading) {
            continue;
        }
        int k = -1, unused = -1;
        for (int j = 0; j < LOCK_SLOTS; ++j) {
            if (s_bridged_two[j].node[0] != '\0' && strcmp(s_bridged_two[j].node, touches[t].node) == 0) {
                k = j;
                break;
            }
            if (unused < 0 && s_bridged_two[j].node[0] == '\0') {
                unused = j;
            }
        }
        if (k < 0) {
            if (unused < 0) {
                continue;
            }
            k = unused;
            snprintf(s_bridged_two[k].node, sizeof s_bridged_two[k].node, "%s", touches[t].node);
            s_bridged_two[k].since = 0;
        }
        seen[k] = true;
        toggle = two_press_step(&s_bridged_two[k].since, touches[t].fingers, touches[t].click, now) || toggle;
    }
    for (int j = 0; j < LOCK_SLOTS; ++j) {
        if (!seen[j]) {
            s_bridged_two[j].node[0] = '\0';
            s_bridged_two[j].since = 0;
        }
    }
#endif
    if (toggle) {
        window_select_let_go(input, "a two-finger press");
        lock_keys_list_toggle("a two-finger press");
    }
}

void lock_keys_tick(app_t *app) {
    window_select_tick();
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
    /* ⭐ DESKTOP MODE WHEN NEITHER IS UP (the header): under a lock or a prompt, and
     * back as each goes, or as its Close turns it off. ⓘ Nothing asked of DS5-USBIP. */
    if (wanted == LOCK_KEYS_OFF && desktop_mode_wanted() && input != NULL && keyboard_input_available(input)) {
        wanted = LOCK_KEYS_DESKTOP;
    }
    if (wanted != s_mode) {
        if (mode_takes_all(wanted) && mode_takes_all(s_mode)) {
            /* Every controller taken on both sides: the prompt's kind said again, a lock
             * or a prompt over Desktop Mode or over each other, or Desktop Mode back
             * after one. Only what the controls are up for changes, and nothing is
             * handed back to the PC between. */
            const lock_keys_mode_t was = s_mode;
            s_mode = wanted;
            if (!(mode_is_prompt(wanted) && mode_is_prompt(was))) {
                /* What is down was pressed for the other: its release does nothing. And
                 * Window Select's Alt goes up before a lock or a prompt has the controls;
                 * the numpad is the lock's alone. */
                window_select_let_go(input, "a lock or a User Account Control prompt over Desktop Mode");
                s_asking = false;
                s_numpad_open = false;
                r2_let_go();
                pads_forget(input);
                if (wanted == LOCK_KEYS_DESKTOP) {
                    s_vmouse_was = false;
                }
#if defined(TARGET_WEBOS)
                ctm_bridge_gesture_note(wanted == LOCK_KEYS_DESKTOP ? "lock keys: the %s is gone, Desktop Mode again"
                                        : wanted == LOCK_KEYS_LOCK  ? "lock keys: Windows is locked, over the %s: every "
                                                                      "controller is a mouse, or types, the bridged ones "
                                                                      "held from the PC"
                                                                    : "lock keys: a User Account Control prompt, over the "
                                                                      "%s: its own controls until it goes",
                                        was == LOCK_KEYS_DESKTOP ? "Desktop Mode"
                                        : was == LOCK_KEYS_LOCK  ? "lock"
                                                                 : "prompt");
#endif
            }
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
        /* A bridged pad's touchpad, the PC's mouse under the controls, let go. */
        bridged_touchpad_tick(input, false);
        return;
    }
#if defined(TARGET_WEBOS)
    const uint32_t now = SDL_GetTicks();
    /* ⓘ Not for Desktop Mode, which waits for nothing: the probe's own ask every
     * second finds a lock or a prompt. */
    if (s_mode != LOCK_KEYS_DESKTOP && now - s_last_ask >= LOCK_ASK_MS) {
        s_last_ask = now;
        ctm_bridge_agent_recheck();
    }
#endif
    /* ⓘ While the overlay or the keyboard has the input, what comes up is not seen
     * here. So everything is forgotten as they take it, and as they give it back. */
    const bool interface_has_input = ui_should_block_input();
    if (interface_has_input != s_interface_had_input) {
        s_interface_had_input = interface_has_input;
        /* And Window Select's Alt goes up: the overlay or the keyboard has the input. */
        window_select_let_go(input, "the overlay or the keyboard took the input");
        pads_forget(input);
        /* Nor does the mouse go on moving, or stay down, behind them. */
        vmouse_set_vector(&input->vmouse, 0, 0);
        vmouse_set_scroll(&input->vmouse, 0, 0);
        vmouse_set_trigger(&input->vmouse, 0, 0);
    }
    /* Options held LOCK_DISMISS_MS alone, on any pad this takes: the rebinds off, which
     * asks first, in Desktop Mode as well since 2026-11-12 (rhoquinn8217: "holding
     * options still should turn rebinds off", "keep the question"). Its OK ends this on
     * the next pass, as an unlock does. Until then Desktop Mode went off at once, with
     * no question. */
    const uint32_t held_at = SDL_GetTicks();
    for (int i = 0; i < LOCK_SLOTS; ++i) {
        if (s_pads[i].options_since != 0 && held_at - s_pads[i].options_since >= LOCK_DISMISS_MS) {
            s_pads[i].options_since = 0;
            lock_keys_ask_dismiss("Options held 2 s");
            break;
        }
    }
    if (input != NULL && !interface_has_input) {
        two_press_tick(input);
    }
    /* ⭐ A bridged pad's touchpad as the PC's mouse, as a pad's on the stream is
     * (stream/input/bridged_touchpad.h); let go while the overlay or the keyboard has
     * the input. */
    bridged_touchpad_tick(input, input != NULL && !interface_has_input);
    stick_moves_tv_cursor(input);
}
