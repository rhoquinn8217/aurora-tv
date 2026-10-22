/* The Settings windows reach from just under the launcher's top bar to near
 * the bottom of the screen: the one the gear opens, and the card each of its
 * sections (Input, Host, ...) opens over it.
 *
 * ⭐ WHY (rhoquinn8217, 2026-10-21): "make the settings window taller". Of the
 * two heights drawn for them on 2026-10-22, "B": the top just under the top
 * bar, which stays in sight, and the bottom LV_DPX(20) above the screen's:
 * 920 px of a 1080 UI, 644 of the rooted monitor's 804. Upstream centres both,
 * shorter: the gear's window was 500 px on that monitor. A section's card now
 * covers the window it opened from exactly.
 *
 * ⓘ Settings opens only from the launcher (launcher.controller.c,
 * open_settings), so the top bar is always there above them. */

#ifndef SETTINGS_CARD_H
#define SETTINGS_CARD_H

#include "lvgl.h"

/* For the gear's window, in the launcher's Settings layer (which starts under
 * the top bar), once upstream's code has sized and centred it. */
void settings_window_under_topbar(lv_obj_t *panel);

/* For a section's message box, on the whole screen, once upstream's code has
 * sized and centred it. Its content grows by as much as the card, so
 * upstream's margins stay. */
void settings_card_under_topbar(lv_obj_t *mbox);

#endif /* SETTINGS_CARD_H */
