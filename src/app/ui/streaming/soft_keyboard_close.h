/* The stream's on-screen keyboard has a Close tab.
 *
 * ⭐ WHY (rhoquinn8217, 2026-10-23): "for gui's aurora-tv on screen keyboard add
 * a button to close it." GuiDev1994's keyboard (soft_keyboard.c) closes with
 * the remote's Back or a pad's Circle, and nothing on it does: with a pointer
 * alone there was no way out. So a "Close" tab sits on its panel's top edge,
 * at the right end, and a click closes it exactly as Back does
 * (USER_CLOSE_SOFT_KEYBOARD). Back and Circle are unchanged.
 *
 * ⓘ A click on the dimmed picture above the keyboard does NOT close it, on
 * purpose (rhoquinn8217, same day): keys are clicked with the remote's
 * pointer, and a click that misses a key would close the keyboard and force it
 * open again, often. */

#pragma once

#include "lvgl.h"

/* For upstream's keyboard (soft_keyboard.c), once it has made its full-screen
 * container and the panel that holds the keys. */
void soft_keyboard_close_tab(lv_obj_t *keyboard, lv_obj_t *panel);
