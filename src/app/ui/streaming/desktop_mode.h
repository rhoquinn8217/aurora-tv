/* ⭐⭐ DESKTOP MODE: ITS SWITCH, AND THE QUESTIONS ABOUT IT (rhoquinn8217, 2026-11-11).
 * What the controllers do in it is stream/input/lock_keys.h's: the User Account Control
 * prompt's controls on every controller, bridged ones too, until it is turned off. Its
 * list is ui/streaming/lock_numpad.h's, in the overlay only.
 *
 * WHY. "Desktop Mode is the solution to first log in after a reboot, accessible with a
 * bridge attempt or overlay button. It's evolved into a DS5-USBIP alternative ... In the
 * case where you are playing full screen mode and need a mouse and keyboard, switching to
 * Desktop mode is cleaner." It needs nothing from DS5-USBIP, which is not running at the
 * first sign-in after a restart.
 *
 * WHERE IT IS TURNED ON AND OFF: the overlay's Desktop Mode button, which took the place
 * of Full keyboard and Virtual Mouse (ui/streaming/bridge_overlay.c); Settings > USB
 * Bridge; a refusal's pop-up, and the questions below (ui/streaming/bridge_prompt.h).
 * Saved at once, off by default (bridge_settings.h).
 *   - Turning it on says so for four seconds, every time, as a warning ("Warning with
 *     turning on Desktop Mode. All controller included bridged controllers use it and it
 *     overrides DS5-USBIP configs", and "3. every time").
 *   - It turns Virtual Mouse off ("turn off virtual mouse similar an any other setting
 *     that would conflict with the rebinds"). Turning it off leaves Virtual Mouse off:
 *     "We are effectively phasing out mouse mode".
 *
 * ⭐ THE QUESTIONS, THREE SECONDS, NO SELECTED ("it has to be very brief and an accidental
 * press should default to no"), each once:
 *   - a stream starts or resumes with Desktop Mode off and DS5-USBIP not answering:
 *     "Turn on Desktop Mode?" ("show on every stream start or resume when Desktop Mode is
 *     off AND DS5-USBIP if off");
 *   - with Desktop Mode on and DS5-USBIP answering, as a stream starts or resumes, and
 *     whenever DS5-USBIP comes online during one: "Turn off Desktop Mode?" ("1. yes", to
 *     a reminder for someone who forgot it was on).
 * Each waits for DS5-USBIP's first answer in the stream (the probe forgets the last
 * stream's as it wakes), and for no other pop-up, no lock or prompt, and no overlay or
 * keyboard up; one not asked within DESKTOP_ASK_WITHIN_MS of what raised it is dropped,
 * so a question does not land in the middle of a game. */

#pragma once

#include <stdbool.h>

typedef struct app_t app_t;

/* Is it switched on (saved)? */
bool desktop_mode_on(void);

/* Switched on or off, saved, with how in the log. On also turns Virtual Mouse off. Says
 * nothing on the screen: the caller says it (bridge_prompt_say_desktop_on). */
void desktop_mode_set(bool on, const char *how);

/* Once a pass of the app's loop: the questions. */
void desktop_mode_tick(app_t *app);

/* What it waits to ask, for the control port: "on", "off" or "none". */
const char *desktop_mode_pending(void);
