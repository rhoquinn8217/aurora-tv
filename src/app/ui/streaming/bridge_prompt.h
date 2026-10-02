/* The pop-up a bridge raises when the TV's own mouse controls are on.
 *
 * ⭐ WHY IT EXISTS. A bridged controller reaches the host as itself, and the TV
 * goes on reading it. Virtual Mouse turns its sticks and triggers into a host
 * mouse, and the touchpad's mouse mode sends its click as a mouse click, so one
 * press arrives as a controller and as a mouse. One switch turns the TV's own
 * handling off for the stream (input/bridge_override.h), and this is where a
 * person is offered it: at the moment they bridge a controller.
 *
 * It warns, offers the fix and its refusal as two buttons, counts, and goes
 * away by itself:
 *
 *     Warning: Turn off Virtual Mouse to prevent binding conflicts with
 *     bridged controllers.
 *                      [ Turn off Virtual Mouse ]  [ Leave it on (10) ]
 *
 * (rhoquinn8217's wording, 2026-10-01, their second: the first was two
 * sentences and a question, with a button that said OK.) The first button
 * switches it off. The second leaves everything as it was, and so do Circle,
 * Back or Escape, and the count running out. With the touchpad's mouse mode on
 * instead, or as well, the words name that.
 *
 * ⭐ THE COUNT IS ON THE BUTTON THE COUNT TAKES. A count on a button reads as
 * "this happens at zero", and at zero it is left on. It had one button for a
 * few builds, "Turn off Virtual Mouse (10)": the count sat on the one thing
 * that did not happen at zero, and nothing on the screen said how to say no.
 *
 * ⓘ IT SITS BOTTOM LEFT, CLEAR OF THE EDGE AND ABOVE THE OVERLAY'S ROW OF
 * BUTTONS, AND IT IS ALL THERE IS. It is in that one place whether the overlay
 * is open or not: a bridge from the USB Bridge panel raises it with the
 * overlay open, and lower down it lay across the overlay's buttons.
 *
 * ⭐ ITS FIRST BUTTON IS ANSWERED, IN THE SAME PLACE, AND NOTHING ELSE IS:
 *
 *     Virtual Mouse has turned off. Toggle it back on in the streaming overlay.
 *
 * (rhoquinn8217's wording, 2026-10-01.) Only that button raises it. The second
 * button, the count running out and a dismissal change nothing and say
 * nothing, and nothing is said when Virtual Mouse brings the TV's controls
 * back. It takes no input,
 * goes by itself, and goes at once if what it says stops being true.
 * ⓘ The history: build 460 had a notice at the top left listing everything
 * that had gone off, and another when the Input settings applied again. Both
 * were taken out the same day, "for now", to have the pop-up alone, and this
 * one came back in the pop-up's place with its own words.
 *
 * ⓘ IT IS WIDE, SHORT AND SEE-THROUGH, because it shares the picture with a
 * game: as wide as its one sentence on one line, so a line of text and a row
 * of buttons are all its height, in the small type, with the picture showing
 * through its background. The picture behind it is dimmed while it is up, as
 * behind any dialogue here, and that dim is what says the pop-up has the
 * input. ⛔ So nothing may climb back over the dim while it is up: the USB
 * Bridge panel did, and now waits behind a dialogue (ctm_panel.c).
 *
 * ⛔ IT MUST NOT NEED ANYONE. This was a two-button dialog once, and became a
 * notice that fades because Auto Bridge takes several devices as a stream
 * starts, with nobody there to answer. The count is what lets it be a question
 * again: unanswered, it is gone in ten seconds and nothing has changed. A
 * second controller bridged while it is up restarts the count rather than
 * raising a second one.
 *
 * ⛔ WHILE IT IS UP THE INTERFACE HAS THE INPUT, as with the overlay. A bridged
 * controller's presses are held from the game, and the stream's own keyboard,
 * mouse and controllers go to the pop-up, or the Cross that answers it would be
 * a Cross in the game as well. That is three places asking whether it is up:
 * ui_should_block_input() in ui/root.c, and the two ticks in app.c.
 *
 * ⓘ IT NEVER SAYS THE SWITCH'S NAME. To a person it turns Virtual Mouse off,
 * and turning Virtual Mouse on again, from the overlay, is how it comes back.
 * The switch had a button of its own beside USB Bridge until 2026-10-01. */

#ifndef BRIDGE_PROMPT_H
#define BRIDGE_PROMPT_H

#include <stdbool.h>

/* A controller has just been bridged. Ask, if the TV's own mouse controls are
 * on and have not been switched off already; restart the count if it is up.
 * Does nothing outside a stream. */
void bridge_prompt_request(void);

/* Is it up? While it is, the interface has the input. */
bool bridge_prompt_shown(void);

/* Close it and change nothing: the overlay is opening over it. */
void bridge_prompt_dismiss(void);

/* The stream is ending: the pop-up goes, unanswered, and so does the message a
 * pressed button left on the screen. */
void bridge_prompt_stream_ended(void);

/* For the control port: the seconds left on the count, 0 when it is not up;
 * and its first button, the one that turns it off, pressed without a hand.
 * False when it is not up. ⓘ Its second button does what a dismissal does. */
int bridge_prompt_seconds_left(void);

bool bridge_prompt_accept(void);

/* For the control port: where it sits and how big it is, in pixels, as it is
 * laid out now, with the picture's own size beside it and how many lines the
 * warning takes. ⓘ Its look cannot be checked from a terminal; its shape can,
 * and "the warning is on one line, 60 pixels in from the corner" is the part
 * of the look that a number settles. False when it is not up. */
typedef struct {
    int x, y, width, height;
    int screen_width, screen_height;
    /* The top of the overlay's row of buttons, which it sits above: its
     * bottom edge, y + height, is meant to be short of this. -1 if unknown. */
    int buttons_top;
    int text_lines;
    /* The pop-up only: how wide its row of two buttons is, which of them is
     * selected (0 turns it off, 1 leaves it on, -1 neither), and whether the
     * selected one is drawn highlighted. */
    int row_width;
    int selected;
    bool highlighted;
} bridge_prompt_measure_t;

bool bridge_prompt_measure(bridge_prompt_measure_t *out);

/* The same for the message a pressed button leaves. Returns the milliseconds
 * it has left, or -1 when it is not on the screen. */
int bridge_prompt_note_measure(bridge_prompt_measure_t *out);

#endif /* BRIDGE_PROMPT_H */
