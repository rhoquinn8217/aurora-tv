/* A controller's buttons as DS5-USBIP's configs window draws them: a PlayStation
 * pad's shapes in a circle, an Xbox pad's letters in a circle, a shoulder button
 * or a trigger in a shoulder button's shape, and the d-pad as a cross with one arm
 * filled.
 *
 * ⭐ DRAWN (rhoquinn8217, 2026-11-01: "showing the numpad with the symbols that
 * switch base on the controller you are using like the DS5-USBIP button symbols",
 * and "symbols might be hard to do unless they are drawn"). The app's fonts hold
 * none of them, and Create and the Xbox pad's View are in no font at all. The
 * shapes are the page's own numbers (keyCap, shoulderBox and dpadArm in the
 * listener's tools/controller-config-test-client.html), on its 20-high grid.
 *
 * ⭐ THE MARKS FOLLOW THE PAD THAT PRESSED LAST, as that page's legend does, and
 * by its rule: a DualSense, an Edge or a DS4 gets the PlayStation set, anything
 * else the Xbox set. */

#pragma once

#include <stdbool.h>

#include <SDL.h>

#include "lvgl.h"

typedef enum {
    PAD_FAMILY_PLAYSTATION = 0,
    PAD_FAMILY_XBOX,
} pad_family_t;

/* A button by where it sits, whatever its pad calls it. */
typedef enum {
    PAD_MARK_UP = 0,
    PAD_MARK_DOWN,
    PAD_MARK_LEFT,
    PAD_MARK_RIGHT,
    PAD_MARK_SOUTH,   /* Cross, A */
    PAD_MARK_EAST,    /* Circle, B */
    PAD_MARK_WEST,    /* Square, X */
    PAD_MARK_NORTH,   /* Triangle, Y */
    PAD_MARK_L1,
    PAD_MARK_R1,
    PAD_MARK_L2,
    PAD_MARK_R2,
    PAD_MARK_VIEW,    /* Create, View */
    PAD_MARK_MENU,    /* Options, Menu */
} pad_mark_t;

/* The mark, `height` pixels high, as an object in `parent` that takes no input.
 * A shoulder button's is a fifth wider than it is high, the others square. */
lv_obj_t *pad_mark_create(lv_obj_t *parent, pad_mark_t mark, pad_family_t family, lv_coord_t height,
                          lv_color_t color);

/* This pad pressed something. */
void pad_family_note(SDL_JoystickID instance_id);

/* The set for the pad that pressed last, or the first connected if that one has
 * gone. With no pad at all, the PlayStation set: this project's pad. */
pad_family_t pad_family_current(void);

/* For a picture of the set not in hand: `force` is a family, or -1 to follow the
 * pad again. Not saved. */
void pad_family_force(int force);
