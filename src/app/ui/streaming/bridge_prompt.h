/* The pop-up a bridge raises when the TV's own mouse controls are on.
 *
 * ⭐ WHY IT EXISTS. A bridged controller reaches the host as itself, and the TV
 * goes on reading it. Virtual Mouse turns its sticks and triggers into a host
 * mouse, and the touchpad's mouse mode sends its click as a mouse click, so one
 * press arrives as a controller and as a mouse. One switch turns the TV's own
 * handling off for the stream (input/bridge_override.h), and this is where a
 * person is offered it: at the moment they bridge a controller.
 *
 * It warns, offers the fix on its one button with a count, and goes away by
 * itself:
 *
 *     Warning: Turn off Virtual Mouse to prevent binding conflicts with
 *     bridged controllers.
 *                                         [ Turn off Virtual Mouse (10) ]
 *
 * (rhoquinn8217's wording, 2026-10-01, their second: the first was two
 * sentences and a question, with a button that said OK.) The button switches
 * it off. Circle, Back or Escape, or the count running out, leaves everything
 * as it was. With the touchpad's mouse mode on instead, or as well, the words
 * name that.
 *
 * ⓘ IT SITS BOTTOM LEFT, CLEAR OF THE EDGES, AND IT IS ALL THERE IS. Nothing is
 * shown once it is answered, and nothing when Virtual Mouse brings the TV's
 * controls back. A notice said what had gone off, and another that the Input
 * settings applied again, for one build; rhoquinn8217 took both out the same
 * day, "for now", to have the pop-up alone.
 *
 * ⓘ IT IS WIDE, SHORT AND SEE-THROUGH, because it shares the picture with a
 * game: as wide as its one sentence on one line, so a line of text and a
 * button are all its height, in the small type, with the picture showing
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

/* Close it and change nothing: the stream is ending, or the overlay is opening
 * over it. */
void bridge_prompt_dismiss(void);

/* For the control port: the seconds left on the count, 0 when it is not up;
 * and its button, pressed without a hand. False when it is not up. */
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
    int text_lines;
} bridge_prompt_measure_t;

bool bridge_prompt_measure(bridge_prompt_measure_t *out);

#endif /* BRIDGE_PROMPT_H */
