/* The TV's pointer in Aurora, drawn with its tip on the point the host is told.
 *
 * ⭐ WHY (rhoquinn8217, 2026-10-11, with a photograph of the two cursors side
 * by side): in an absolute-mouse stream the host puts its arrow's tip on the
 * point the TV reports, and webOS draws its teardrop with the tip down and to
 * the right of that point, so the two never met. *"Use the same image of the
 * cursor just shift it up and to the left"*, and *"only for the aurora-tv app
 * and not for anything else on the TV"*.
 *
 * ➡️ So Aurora sets a cursor of its own: webOS's teardrop, drawn by Aurora in
 * the same shape with a sharp tip, a thinner line and nothing inside (the
 * later asks, in tv_cursor.c), with its tip as the cursor's hot spot. SDL
 * draws that pixel on the point, which is the point the host's cursor goes
 * to. Every other app keeps webOS's pointer.
 *
 * ⭐ WHERE THE TIP SITS IS KEPT PER TV. The bar Ctrl+Alt+Shift+A opens
 * (ui/streaming/cursor_align.h) moves it a TV pixel at a time and saves it in
 * the TV's own settings; a TV that was never aligned uses -1, 3, which
 * rhoquinn8217 found by eye on the C1 and which looked right on the B4 too.
 *
 * ⭐ AND IT CAN BE SWITCHED OFF, for games (rhoquinn8217, 2026-10-11: "have the
 * tv cursor present does not work with games were you can continously move the
 * mouse even at the edge of the screen"). Off is upstream's relative mouse:
 * the host is told only how the mouse moved, so nothing stops at the screen's
 * edge, and the TV's cursor goes, since nothing then puts it where the host's
 * is. ⓘ The two go together: the cursors meet only while the host is told the
 * point, and that is exactly what stops at the edge.
 *
 * ⭐ TWO CURSORS, SWITCHED BY Ctrl+Alt+Shift+C (rhoquinn8217, 2026-10-12: "I
 * want a chord that toggles between my small sharp tv cursor and the regular
 * cursor. The regular cursor should be adjustable as well"): the drop Aurora
 * draws, and the regular one, webOS's own picture read from the TV as webOS
 * draws it, at the size the TV's Pointer Size setting names ("use whatever is
 * set for the TV"). Each keeps its own alignment ("yes"), and which one shows
 * is kept as the on and off are. ⓘ C is upstream's "show the mouse", which
 * does nothing else in this app, and the host answers only N and F1 to F13.
 *
 * ⓘ An SDL that refuses the cursor keeps webOS's own pointer, and
 * logs/tv-cursor.log says so. */

#ifndef TV_CURSOR_H
#define TV_CURSOR_H

#include <stdbool.h>
#include <stddef.h>

#include <SDL.h>

/* The furthest either cursor can be shifted either way, in the TV's own
 * pixels. */
#define TV_CURSOR_SHIFT_MAX 16

/* Once the window is up: its size against the panel's says how much the TV
 * enlarges the app, and the pointer is shrunk by as much. Which cursor, and
 * each one's shift, are this TV's, from its settings (bridge_settings.h). */
void tv_cursor_init(SDL_Window *window);

/* Before SDL's video goes. */
void tv_cursor_deinit(void);

/* Once per pass of the app's loop: puts Aurora's cursor back when the window
 * has the screen again, in case webOS reset it while it did not. */
void tv_cursor_tick(void);

/* Live, and not saved: shift the cursor showing this many of the TV's own
 * pixels to the right and down of its tip, up to TV_CURSOR_SHIFT_MAX either
 * way, and draw the drop's line 3 less `thin` of them wide (below 0: keep what
 * stands). False when there is no cursor of Aurora's to change. From the
 * control port (`cursor`) and from the Align bar, which saves it itself. */
bool tv_cursor_adjust(int shift_x, int shift_y, int thin);

/* The shift that stands now for the cursor showing, in the TV's own pixels,
 * right and down. */
void tv_cursor_get_shift(int *shift_x, int *shift_y);

/* Saves the shift that stands now as this TV's for the cursor showing, at
 * once. From the align bar's Enter or A. */
bool tv_cursor_keep(void);

/* Which cursor shows: true for the regular one, webOS's own picture; false for
 * the drop Aurora draws. */
bool tv_cursor_regular(void);

/* Show the regular cursor or the drop, with its own shift, and keep the choice
 * at once. False when that cursor cannot be made, and the one that stood
 * stays. ⓘ From Ctrl+Alt+Shift+C on every keyboard, and from the control port
 * (`cursor regular`, `cursor sharp`). */
bool tv_cursor_set_look(bool regular);
bool tv_cursor_toggle_look(void);

/* The cursor's picture as it shows now, red, green, blue and alpha bytes, and
 * where its tip is drawn in it; NULL when there is none of Aurora's.
 * `generation` changes each time it is made again (its look switched, a shift
 * kept). For the touchpad's pointer, which Aurora draws itself in the same
 * look (input/touchpad_pointer.h). */
const SDL_Surface *tv_cursor_picture(int *tip_x, int *tip_y, unsigned *generation);

/* Is the cursor Aurora's own? False where SDL refused it and webOS's own
 * pointer stays: there is nothing to shift. */
bool tv_cursor_drawn(void);

/* Is the TV cursor on: upstream's "Absolute mouse mode", and not "Use mouse
 * hardware", which takes the mouse past SDL and has no cursor to show. */
bool tv_cursor_mode_on(void);

/* Can it be switched here at all? Not with "Use mouse hardware" on: Settings
 * greys out absolute mouse mode then, for the same reason. */
bool tv_cursor_mode_switchable(void);

/* Switch the TV cursor on or off, save it, and if the stream has the mouse
 * right now, give it the new mode at once. False when it cannot be switched:
 * with "Use mouse hardware" on (tv_cursor_mode_switchable), or on while a
 * mouse is bridged (input/mouse_seen.h), which a message on the stream says.
 * ⓘ From Ctrl+Alt+Shift+M, Moonlight's own shortcut for this switch, on every
 * keyboard; from the pop-up a bridged mouse raises (bridge_prompt.h); and from
 * the control port. */
bool tv_cursor_set_mode(bool on);
bool tv_cursor_toggle_mode(void);

/* One line on the cursor that stands now. */
void tv_cursor_describe(char *out, size_t len);

/* A line in logs/tv-cursor.log, where a set's developer account can read it:
 * the cursor's own, and the align bar's. */
void tv_cursor_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#endif /* TV_CURSOR_H */
