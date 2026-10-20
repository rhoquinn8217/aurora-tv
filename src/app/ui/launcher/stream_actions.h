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
 * the tile's small picture alone (the running app, this TV's Remote Input or
 * Remote Monitor), or "Streaming: None"; the two buttons sit on the left of
 * the bar; and the menu opens with a line saying in full what streams, then
 * shows every entry, greyed when it cannot be used, with Resume and Terminate
 * on the TV that started the app done as Aurora's own resume and quit.
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
 * has just loaded, before the grid shows it, and puts the list in its order:
 * this TV's Remote Monitor or Remote Input tile while it is one, the running
 * app, Desktop, the starred, the rest. Launchers (Steam, Playnite ...) are
 * starred once, by themselves. */
void stream_actions_take(const uuidstr_t *host, apploader_list_t *apps);

/* The running app's tile id: what the host reported, or, when it says nothing
 * runs because another TV started it, the app its copy names. For the badge. */
int stream_actions_running_id(const uuidstr_t *host, int reported);

/* Whether a tile wears the badges: the running app's (`running_id`, from the
 * call above), or this TV's Remote Monitor or Remote Input tile. */
bool stream_actions_tile_badged(const uuidstr_t *host, int running_id, int app_id);

/* Whether a host's list, as it loaded, shows an app running that another TV
 * started: the running app's copy, or Resume or Terminate. For the server
 * window, which reads every host's list. */
bool stream_actions_list_shows_running(const apploader_list_t *apps);

/* Makes that badge a row along the tile's bottom holding two see-through ones,
 * each a white ring and half-see-through purple: Play, the triangle cut out, in
 * the bottom-left corner, and Stop, a square cut out, in the bottom-right.
 * Play resumes and Stop stops (asking first, for an app), and either grows
 * and brightens under a pointer. */
void stream_actions_badge_style(lv_obj_t *badge, lv_coord_t tile_width);

/* A tile pressed. True when it was handled here: another app runs, and the
 * person is asked before it is closed for this one; or it is this TV's Remote
 * Monitor or Remote Input tile, which goes back into it, as its Play does. */
bool stream_actions_tile_pressed(const uuidstr_t *host, const apploader_list_t *apps, int app_id);
