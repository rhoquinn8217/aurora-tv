/* A bridged pad's touchpad as the PC's mouse under the TV's controls. The why is in the header. */

#include "bridged_touchpad.h"

#include <stdio.h>
#include <string.h>

#include <SDL.h>

#include "app_settings.h"
#include "input/app_input.h"
#include "input/input_gamepad.h"
#include "input/bridge_override.h"
#include "stream/input/session_input.h"
#if defined(TARGET_WEBOS)
#include "ctm_bridge_glue.h"
#include "input/ctm_bridge_gesture.h"
#define pad_note(...) ctm_bridge_gesture_note("bridged touchpad: " __VA_ARGS__)
#endif

/* The bridged pads followed at once: as many as the app takes. */
#define FED_SLOTS 4

/* The touchpad as the core reads it (held_touch.inl): X 0 to 1919, Y 0 to 1079. */
#define RAW_W 1920.0f
#define RAW_H 1080.0f

typedef struct {
    bool down;
    int id;       /* its contact count: a new touch changes it */
    float x, y;   /* 0 to 1, as SDL gives a touch */
} finger_t;

typedef struct {
    char node[64];          /* the bridged device's node; empty: free */
    SDL_JoystickID which;   /* its pad's SDL instance */
    finger_t finger[2];     /* as handed over */
    bool click;             /* as handed over */
} fed_t;

static fed_t s_fed[FED_SLOTS];
static bool s_feeding;

bool bridged_touchpad_feeding(void) {
    return s_feeding;
}

bool bridged_touchpad_owns(const app_gamepad_state_t *gamepad) {
    if (gamepad == NULL) {
        return false;
    }
    for (int i = 0; i < FED_SLOTS; ++i) {
        if (s_fed[i].node[0] != '\0' && s_fed[i].which == gamepad->instance_id) {
            return true;
        }
    }
    return false;
}

static void hand_touch(stream_input_t *input, const fed_t *f, Uint32 type, int finger) {
    SDL_ControllerTouchpadEvent event;
    SDL_zero(event);
    event.type = type;
    event.timestamp = SDL_GetTicks();
    event.which = f->which;
    event.touchpad = 0;
    event.finger = finger;
    event.x = f->finger[finger].x;
    event.y = f->finger[finger].y;
    event.pressure = type == SDL_CONTROLLERTOUCHPADUP ? 0.0f : 1.0f;
    s_feeding = true;
    stream_input_handle_ctouchpad(input, &event);
    s_feeding = false;
}

static void hand_click(stream_input_t *input, const fed_t *f, bool down) {
    SDL_ControllerButtonEvent event;
    SDL_zero(event);
    event.type = down ? SDL_CONTROLLERBUTTONDOWN : SDL_CONTROLLERBUTTONUP;
    event.timestamp = SDL_GetTicks();
    event.which = f->which;
    event.button = SDL_CONTROLLER_BUTTON_TOUCHPAD;
    event.state = down ? SDL_PRESSED : SDL_RELEASED;
    s_feeding = true;
    stream_input_handle_cbutton(input, &event);
    s_feeding = false;
}

/* What was handed over let go, the click before the fingers as a hand lifts, and the
 * slot freed. ⛔ Never a button left down on the PC: the stream's handling lets go of
 * what it holds as the last finger lifts (stream/input/touchpad_drag_lock.h). */
static void let_go(stream_input_t *input, fed_t *f) {
    if (input != NULL && f->node[0] != '\0') {
        if (f->click) {
            hand_click(input, f, false);
        }
        for (int k = 1; k >= 0; --k) {
            if (f->finger[k].down) {
                hand_touch(input, f, SDL_CONTROLLERTOUCHPADUP, k);
            }
        }
    }
    memset(f, 0, sizeof(*f));
}

/* One reading, handed over as what changed since the last, in the order SDL hands one
 * report's: fingers down and moving, then the click, then fingers up. A two-finger
 * press is told from one by the fingers already down as it goes in. */
static void step(stream_input_t *input, fed_t *f, const finger_t now[2], bool click) {
    bool lifted[2] = {false, false};
    for (int k = 0; k < 2; ++k) {
        finger_t *was = &f->finger[k];
        if (now[k].down && was->down && now[k].id != was->id) {
            /* Another touch where this one was, between two readings: this one lifted. */
            hand_touch(input, f, SDL_CONTROLLERTOUCHPADUP, k);
            was->down = false;
        }
        if (now[k].down) {
            const bool landed = !was->down;
            const bool moved = !landed && (now[k].x != was->x || now[k].y != was->y);
            *was = now[k];
            if (landed) {
                hand_touch(input, f, SDL_CONTROLLERTOUCHPADDOWN, k);
            } else if (moved) {
                hand_touch(input, f, SDL_CONTROLLERTOUCHPADMOTION, k);
            }
        } else if (was->down) {
            lifted[k] = true;
        }
    }
    if (click != f->click) {
        f->click = click;
        hand_click(input, f, click);
    }
    for (int k = 0; k < 2; ++k) {
        if (lifted[k]) {
            hand_touch(input, f, SDL_CONTROLLERTOUCHPADUP, k);
            f->finger[k].down = false;
        }
    }
}

#if defined(TARGET_WEBOS)
/* The pad SDL has at a bridged device's node, if any. */
static app_gamepad_state_t *pad_at(stream_input_t *input, const char *node) {
    const short max = app_input_get_max_gamepads(input->input);
    for (short i = 0; i < max; ++i) {
        app_gamepad_state_t *gamepad = app_input_gamepad_state_by_index(input->input, i);
        if (gamepad == NULL || gamepad->controller == NULL) {
            continue;
        }
        const char *path = SDL_GameControllerPath(gamepad->controller);
        if (path != NULL && ctm_bridge_gesture_path_is_node(path, node)) {
            return gamepad;
        }
    }
    return NULL;
}

/* Where each finger is, from the core's two words: the first's place whenever the first
 * is down, and the second's alone. Both count the fingers, so false when they disagree:
 * the two were read a report apart, and this reading is passed over. */
static bool fingers_of(const ctm_bridge_touch_t *t, finger_t now[2]) {
    const bool second = t->fingers2 != 0;
    if ((t->fingers == 0 && second) || (t->fingers == 2 && t->fingers2 != 2) ||
        (t->fingers == 1 && t->fingers2 == 2) ||
        (t->fingers == 1 && second && (t->x != t->x2 || t->y != t->y2))) {
        return false;
    }
    now[0].down = t->fingers == 2 || (t->fingers == 1 && !second);
    now[0].id = t->id;
    now[0].x = (float) t->x / RAW_W;
    now[0].y = (float) t->y / RAW_H;
    now[1].down = second;
    now[1].id = t->id2;
    now[1].x = (float) t->x2 / RAW_W;
    now[1].y = (float) t->y2 / RAW_H;
    return true;
}
#endif

void bridged_touchpad_tick(stream_input_t *input, bool on) {
#if defined(TARGET_WEBOS)
    on = on && input != NULL && !input->view_only && input->touchpad_mode == TOUCHPAD_MODE_MOUSE &&
         !bridge_override_vmouse_lends_touchpad(input);
    bool seen[FED_SLOTS] = {false};
    if (on) {
        ctm_bridge_touch_t touches[FED_SLOTS];
        const int n = ctm_bridge_held_touches(touches, FED_SLOTS);
        for (int t = 0; t < n; ++t) {
            if (!touches[t].reading) {
                continue;
            }
            app_gamepad_state_t *gamepad = pad_at(input, touches[t].node);
            if (gamepad == NULL) {
                continue;
            }
            int k = -1, free_slot = -1;
            for (int j = 0; j < FED_SLOTS; ++j) {
                if (s_fed[j].node[0] != '\0' && strcmp(s_fed[j].node, touches[t].node) == 0) {
                    k = j;
                    break;
                }
                if (free_slot < 0 && s_fed[j].node[0] == '\0') {
                    free_slot = j;
                }
            }
            if (k >= 0 && s_fed[k].which != gamepad->instance_id) {
                /* Another pad at that node now: the old one's let go first. */
                let_go(input, &s_fed[k]);
                free_slot = k;
                k = -1;
            }
            if (k < 0) {
                if (free_slot < 0) {
                    continue;
                }
                k = free_slot;
                memset(&s_fed[k], 0, sizeof(s_fed[k]));
                snprintf(s_fed[k].node, sizeof(s_fed[k].node), "%s", touches[t].node);
                s_fed[k].which = gamepad->instance_id;
                pad_note("%s's touchpad is the PC's mouse, read by the core", s_fed[k].node);
            }
            seen[k] = true;
            finger_t now[2];
            if (fingers_of(&touches[t], now)) {
                step(input, &s_fed[k], now, touches[t].click);
            }
        }
    }
    for (int j = 0; j < FED_SLOTS; ++j) {
        if (!seen[j] && s_fed[j].node[0] != '\0') {
            pad_note("%s's touchpad let go", s_fed[j].node);
            let_go(input, &s_fed[j]);
        }
    }
#else
    (void) input;
    (void) on;
#endif
}
