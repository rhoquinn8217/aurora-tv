/* The stream's pop-ups and messages, all in one place at the bottom left.
 *
 * ⭐⭐ TWO POP-UPS AND TWO MESSAGES, AND NO OTHERS (rhoquinn8217, 2026-11-12, shown
 * every one there was: "we may have to many or they have an inconsistent structure",
 * then "Get rid of all of these and one have this one", "Use only these two" and "you
 * created too many messages about the cursor doesn't need explaining"). Their words:
 *
 *     Warning: Desktop Mode is on. DS5-USBIP configs are disabled when Desktop Mode
 *     is on.
 *                       [ Turn off Desktop Mode ]  [ Leave it on (10) ]
 *
 * when a controller is bridged with Desktop Mode on (ui/streaming/desktop_mode.h), and
 *
 *     Unable to bridge device. DS5-USBIP is offline. Turn on Desktop Mode?
 *                       [ Yes ]  [ No (5) ]
 *
 * when a bridge is refused because DS5-USBIP does not answer, which is also what a PC
 * at its sign-in screen after a restart looks like, with Desktop Mode off. The messages,
 * four seconds each:
 *
 *     Warning: DS5-USBIP configs are disabled when Desktop Mode is on.
 *
 * whenever Desktop Mode is turned on with DS5-USBIP answering, and
 *
 *     Bridge a device to use DS5-USBIP
 *
 * from the overlay's DS5-USBIP button with nothing bridged.
 * ⓘ Gone then: Virtual Mouse's warning at a bridge (Desktop Mode turns it off, and it is
 * being phased out), the TV cursor's at a bridged mouse, Desktop Mode's questions at a
 * stream's start, a refusal at a locked Windows (its lock list says devices bridge after
 * the sign-in), Auto Bridge's waiting messages, and every message about the cursor.
 *
 * ⭐ THE SECOND BUTTON IS SELECTED AND CARRIES THE COUNT. A count on a button reads as
 * "this happens at zero", and at zero nothing changes; and an accidental press then
 * changes nothing either (rhoquinn8217, 2026-11-11: "an accidental press should default
 * to no"). Circle, Back or Escape do what the second button does.
 *
 * ⓘ IT SITS BOTTOM LEFT, CLEAR OF THE EDGE AND ABOVE THE OVERLAY'S ROW OF
 * BUTTONS, AND IT IS ALL THERE IS. It is in that one place whether the overlay
 * is open or not: a bridge from the USB Bridge panel raises it with the
 * overlay open, and lower down it lay across the overlay's buttons.
 *
 * ⓘ IT IS WIDE, SHORT AND SEE-THROUGH, because it shares the picture with a
 * game: as wide as its one sentence on one line, so a line of text and a row
 * of buttons are all its height, in the small type, with the picture showing
 * through its background. The picture behind it is dimmed while it is up, as
 * behind any dialogue here, and that dim is what says the pop-up has the
 * input. ⛔ So nothing may climb back over the dim while it is up: the USB
 * Bridge panel did, and now waits behind a dialogue (ctm_panel.c).
 *
 * ⛔ IT MUST NOT NEED ANYONE. Auto Bridge takes several devices as a stream starts,
 * with nobody there to answer, so it goes by itself when its count runs out and
 * nothing has changed. A second controller bridged while it is up restarts the
 * count rather than raising a second one.
 *
 * ⛔ WHILE IT IS UP THE INTERFACE HAS THE INPUT, as with the overlay. A bridged
 * controller's presses are held from the game, and the stream's own keyboard,
 * mouse and controllers go to the pop-up, or the Cross that answers it would be
 * a Cross in the game as well. That is three places asking whether it is up:
 * ui_should_block_input() in ui/root.c, and the two ticks in app.c. */

#ifndef BRIDGE_PROMPT_H
#define BRIDGE_PROMPT_H

#include <stdbool.h>

/* A controller has just been bridged: Desktop Mode's warning, if it is on, and not
 * while a lock's or a User Account Control prompt's list has the controls; the count
 * restarts if it is up. Does nothing outside a stream. */
void bridge_prompt_request(void);

/* Is it up? While it is, the interface has the input. */
bool bridge_prompt_shown(void);

/* Close it and change nothing: the overlay is opening over it. */
void bridge_prompt_dismiss(void);

/* The stream is ending: the pop-up goes, unanswered, and so does a message. */
void bridge_prompt_stream_ended(void);

/* A message in the pop-up's place for four seconds, replacing one that is up; none
 * while the pop-up is. For the DS5-USBIP button with nothing bridged. */
void bridge_prompt_note(const char *text);

/* Is a message up in the pop-up's place? */
bool bridge_prompt_note_up(void);

/* A bridge somebody asked for was refused: `locked` when DS5-USBIP answered that
 * Windows is locked, otherwise it did not answer. The second offers Desktop Mode while
 * it is off (above); a locked Windows says nothing. */
void bridge_prompt_note_bridge_refused(bool locked);

/* Desktop Mode was just turned on: its message (above), with DS5-USBIP answering.
 * Nothing outside a stream or while a pop-up is up. */
void bridge_prompt_say_desktop_on(void);

/* For the control port: the pop-up's words, "" when it is not up. */
const char *bridge_prompt_words(void);

/* For the control port: the seconds left on the count, 0 when it is not up;
 * and its first button pressed without a hand. False when it is not up.
 * ⓘ Its second button does what a dismissal does. */
int bridge_prompt_seconds_left(void);

bool bridge_prompt_accept(void);

/* For the control port: where it sits and how big it is, in pixels, as it is
 * laid out now, with the picture's own size beside it and how many lines the
 * words take. ⓘ Its look cannot be checked from a terminal; its shape can,
 * and "the words are on one line, 60 pixels in from the corner" is the part
 * of the look that a number settles. False when it is not up. */
typedef struct {
    int x, y, width, height;
    int screen_width, screen_height;
    /* The top of the overlay's row of buttons, which it sits above: its
     * bottom edge, y + height, is meant to be short of this. -1 if unknown. */
    int buttons_top;
    int text_lines;
    /* The pop-up only: how wide its row of two buttons is, which of them is
     * selected (0 the first, 1 the second, -1 neither), and whether the
     * selected one is drawn highlighted. */
    int row_width;
    int selected;
    bool highlighted;
} bridge_prompt_measure_t;

bool bridge_prompt_measure(bridge_prompt_measure_t *out);

/* The same for a message. Returns the milliseconds it has left, or -1 when none
 * is on the screen. */
int bridge_prompt_note_measure(bridge_prompt_measure_t *out);

/* The message's words, for the control port; empty when none is up. */
const char *bridge_prompt_note_words(void);

#endif /* BRIDGE_PROMPT_H */
