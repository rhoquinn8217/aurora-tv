/* The TV's pointer in Aurora, drawn with its tip on the point the host is told.
 *
 * ⭐ WHY (rhoquinn8217, 2026-10-11, with a photograph of the two cursors side
 * by side): in an absolute-mouse stream the host puts its arrow's tip on the
 * point the TV reports, and webOS draws its teardrop with the tip down and to
 * the right of that point, so the two never met. *"Use the same image of the
 * cursor just shift it up and to the left"*, and *"only for the aurora-tv app
 * and not for anything else on the TV"*.
 *
 * ➡️ So Aurora sets a cursor of its own: webOS's own teardrop, read from the
 * TV as the app starts (nothing of LG's is copied into this app), with its tip
 * as the cursor's hot spot. SDL draws that pixel on the point, which is the
 * point the host's cursor goes to. Every other app keeps webOS's pointer.
 *
 * ⓘ A set without the file, or an SDL that refuses the cursor, keeps webOS's
 * own pointer, and logs/tv-cursor.log says which. */

#ifndef TV_CURSOR_H
#define TV_CURSOR_H

/* Once the window is up. */
void tv_cursor_init(void);

/* Before SDL's video goes. */
void tv_cursor_deinit(void);

#endif /* TV_CURSOR_H */
