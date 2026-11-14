/* ⭐⭐ WINDOW SELECT: L1 AND R1 IN DESKTOP MODE (rhoquinn8217, 2026-11-11). Windows' own
 * window picker, the one Alt+Tab opens, kept open by holding Alt while L1 and R1 step
 * through it: "If L1 is pressed hold alt if it's not already held down and press tab /
 * If R1 is pressed hold alt if it's not already held down and pres shift+tab", then,
 * the direction made plain, "L1 move back an R1 move forward". So L1 is Alt+Shift+Tab
 * and R1 Alt+Tab, Alt pressed only by the first of them.
 *
 * ⭐ ALT IS LET GO BY ANYTHING ELSE PRESSED: "Release alt if any other button or keyboard
 * key is pressed except for R1, L1, d-pad, tab, shift and keyboard arrow keys". Create
 * is the pad's Tab, so it keeps Alt held as Tab does. Their answers on the rest:
 *   - Cross picks the window: Alt is let go, and no Enter goes with it ("2. yes");
 *   - Circle cancels: Esc while Alt is still held, which leaves the window you were in,
 *     then Alt is let go ("2. yes");
 *   - a click goes through first and Alt after it, so a window in the picker can be
 *     clicked ("3. click first"): the triggers, the touchpad and a mouse;
 *   - any other button lets go of Alt, then does its own job.
 * And so that Alt is never left down on the PC: Desktop Mode ending, a lock or a User
 * Account Control prompt taking over, the overlay or the keyboard taking the input, and
 * the stream ending (stream/input/lock_keys.c).
 *
 * ⓘ On webOS a keyboard's keys come from its own reading thread (platform/webos/
 * keyboard_evdev.c), so whether Alt is held is atomic and it is let go exactly once; the
 * log line for that one waits for the app's loop (window_select_tick). */

#pragma once

#include <stdbool.h>

struct stream_input_t;

/* L1 (back) or R1: Alt down if it is not already, then Shift+Tab or Tab. */
void window_select_step(struct stream_input_t *input, bool back);

/* Is Alt held down by it? */
bool window_select_held(void);

/* Does this key leave Alt held: Tab, Shift or an arrow? */
bool window_select_keeps(short vk);

/* Alt let go now if it is held, with why in the log. Without a stream, only forgotten. */
void window_select_let_go(struct stream_input_t *input, const char *why);

/* Esc while Alt is still held (the picker closes on the window you were in), then Alt let
 * go. */
void window_select_cancel(struct stream_input_t *input);

/* A keyboard's key on its way to the PC, going down: Alt is let go first, unless the key
 * is Tab, Shift or an arrow. Any thread. */
void window_select_before_key(struct stream_input_t *input, short vk);

/* A click's release has just gone to the PC: Alt is let go after it. */
void window_select_after_click(void);

/* Once a pass of the app's loop: the log line of a let-go from another thread. */
void window_select_tick(void);
