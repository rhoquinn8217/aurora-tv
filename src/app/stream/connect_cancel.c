/* A stream can be cancelled while it connects. The why is in the header. */

#include "connect_cancel.h"

#include <Limelight.h>
#include <SDL.h>

#include "errors.h"
#include "logging.h"
#include "session.h"
#include "session_priv.h"

/* The worker watches and is done; the main thread cancels. */
static SDL_SpinLock s_lock;
static GS_CLIENT s_client;
static bool s_cancelled;

void connect_cancel_watch(GS_CLIENT client) {
    SDL_AtomicLock(&s_lock);
    s_client = client;
    /* Cancelled before the launch request began: it gives up at once. */
    if (s_cancelled) {
        gs_abort(client);
    }
    SDL_AtomicUnlock(&s_lock);
}

void connect_cancel_done(session_t *session, GS_CLIENT client) {
    SDL_AtomicLock(&s_lock);
    const bool cancelled = s_cancelled;
    s_cancelled = false;
    if (s_client == client) {
        s_client = NULL;
    }
    SDL_AtomicUnlock(&s_lock);
    if (cancelled) {
        /* What the cut-short launch or connection reported was the cancel. */
        streaming_error(session, GS_OK, "");
        commons_log_info("Session", "Cancelled by the user: the error above was the cancel, and is not shown");
    }
}

bool connect_cancel_now(session_t *session) {
    if (session == NULL) {
        return false;
    }
    SDL_LockMutex(session->mutex);
    const bool connecting = session->state == STREAMING_CONNECTING;
    SDL_UnlockMutex(session->mutex);
    if (!connecting) {
        return false;
    }
    commons_log_info("Session", "Cancel pressed while connecting: the launch and the connection give up");
    SDL_AtomicLock(&s_lock);
    s_cancelled = true;
    if (s_client != NULL) {
        gs_abort(s_client);
    }
    SDL_AtomicUnlock(&s_lock);
    /* The connection, if it has begun (moonlight-common-c checks this between
     * its steps), and the session, which then ends without quitting the game. */
    LiInterruptConnection();
    session_interrupt(session, false, STREAMING_INTERRUPT_USER);
    return true;
}
