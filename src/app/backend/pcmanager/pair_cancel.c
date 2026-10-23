/* A pairing can be cancelled. The why is in the header. */

#include "pair_cancel.h"

#include <stdint.h>

#include <SDL.h>

/* One lock for all of it: the main thread begins, answers and ends; the
 * pairing worker watches and unwatches. Tokens count from 1, so 0 is none,
 * and a caller's own userdata (a pointer) never matches one. */
static SDL_SpinLock s_lock;
static uintptr_t s_newest;          /* the newest pairing's token */
static void *s_waiting;             /* its dialog, until answered or gone */
static uintptr_t s_cancelled;       /* a token whose dialog went before its answer */
static GS_CLIENT s_client;          /* the request in flight */
static uintptr_t s_client_token;    /* and the token it was started with */

void *pair_cancel_begin(void *dialog) {
    SDL_AtomicLock(&s_lock);
    s_newest++;
    s_waiting = dialog;
    void *token = (void *) s_newest;
    SDL_AtomicUnlock(&s_lock);
    return token;
}

void *pair_cancel_answer(void *userdata) {
    void *dialog = NULL;
    SDL_AtomicLock(&s_lock);
    /* An older pairing's answer, after a newer one began, is no one's. */
    if ((uintptr_t) userdata == s_newest) {
        dialog = s_waiting;
        s_waiting = NULL;
    }
    SDL_AtomicUnlock(&s_lock);
    return dialog;
}

void pair_cancel_end(void *dialog) {
    if (dialog == NULL) {
        return;
    }
    SDL_AtomicLock(&s_lock);
    if (dialog == s_waiting) {
        s_waiting = NULL;
        s_cancelled = s_newest;
        if (s_client != NULL && s_client_token == s_newest) {
            gs_abort(s_client);
        }
    }
    SDL_AtomicUnlock(&s_lock);
}

void pair_cancel_watch(GS_CLIENT client, void *userdata) {
    SDL_AtomicLock(&s_lock);
    s_client = client;
    s_client_token = (uintptr_t) userdata;
    /* Cancelled before its request began: it gives up at once. */
    if (s_client_token != 0 && s_client_token == s_cancelled) {
        gs_abort(client);
    }
    SDL_AtomicUnlock(&s_lock);
}

void pair_cancel_unwatch(GS_CLIENT client) {
    SDL_AtomicLock(&s_lock);
    if (s_client == client) {
        s_client = NULL;
        s_client_token = 0;
    }
    SDL_AtomicUnlock(&s_lock);
}
