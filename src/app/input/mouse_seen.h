/* Which mice the TV has seen move, so a bridge can tell a mouse in use from a
 * mouse part with nothing behind it.
 *
 * ⭐ WHY (rhoquinn8217, 2026-10-11): the pop-up that asks to turn the TV cursor
 * off "should appear immediately after a mouse is bridged just like the message
 * when a controller is bridge and virtual mouse is on". But a part that says it
 * is a mouse is not always one: the RONGYUAN keyboard's receiver has a mouse
 * part with no mouse behind it, and bridging that keyboard would ask about a
 * TV cursor its mouse never moved.
 *
 * ➡️ So the app watches every pointer the TV has, without taking any, and
 * remembers which have moved. A mouse part bridged whose input node the TV saw
 * move asks at once (bridge_prompt.h). One the TV never saw move asks at its
 * first use instead, which the bridge core tells: a mouse Auto Bridge takes as
 * a stream starts, before anyone touched it; and a part with no mouse behind
 * it never asks at all.
 *
 * ⓘ Each node is read only until it first moves, then closed: nothing is read
 * from a mouse in use. What moved is forgotten when the input devices change,
 * as a node's number can pass to another device. App's loop only. */

#ifndef MOUSE_SEEN_H
#define MOUSE_SEEN_H

#include <stdbool.h>

#include "ctm_bridge_glue.h"

/* Once per pass of the app's loop: watch the pointers, a short burst of reads
 * each. */
void mouse_seen_tick(void);

/* A part was just bridged: if it is a mouse the TV saw move, the pop-up is due
 * (mouse_seen_take_bridged). */
void mouse_seen_plugged(const ctm_bridge_dev_t *dev);

/* Was a mouse the TV saw move just bridged? True once for each. */
bool mouse_seen_take_bridged(void);

#endif /* MOUSE_SEEN_H */
