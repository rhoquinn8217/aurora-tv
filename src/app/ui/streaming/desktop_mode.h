/* ⭐⭐ DESKTOP MODE: ITS SWITCH (rhoquinn8217, 2026-11-11).
 * What the controllers do in it is stream/input/lock_keys.h's: the User Account Control
 * prompt's controls on every controller, bridged ones too, until it is turned off, with
 * L1 and R1 stepping through Windows' window picker (stream/input/window_select.h), the
 * PS button showing its list over the screen, Triangle switching the TV cursor and
 * Options held turning it off (below). Its list is ui/streaming/lock_numpad.h's, in the
 * overlay, and over the screen by the PS button.
 *
 * WHY. "Desktop Mode is the solution to first log in after a reboot, accessible with a
 * bridge attempt or overlay button. It's evolved into a DS5-USBIP alternative ... In the
 * case where you are playing full screen mode and need a mouse and keyboard, switching to
 * Desktop mode is cleaner." It needs nothing from DS5-USBIP, which is not running at the
 * first sign-in after a restart.
 *
 * WHERE IT IS TURNED ON AND OFF: the overlay's Desktop Mode button, which took the place
 * of Full keyboard and Virtual Mouse (ui/streaming/bridge_overlay.c); Settings > USB
 * Bridge; Options held on a controller (below); and the two pop-ups
 * (ui/streaming/bridge_prompt.h): its warning when a controller is bridged with it on,
 * and a refusal with DS5-USBIP offline, which offers to turn it on. Saved at once, off
 * by default (bridge_settings.h).
 *   - Turned on in a stream with DS5-USBIP answering, it says "Warning: DS5-USBIP
 *     configs are disabled when Desktop Mode is on." for four seconds (rhoquinn8217,
 *     2026-11-12: "Whenever Desktop Mode is turned on and DS5-USBIP is on").
 *   - It turns Virtual Mouse off ("turn off virtual mouse similar an any other setting
 *     that would conflict with the rebinds"). Turning it off leaves Virtual Mouse off:
 *     "We are effectively phasing out mouse mode".
 * ⓘ Its questions at a stream's start, "Turn on Desktop Mode?" and "Turn off Desktop
 * Mode?", were removed on 2026-11-12 with every pop-up but the two ("Get rid of all of
 * these"). */

#pragma once

#include <stdbool.h>

typedef struct app_t app_t;

/* Is it switched on (saved)? */
bool desktop_mode_on(void);

/* Switched on or off, saved, with how in the log. On also turns Virtual Mouse off. Says
 * nothing on the screen: the caller says it (bridge_prompt_say_desktop_on). */
void desktop_mode_set(bool on, const char *how);

/* Once a pass of the app's loop: its list hidden again at each stream. */
void desktop_mode_tick(app_t *app);

/* ⭐ ITS LIST OVER THE SCREEN, BY THE PS BUTTON (rhoquinn8217, 2026-11-11: "Show the
 * legend over the screen", on Triangle first, then "Rebind the PS button to to the
 * Legend instead"). Until then only in the overlay, and still there whichever way this
 * is. Hidden again at each stream ("5. hidden") and as Desktop Mode is turned off.
 * Shown by ui/streaming/lock_numpad.c. */
bool desktop_mode_legend_shown(void);
void desktop_mode_legend_toggle(void);

/* ⭐ THE TV CURSOR BY TRIANGLE (rhoquinn8217, 2026-11-11): "It will toggle cursor on/off
 * but will allow cursor alignment when turned on". Off: on, and the alignment bar up
 * (ui/streaming/cursor_align.h), where the d-pad moves its tip, Cross keeps it and
 * Circle puts back what was there, the cursor staying on ("on cancel cursor stays up").
 * On: off. The same saved switch as Ctrl+Alt+Shift+M (input/tv_cursor.h). */
void desktop_mode_cursor_toggle(void);

/* ⭐ OPTIONS HELD 2 S ON A CONTROLLER TURNS IT OFF, at once and with no question
 * (rhoquinn8217, 2026-11-11: "Options: hold to turn off Desktop Mode", and "4. yes" to
 * the UAC list's 2 s and no "Are you sure?"). No message says so since 2026-11-12. */
void desktop_mode_off_on_pad(void);
