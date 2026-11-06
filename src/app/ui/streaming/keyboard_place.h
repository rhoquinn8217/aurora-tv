/* ⭐ CREATE MOVES THE ON-SCREEN KEYBOARD (rhoquinn8217, 2026-11-01: "I would like
 * some button function to move it to the top of the screen"). Between the bottom
 * of the screen and the top, so it need not lie over the box being typed into.
 * In every stream, not only at a lock: it is the same keyboard. It stays where it
 * was put until the app closes. A tab on the keyboard's edge says where it goes
 * next, and a click on the tab moves it as Create does (keyboard_tabs.h). */

#pragma once

#include <stdbool.h>

#include <SDL.h>

#include "lvgl.h"

/* The keyboard's panel, just made: in its place. */
void keyboard_place_attach(lv_obj_t *panel);

/* A controller's button while the keyboard is open. True: it was Create, and it is
 * dealt with. */
bool keyboard_place_button(SDL_JoystickID which, Uint8 button, bool pressed);

/* To the other end of the screen, as Create and the Move tab do. */
void keyboard_place_toggle(void);

/* At the top of the screen, or at the bottom. */
bool keyboard_place_at_top(void);

/* Where it is, for the control port: "top" or "bottom". */
const char *keyboard_place_name(void);
