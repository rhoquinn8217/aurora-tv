/* A pairing can be cancelled: the window showing its PIN closes, the TV's
 * request to the host gives up at once, and its answer, when it comes, finds
 * nothing.
 *
 * ⭐ WHY (rhoquinn8217, 2026-10-22): "when aurora-tv provide the 4 digit
 * pairing, it won't let me cancel out of it. Add a cancel button so that I
 * can." Upstream's window hid its only button until pairing ended, and Back
 * closes a window only through a shown button (lv_theme_moonlight.c,
 * msgbox_cancel), so nothing closed it while the host waited for the PIN.
 *
 * ⚠️ THE REQUEST HAS NO READ TIMEOUT, ON PURPOSE: the host answers it only once
 * the PIN is typed (libgamestream, gs_pair). A window closed on its own would
 * leave it waiting for ever, holding a worker and the host's pairing, and a
 * pairing started again would race it. So the request is cut short (gs_abort),
 * within a second.
 *
 * ⓘ The answer reaches the window through a token, never the window itself:
 * by then the window may have gone. */

#pragma once

#include "libgamestream/client.h"

/* Main thread: `dialog` waits for the pairing about to start. Returns the
 * userdata to give pcmanager_pair. */
void *pair_cancel_begin(void *dialog);

/* Main thread, in pcmanager_pair's callback, given its userdata: the dialog
 * the pairing was for, or NULL when that dialog has gone and the answer is
 * dropped. */
void *pair_cancel_answer(void *userdata);

/* Main thread: `dialog` is going. A pairing it still waits for is cut short,
 * and its answer will find nothing. */
void pair_cancel_end(void *dialog);

/* The pairing worker, around its request: the client a Cancel cuts short, and
 * the userdata its pairing was started with. */
void pair_cancel_watch(GS_CLIENT client, void *userdata);

void pair_cancel_unwatch(GS_CLIENT client);
