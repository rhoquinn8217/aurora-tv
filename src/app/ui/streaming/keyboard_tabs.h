/* The stream's on-screen keyboard has two tabs at its edge: Move and Close.
 *
 * ⭐ CLOSE (rhoquinn8217, 2026-10-23): "for gui's aurora-tv on screen keyboard
 * add a button to close it." GuiDev1994's keyboard (soft_keyboard.c) closes with
 * the remote's Back or a pad's Circle, and nothing on it does: with a pointer
 * alone there was no way out. A click on Close closes it exactly as Back does
 * (USER_CLOSE_SOFT_KEYBOARD). Back and Circle are unchanged. Beside the word,
 * Circle's mark while a controller is connected, as Move has Create's: a cross
 * there read as the button to press, and Cross types (rhoquinn8217, 2026-11-07:
 * "Change it to a circle in a button instead of an x").
 *
 * ⓘ And one purple halo on the selected key, the edge without the outline
 * GuiDev1994 drew around it (rhoquinn8217, 2026-11-07: "virtual keyboard has 2
 * purple halos"), from the same hook (keyboard_tabs_create).
 *
 * ⭐ MOVE (rhoquinn8217, 2026-11-06, of the button that switches its place: "yes,
 * make it clickable"). Create moves the keyboard between the bottom of the screen
 * and the top (keyboard_place.h), and a line beside the keys said so. That line
 * is this tab now: a click moves the keyboard as Create does. It shows with no
 * controller connected too, without Create's mark then.
 *
 * ⓘ WHERE (their picture, the same day): side by side at the right end, Close
 * last, its right end over the keys' right end (F12's), on the side that faces
 * the middle of the screen: above the keys while the keyboard is at the bottom,
 * below them while it is at the top.
 *
 * ⭐ NO BOX (rhoquinn8217, the same day: "can you have them floating and not
 * inside of a button?"). A tab is its mark and its words alone, on the panel, in
 * the strip between the panel's edge and the keys, so they read over any picture.
 * Its padding stays as the target a pointer aims at, and a press turns it the
 * purple a focused key is outlined in.
 *
 * ⓘ A click on the dimmed picture does NOT close the keyboard, on purpose
 * (rhoquinn8217, 2026-10-23): keys are clicked with the remote's pointer, and a
 * click that missed a key would close the keyboard and force it open again,
 * often. */

#pragma once

#include "lvgl.h"

/* For upstream's keyboard (soft_keyboard.c), once it has made its full-screen
 * container and the panel that holds the keys. */
void keyboard_tabs_create(lv_obj_t *keyboard, lv_obj_t *panel);

/* To the edge that faces the middle now, Move with the words for where it goes
 * next (keyboard_place.c, after each move). Nothing while no keyboard is up. */
void keyboard_tabs_place(void);
