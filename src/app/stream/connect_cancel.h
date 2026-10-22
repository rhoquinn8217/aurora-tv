/* A stream can be cancelled while it connects: the launch request to the host
 * and the connection give up at once, and the stream ends the way a failed
 * start ends (session_worker.c, thread_cleanup, which closes the box), with no
 * error shown. The game is left running on the host.
 *
 * ⭐ WHY (rhoquinn8217, 2026-10-22): "Add a cancel for the connecting spinner
 * as well", and of the game: "leave it running". session_interrupt alone is
 * read only after the launch and the connection have returned, and the launch
 * waits as long as the host does: a PC still signing in to Windows held it
 * about a minute on 2026-10-21.
 *
 * ⚠️ Never on a stream already up: a "Connecting..." box can outlive its
 * screen (the stuck box of 2026-10-21), and pressed over a later stream it
 * must not cut that stream short. */

#pragma once

#include <stdbool.h>

#include "libgamestream/client.h"

typedef struct session_t session_t;

/* The session's worker, once it has made the client its launch request uses. */
void connect_cancel_watch(GS_CLIENT client);

/* The session's worker, at its end, before the client goes. If the user
 * cancelled, the error the cancel itself caused is cleared, so no "Failed to
 * start streaming" window opens. */
void connect_cancel_done(session_t *session, GS_CLIENT client);

/* Main thread: cancels `session` if it is still connecting, and says whether
 * it did. */
bool connect_cancel_now(session_t *session);
