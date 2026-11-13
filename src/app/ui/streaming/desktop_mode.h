/* ⭐⭐ DESKTOP MODE: ITS SWITCH (rhoquinn8217, 2026-11-11).
 * What the controllers do in it is stream/input/lock_keys.h's: the User Account Control
 * prompt's controls on every controller, bridged ones too, until it is turned off, with
 * L1 and R1 stepping through Windows' window picker (stream/input/window_select.h), a
 * two-finger press (an Xbox pad's Share button) showing or hiding its list, L3 switching
 * the TV cursor, PS going to Windows and Options held turning it off, after its question
 * (all three the same at a lock and a prompt since 2026-11-12/13). Its list is
 * ui/streaming/lock_numpad.h's, over the screen unless hidden, and always in the
 * overlay.
 *
 * WHY. "Desktop Mode is the solution to first log in after a reboot, accessible with a
 * bridge attempt or overlay button. It's evolved into a DS5-USBIP alternative ... In the
 * case where you are playing full screen mode and need a mouse and keyboard, switching to
 * Desktop mode is cleaner." It needs nothing from DS5-USBIP, which is not running at the
 * first sign-in after a restart.
 *
 * ⭐⭐ WHAT IT IS FOR, SINCE 2026-11-12 (rhoquinn8217): "The common case is that the
 * stream starts and the user relies solely on DS5-USBIP to manage controller rebinds.
 * Desktop mode has the following purpose for existing: backup for when DS5-USBIP can't
 * work, crashes or closes and the user need these basic rebinds to control the PC most
 * likely to turn DS5-USBIP back on; the user doesn't want to use DS5-USBIP; edge cases
 * like use in full screen games." And the one place to look is whether DS5-USBIP is on:
 * on, the bridged controllers are its; off, Desktop Mode or plain controllers.
 *   - IT STARTS BY ITSELF at a stream's start, not a resume, when DS5-USBIP has not
 *     answered 3 s after the stream plays, with its list over the screen ("on a Stream
 *     Start(not a resume) if DS5-USBIP is not on, Desktop Mode starts automatically
 *     with legend up"). Settings > USB Bridge says whether it does
 *     (bridge_settings.h, desktop_mode_start, on by default). It is not saved: every
 *     stream's start begins with it off, and a resume keeps it as it was.
 *   - ⭐ IT HANDS OVER TO DS5-USBIP when DS5-USBIP comes up during the stream and Auto
 *     Bridge runs, at the stream's start or as DS5-USBIP comes back, "even if there is
 *     nothing to auto bridge" (input/auto_bridge.c): its bridges ask DS5-USBIP for
 *     default_windows_config, its config with these same rebinds ("The transition
 *     between aurora-tv managing controllers and DS5-USBIP managing controllers needs
 *     to be seemless"), and once they are made it turns off without a word. Its
 *     warning at a bridge is not raised meanwhile. ⓘ A DS5-USBIP that does not know
 *     the request links each bridge as usual.
 *   - Turned on by hand with DS5-USBIP on, "the case I allow but it is rare", it takes
 *     the bridged controllers too, and says so (below).
 *
 * WHERE IT IS TURNED ON AND OFF: the overlay's Desktop Mode button, which took the place
 * of Full keyboard and Virtual Mouse (ui/streaming/bridge_overlay.c); Options held on a
 * controller, after its question (stream/input/lock_keys.h); the two pop-ups (ui/streaming/bridge_prompt.h): its warning when
 * a controller is bridged with it on, and a refusal with DS5-USBIP offline, which offers
 * to turn it on; and by itself, as above.
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

/* Is it on, in this stream? */
bool desktop_mode_on(void);

/* Switched on or off BY HAND, with how in the log: it no longer starts by itself in this
 * stream. On also turns Virtual Mouse off. Says nothing on the screen: the caller says
 * it (bridge_prompt_say_desktop_on). */
void desktop_mode_set(bool on, const char *how);

/* Once a pass of the app's loop: off at each stream's start, and started by itself
 * when DS5-USBIP is not on (the header). */
void desktop_mode_tick(app_t *app);

/* ⭐ THE HANDOVER TO DS5-USBIP (the header), run by input/auto_bridge.c: begun as its
 * bridges go out asking for default_windows_config, and ended once they are made, which
 * turns Desktop Mode off and the cursor above away. `handing_over`: its warning at a
 * bridge is not raised meanwhile (ui/streaming/bridge_prompt.c). */
void desktop_mode_handover_begin(void);
bool desktop_mode_handing_over(void);
void desktop_mode_handover_end(const char *why);

/* ⭐ THE TV CURSOR BY L3, at a lock and a prompt as well (rhoquinn8217, 2026-11-12:
 * "Lets move the cursor->alignment button to L3"; Triangle's until then, 2026-11-11:
 * "It will toggle cursor on/off but will allow cursor alignment when turned on",
 * stream/input/lock_keys.h). Off: on, and the alignment bar up
 * (ui/streaming/cursor_align.h), where the d-pad moves its tip, Cross keeps it and
 * Circle puts back what was there, the cursor staying on ("on cancel cursor stays up").
 * On: off. The same saved switch as Ctrl+Alt+Shift+M (input/tv_cursor.h). */
void desktop_mode_cursor_toggle(void);

