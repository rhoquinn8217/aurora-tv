#pragma once

/* Stream actions: Vibepollo's host controls, out of the game grid and into one
 * button in the launcher's top bar.
 *
 * rhoquinn8217, 2026-10-17: "I want to hide the extra Remote monitor, remote
 * input, resume, terminate tiles however I want to add a menu at the top that
 * list them in a dropdown." Vibepollo (2.0) sends these as apps in the host's
 * app list, and to a TV that did not start the running app it also sends a
 * copy of that app first, which shows as a second, identical tile. All of
 * them leave the grid; the controls are offered from "Stream actions", beside
 * the host button, and the copy is left out (Resume does the same thing).
 *
 * Terminate asks first, warning that everyone streaming is kicked off, and
 * then sends what Vibepollo asks of a TV that did not start the app: the
 * launch twice, the second confirming the first.
 *
 * A host that sends none of these (anything but Vibepollo) changes nothing:
 * its grid is as before and the button stays hidden. */

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

/* Sizes and places that badge: a ringed circle in the tile's top-left corner. */
void stream_actions_badge_style(lv_obj_t *badge, lv_coord_t tile_width);

/* A tile pressed. True when it was handled here: another app runs, and the
 * person is asked before it is closed for this one. */
bool stream_actions_tile_pressed(const uuidstr_t *host, const apploader_list_t *apps, int app_id);
