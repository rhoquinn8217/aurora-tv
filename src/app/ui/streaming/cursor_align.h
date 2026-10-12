/* The bar that aligns the TV cursor with the PC's, opened by Ctrl+Alt+Shift+A.
 *
 * ⭐ WHY (rhoquinn8217, 2026-10-11): the TV's cursor and the PC's sit apart by
 * a few of the TV's pixels, and how far is the TV's own business, so each TV
 * is aligned by eye and keeps its own. It was the Align row of a Mouse menu in
 * the overlay for one build, and then: "I want to remove the mouse overlay
 * button. Instead a want a chord that will open up the tv cursor calibrator."
 * The chord is Ctrl+Alt+Shift+A ("yes"), from a keyboard the TV holds, one
 * webOS hands over, and a bridged one, which the bridge core watches for it
 * (its kbd_chord.inl).
 *
 * - **The bar** sits at the top of the stream. The host's cursor first goes to
 *   the TV cursor's tip, so the two start together; the mouse still moves
 *   both, and its clicks go nowhere. Each arrow or d-pad press moves the TV
 *   cursor one of the TV's own pixels; Enter or A keeps it as this TV's, Esc
 *   or B puts back what was there.
 * - **While it is up it has the input**, as the overlay does (ui/root.c), and
 *   a bridged device is held (bridge_app.c), so a bridged keyboard's arrows
 *   reach it. ⭐ And a bridged keyboard is taken from webOS for as long, which
 *   the overlay does not need: webOS hides its pointer at every key it reads
 *   (rhoquinn8217: "align is unusable because the cursor disappears whenever
 *   I click an arrow key"). input/bridge_keyboard.h says how it is given back.
 * - ⭐ **THE TV CURSOR STAYS UP WHILE IT IS** (rhoquinn8217: "When calibrating,
 *   the tv mouse cursor disappears after sometime"). webOS hides a mouse's
 *   pointer after 5 seconds without movement, and keys never count. So while
 *   the bar is up, each mouse the TV has seen move is moved one pixel and
 *   back, once a second and after every step, through its own input node
 *   (mouse_seen_wake, input/mouse_seen.h, which says what failed first).
 * - **With the TV cursor off** the chord does not open it, and says how to turn
 *   the cursor on, in the place of the bridge's pop-up (bridge_prompt.h).
 */

#ifndef CURSOR_ALIGN_H
#define CURSOR_ALIGN_H

#include <stdbool.h>

/* The chord: open the bar, or say why not. Opens on the next pass of the
 * interface, so it may be called from inside the interface's own reading of a
 * key. Nothing outside a stream, or while the interface has the input already. */
void cursor_align_request(void);

/* Is the bar up? While it is, it takes the keys as the overlay does
 * (ui/root.c) and a bridged device is held (bridge_app.c). */
bool cursor_align_shown(void);

/* The stream's screen is going: the bar goes, unsaved, with nothing handed
 * back to a screen that is no longer there. */
void cursor_align_stream_ended(void);

#endif /* CURSOR_ALIGN_H */
