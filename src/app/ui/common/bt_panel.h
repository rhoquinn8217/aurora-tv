/* The Bluetooth Devices panel (its title since 2026-10-29): what the TV has
 * registered, Forget on each
 * and Forget all, and a Search switch that pairs a fresh device and turns
 * itself off (input/bt_pairing.h has the why and the TV's calls).
 *
 * ⭐ WHERE IT OPENS (rhoquinn8217, 2026-10-29, the picture they said "build" to):
 *   - the dashboard: a controller button in the top bar, after Stream actions
 *     and before Add, Help, Settings and Close; the panel in the middle;
 *   - the streaming overlay: a Controllers button in the overlay's row, after
 *     DS5-USBIP; the panel on the right, where the USB Bridge panel opens, with
 *     the overlay left open behind it, so a bridged controller's presses stay
 *     out of the game.
 * ⓘ Closing the panel turns the search off. */

#pragma once

#include <stdbool.h>

#include "lvgl.h"
#include "ui/launcher/launcher.controller.h"

/* The dashboard's button, made in the top bar `topbar` in its own place, and
 * styled as the icons beside it. When: as the launcher builds its top bar. */
void bt_panel_topbar_button(launcher_fragment_t *launcher, lv_obj_t *topbar);

/* Open the panel: in the middle of the screen, or on the right over a stream.
 * Nothing happens if it is open already. */
void bt_panel_open(bool over_stream);

/* Close it, and turn the search off with it. */
void bt_panel_close(void);

/* Whether it is open. */
bool bt_panel_shown(void);
