#pragma once

/* Stream actions: Vibepollo's host controls, out of the game grid and into one
 * button in the launcher's top bar.
 *
 * rhoquinn8217, 2026-10-17: "I want to hide the extra Remote monitor, remote
 * input, resume, terminate tiles however I want to add a menu at the top that
 * list them in a dropdown." Vibepollo (2.0) sends these as apps in the host's
 * app list, and to a TV that did not start the running app it also sends a
 * copy of that app first, which shows as a second, identical tile. All of
 * them leave the grid; the controls are offered from a button beside the host
 * button, and the copy is left out (Resume does the same thing).
 *
 * ⭐ Since 2026-10-20 (their words): the button reads "Streaming: <tile>",
 * with the tile's small picture (the running app, this TV's Remote Input or
 * Remote Monitor, or None); the two buttons sit on the left of the bar; and
 * the menu shows every entry, greyed when it cannot be used, with Resume and
 * Terminate on the TV that started the app done as Aurora's own resume and
 * quit.
 *
 * Terminate asks first, warning that everyone streaming is kicked off, and
 * then sends what Vibepollo asks of a TV that did not start the app: the
 * launch twice, the second confirming the first.
 *
 * A host that sends none of these (anything but Vibepollo) keeps its grid as
 * before; the button still says what streams, and only the TV that started
 * an app can use its menu (Resume and Terminate). */

#include "lvgl.h"
#include "uuidstr.h"

#include "launcher.controller.h"
#include "backend/apploader/apploader.h"

/* Makes the button, as a child of the top bar right after the host button. */
void stream_actions_button(launcher_fragment_t *launcher, lv_obj_t *topbar);

/* Takes the host's controls (and the running app's copy) out of a list that
 * has just loaded, before the grid shows it. */
void stream_actions_take(const uuidstr_t *host, apploader_list_t *apps);

/* The running app's tile id: what the host reported, or, when it says nothing
 * runs because another TV started it, the app its copy names. For the badge. */
int stream_actions_running_id(const uuidstr_t *host, int reported);

/* Whether a host's list, as it loaded, shows an app running that another TV
 * started: the running app's copy, or Resume or Terminate. For the server
 * window, which reads every host's list. */
bool stream_actions_list_shows_running(const apploader_list_t *apps);

/* Sizes and places that badge in the tile's top-left corner, and makes it the
 * see-through one: a white ring, a quarter of purple, the triangle cut out. */
void stream_actions_badge_style(lv_obj_t *badge, lv_coord_t tile_width);

/* A tile pressed. True when it was handled here: another app runs, and the
 * person is asked before it is closed for this one. */
bool stream_actions_tile_pressed(const uuidstr_t *host, const apploader_list_t *apps, int app_id);
