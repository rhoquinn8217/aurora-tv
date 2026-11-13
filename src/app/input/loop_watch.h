/* ⓘ A DIAGNOSTIC (rhoquinn8217, 2026-11-13, at the C3: with the TV cursor's alignment
 * bar up, "aligning is slow and keyboard and controller inputs are unresponsive or very
 * slow", and "It wasn't slow before"). Measured from outside, the app's loop stopped for
 * about four seconds at a time while using no processor: it waited on something. Each
 * pass of the loop is timed by its steps here, and a pass that takes longer than
 * LOOP_WATCH_SLOW_MS is written to logs/loop-watch.log with its three slowest steps, so
 * the step that waits names itself. A clock read per step; nothing written otherwise. */

#pragma once

/* At the start of a pass of the app's loop. */
void loop_watch_begin(void);

/* The time since the last mark, or the pass's start, was `step`'s. */
void loop_watch_mark(const char *step);

/* At the end of the pass: written to the log if it was slow. */
void loop_watch_end(void);
