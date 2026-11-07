/* aurora-tv's own on-screen keyboard (ui/streaming/soft_keyboard.c, GuiDev1994's),
 * typing into a box of the app's instead of to a host.
 *
 * ⭐ WHY (rhoquinn8217, 2026-11-06, of the add-a-computer window's number pad for
 * a controller: "add a button that brings up the the aurora-tv virtual keyboard
 * rather than the webOS one"). webOS's own keyboard answers the remote alone: a
 * controller's press reaches the app instead, which moves the focus off the box,
 * and webOS closes its keyboard. The stream's keyboard is the app's own, so a
 * controller walks it: for a computer's name, or an address the number pad
 * cannot type.
 *
 * ⓘ It is the stream's keyboard, opened with no stream: the same keys, Move and
 * Close, and the same buttons. The d-pad and Cross walk and press the keys, L1
 * and R1 move the cursor, L2 switches to the &123 keys, Triangle types a space,
 * Square is Backspace, Options is Enter, Circle closes it and Create moves it.
 * What a key would send to a host goes into the box instead
 * (keyboard_text_send): a letter, a number or a symbol the box accepts,
 * Backspace and Delete, and the arrows, Home and End for the cursor. Enter and
 * Esc close it, Enter as the window's OK would want it. Nothing reaches a host:
 * there is none.
 *
 * ⓘ The window stays in view: it moves into the part of the screen the keyboard
 * leaves free, and follows the keyboard when it moves. */

#pragma once

#include <stdbool.h>

#include <SDL.h>

#include "lvgl.h"

/* The keyboard, typing into `box`, over `window`. `closed` runs once it has
 * gone, `enter` true when Enter closed it. Nothing while one is up. */
void keyboard_text_open(lv_obj_t *box, lv_obj_t *window, void (*closed)(bool enter, void *user), void *user);

/* Is it up? */
bool keyboard_text_shown(void);

/* Closes it, as Close does; as Enter does when `enter`. */
void keyboard_text_close(bool enter);

/* Gone without telling `closed`: for a window that goes away under it. */
void keyboard_text_drop(void);

/* For soft_keyboard.c: a key it would send to a host, `shift` as it is sent.
 * True while this keyboard is up, the key gone into the box; false otherwise,
 * and the stream's keyboard sends it as before. */
bool keyboard_text_send(short vk, bool down, bool shift);

/* For lvgl/input/lv_drv_sdl_key.c: a controller's button while it is up. True
 * when it was done here; false for the d-pad and Cross, which go on as the
 * interface's keys and walk and press the keys. */
bool keyboard_text_button(const SDL_Event *event);

/* For ui/streaming/keyboard_place.c: the window follows the keyboard. */
void keyboard_text_place(void);
