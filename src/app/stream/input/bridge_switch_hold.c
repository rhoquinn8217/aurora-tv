#include "bridge_switch_hold.h"

#include <Limelight.h>
#include <SDL.h>
#include <stdint.h>
#include <string.h>

#include "input/app_input.h"
#include "logging.h"

/* What bridge_switch_hold.h says, per Moonlight slot. */
#define SWITCH_SLOTS 16
#define SWITCH_AXIS_LT    0x01
#define SWITCH_AXIS_RT    0x02
#define SWITCH_AXIS_LEFT  0x04
#define SWITCH_AXIS_RIGHT 0x08
#define SWITCH_FINGERS    8

static struct {
    SDL_JoystickID instance_id;   /* the pad that held them */
    int buttons;
    uint8_t axes;
    uint8_t fingers;
} s_held[SWITCH_SLOTS];

void bridge_switch_hold_reset(void) {
    /* ⓘ At a stream's start and end (2026-10-23): a hold left at the end of a
     * stream masked that button's first press in the next one. */
    memset(s_held, 0, sizeof s_held);
}

/* This pad's slot, or NULL. ⓘ A slot another pad held is emptied first: a pad
 * that left while something was held passes nothing on. */
static void *held_for(const app_gamepad_state_t *gamepad) {
    if (gamepad == NULL || gamepad->gs_id < 0 || gamepad->gs_id >= SWITCH_SLOTS) {
        return NULL;
    }
    if (s_held[gamepad->gs_id].instance_id != gamepad->instance_id) {
        s_held[gamepad->gs_id].instance_id = gamepad->instance_id;
        s_held[gamepad->gs_id].buttons = 0;
        s_held[gamepad->gs_id].axes = 0;
        s_held[gamepad->gs_id].fingers = 0;
    }
    return &s_held[gamepad->gs_id];
}

void bridge_switch_hold_begin(const app_gamepad_state_t *gamepad) {
    if (held_for(gamepad) == NULL) {
        return;
    }
    const int slot = gamepad->gs_id;
    s_held[slot].buttons = gamepad->buttons;
    uint8_t axes = 0;
    if ((unsigned char) gamepad->leftTrigger != 0) axes |= SWITCH_AXIS_LT;
    if ((unsigned char) gamepad->rightTrigger != 0) axes |= SWITCH_AXIS_RT;
    /* ⓘ After the stream's own dead zone, so a stick only resting is at 0. */
    if (gamepad->leftStickX != 0 || gamepad->leftStickY != 0) axes |= SWITCH_AXIS_LEFT;
    if (gamepad->rightStickX != 0 || gamepad->rightStickY != 0) axes |= SWITCH_AXIS_RIGHT;
    s_held[slot].axes = axes;
    uint8_t fingers = 0;
#if SDL_VERSION_ATLEAST(2, 0, 14)
    if (gamepad->controller != NULL && SDL_GameControllerGetNumTouchpads(gamepad->controller) > 0) {
        const int count = SDL_GameControllerGetNumTouchpadFingers(gamepad->controller, 0);
        for (int finger = 0; finger < count && finger < SWITCH_FINGERS; ++finger) {
            Uint8 down = 0;
            float x, y, pressure;
            if (SDL_GameControllerGetTouchpadFinger(gamepad->controller, 0, finger, &down, &x, &y,
                                                    &pressure) == 0 && down) {
                fingers |= (uint8_t) (1u << finger);
            }
        }
    }
#endif
    s_held[slot].fingers = fingers;
    if (s_held[slot].buttons != 0 || axes != 0 || fingers != 0) {
        commons_log_info("Input", "Controller %d back from the bridge holding buttons 0x%x, axes 0x%x, "
                                  "fingers 0x%x: the stream sees them released until they are let go",
                         slot, (unsigned) s_held[slot].buttons, (unsigned) axes, (unsigned) fingers);
    }
}

void bridge_switch_hold_note_button(const app_gamepad_state_t *gamepad, int button_flag, bool down) {
    if (down || held_for(gamepad) == NULL) {
        return;
    }
    s_held[gamepad->gs_id].buttons &= ~button_flag;
}

void bridge_switch_hold_note_axes(const app_gamepad_state_t *gamepad) {
    if (held_for(gamepad) == NULL) {
        return;
    }
    uint8_t *axes = &s_held[gamepad->gs_id].axes;
    if ((*axes & SWITCH_AXIS_LT) && (unsigned char) gamepad->leftTrigger == 0) *axes &= (uint8_t) ~SWITCH_AXIS_LT;
    if ((*axes & SWITCH_AXIS_RT) && (unsigned char) gamepad->rightTrigger == 0) *axes &= (uint8_t) ~SWITCH_AXIS_RT;
    if ((*axes & SWITCH_AXIS_LEFT) && gamepad->leftStickX == 0 && gamepad->leftStickY == 0) {
        *axes &= (uint8_t) ~SWITCH_AXIS_LEFT;
    }
    if ((*axes & SWITCH_AXIS_RIGHT) && gamepad->rightStickX == 0 && gamepad->rightStickY == 0) {
        *axes &= (uint8_t) ~SWITCH_AXIS_RIGHT;
    }
}

bridge_switch_axes_t bridge_switch_hold_axes(const app_gamepad_state_t *gamepad) {
    bridge_switch_axes_t out = {
        (unsigned char) gamepad->leftTrigger, (unsigned char) gamepad->rightTrigger,
        gamepad->leftStickX, gamepad->leftStickY, gamepad->rightStickX, gamepad->rightStickY,
    };
    if (held_for(gamepad) == NULL) {
        return out;
    }
    const uint8_t axes = s_held[gamepad->gs_id].axes;
    if (axes & SWITCH_AXIS_LT) out.lt = 0;
    if (axes & SWITCH_AXIS_RT) out.rt = 0;
    if (axes & SWITCH_AXIS_LEFT) { out.lx = 0; out.ly = 0; }
    if (axes & SWITCH_AXIS_RIGHT) { out.rx = 0; out.ry = 0; }
    return out;
}

bool bridge_switch_hold_skips_finger(const app_gamepad_state_t *gamepad, int finger, bool lifted) {
    if (finger < 0 || finger >= SWITCH_FINGERS || held_for(gamepad) == NULL) {
        return false;
    }
    const uint8_t bit = (uint8_t) (1u << finger);
    if ((s_held[gamepad->gs_id].fingers & bit) == 0) {
        return false;
    }
    if (lifted) {
        s_held[gamepad->gs_id].fingers &= (uint8_t) ~bit;   /* the next touch is a new one */
    }
    return true;
}

void bridge_switch_hold_send(const app_gamepad_state_t *gamepad, short active_mask, bool buttons_only) {
    int buttons = gamepad->buttons;
    if (held_for(gamepad) != NULL) {
        buttons &= ~s_held[gamepad->gs_id].buttons;
    }
    if (buttons_only) {
        LiSendMultiControllerEvent(gamepad->gs_id, active_mask, buttons, 0, 0, 0, 0, 0, 0);
        return;
    }
    const bridge_switch_axes_t axes = bridge_switch_hold_axes(gamepad);
    LiSendMultiControllerEvent(gamepad->gs_id, active_mask, buttons, axes.lt, axes.rt,
                               axes.lx, axes.ly, axes.rx, axes.ry);
}
