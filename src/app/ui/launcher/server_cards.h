#pragma once

/* Server cards: in "Select server", each computer shows the picture of its
 * Desktop app instead of a plain icon.
 *
 * rhoquinn8217, 2026-10-20: "instead of creating a new image to manage, just
 * use the same image being used for the Desktop of each computer. It's okay if
 * it says Desktop." A host sends a picture only for each app, so the TV
 * remembers which app is a host's Desktop whenever that host's list loads,
 * keeps it in conf/server-cards.txt, and shows that app's cover on the host's
 * tile through the same loader and cache as the game grid. A host whose list
 * this TV has never loaded, or that has no app named "Desktop", keeps the icon
 * and name, as before.
 *
 * On a card, a host that is not ready shows the window's own status icon in a
 * small corner badge, and a host that is offline or not paired is dimmed. */

#include "lvgl.h"
#include "uuidstr.h"

#include "backend/types.h"
#include "backend/apploader/apploader.h"

typedef struct app_t app_t;

/* Notes the host's Desktop app from a list that has just loaded. */
void server_cards_learn(const uuidstr_t *host, const apploader_list_t *apps);

/* Puts the host's card on a server tile that has just been bound, or leaves the
 * tile as it is when no card is known. */
void server_cards_bind(lv_obj_t *tile, app_t *app, const uuidstr_t *host, const char *status_icon,
                       SERVER_STATE_ENUM state);

/* The window has closed: lets the cards' pictures go. */
void server_cards_closed(void);
