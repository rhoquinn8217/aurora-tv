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
    PAD_MARK_DPAD,    /* the whole d-pad, no arm filled */
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
    /* The PS button: "PS" in a circle (rhoquinn8217, 2026-11-11: "create button symbol
     * with "PS" in a circle ike the other symbol"). ⓘ An Xbox pad's Xbox button has no
     * mark, only its circle: "we have to use the text "Xbox button" since there is not
     * symbol we can use for it", so a legend writes those words in its place. */
    PAD_MARK_HOME,
    /* L3, the left stick pressed: "L3" in a circle, an Xbox pad's "LS", drawn in strokes
     * as the PS button's letters are (rhoquinn8217, 2026-11-12: "Lets move the
     * cursor->alignment button to L3"), as DS5-USBIP's configs window marks it. */
    PAD_MARK_L3,
    /* ⭐ THE TOUCHPAD (rhoquinn8217, 2026-11-13, of the mark DS5-USBIP's configs window
     * was given that day, picked at its fourth brightness: "can we use this for both
     * aurora-tv and DS5-USBIP"): DualSense's touchpad, its top a slight arch and its
     * corners rounded, and the fingers on it as solid dots of the mark's own colour at
     * 40% ("not as bright as the touchpad outline"). The same on every pad: bare, with
     * one finger, with two. */
    PAD_MARK_TOUCHPAD,
    PAD_MARK_TOUCH_ONE,
    PAD_MARK_TOUCH_TWO,
    /* ⭐ THE STICKS (rhoquinn8217, 2026-11-14): a stick tilted, drawn plain, an outline
     * of the stick with its letter, R or L ("remove the arrow on the stick for mouse and
     * scroll"; "make them a little bigger when you build it"); and a stick pressed, L3
     * and R3: a solid flat arrow over an outline of the stick's cap ("outline of the
     * stick but solid arrow", "the same as the text color"). The same on every pad. */
    PAD_MARK_STICK_R,
    PAD_MARK_STICK_L,
    PAD_MARK_STICK_PRESS,     /* L3 (an Xbox pad's LS), its name inside the cap */
    PAD_MARK_STICK_PRESS_R,   /* R3 (RS) */
} pad_mark_t;

/* The mark, `height` pixels high, as an object in `parent` that takes no input.
 * A shoulder button's is a fifth wider than it is high, the touchpad's two fifths,
 * the others square. */
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
