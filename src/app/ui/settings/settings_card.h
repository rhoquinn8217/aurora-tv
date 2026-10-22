/* The card a Settings section opens in reaches from just under the launcher's
 * top bar to near the bottom of the screen.
 *
 * ⭐ WHY (rhoquinn8217, 2026-10-21): "make the settings window taller". Of the
 * two heights drawn for them on 2026-10-22, "B": the card's top just under the
 * top bar, which stays in sight, and its bottom LV_DPX(20) above the screen's.
 * 920 px of a 1080 UI, against upstream's 840 centred: about 5.2 rows of a
 * section in sight, from 4.7.
 *
 * ⓘ Settings opens only from the launcher (launcher.controller.c,
 * open_settings), so the top bar is always there above the card. */

#ifndef SETTINGS_CARD_H
#define SETTINGS_CARD_H

#include "lvgl.h"

/* For the section's message box, once upstream's code has sized and centred
 * it. Its content grows by as much as the card, so upstream's margins stay; a
 * screen with no room to grow keeps upstream's card. */
void settings_card_under_topbar(lv_obj_t *mbox);

#endif /* SETTINGS_CARD_H */
