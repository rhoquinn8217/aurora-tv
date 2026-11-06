#include "session_evmouse.h"
#include "evmouse.h"
#include "stream/session_priv.h"

#include <errno.h>
#include <SDL_timer.h>

#include "logging.h"

static void mouse_listener(const evmouse_event_t *event, void *userdata);

static int mouse_worker(session_evmouse_t *mouse);

static void set_evmouse(session_evmouse_t *mouse, evmouse_t *dev);

void session_evmouse_init(session_evmouse_t *mouse, session_t *session) {
    mouse->session = session;
    mouse->lock = SDL_CreateMutex();
    mouse->cond = SDL_CreateCond();
    mouse->disabled = SDL_FALSE;
    /* This fork: the stop's latch and its two flags (session_evmouse.h). */
    mouse->stopped = SDL_FALSE;
    mouse->ready = SDL_FALSE;
    SDL_AtomicSet(&mouse->exited, 0);
    mouse->thread = SDL_CreateThread((SDL_ThreadFunction) mouse_worker, "sessinput", mouse);
}

void session_evmouse_deinit(session_evmouse_t *mouse) {
    if (mouse->thread != NULL) {
        // Defensive: make sure the worker is told to stop before joining it;
        // the stopped latch makes a repeated interrupt harmless.
        // This fork: said again until the worker has gone (2026-10-23): the latch is
        // read before evmouse_listen() marks itself listening, and a stop that
        // lands in between is overwritten there, so the listen loop ran on for
        // good and this join hung the main thread.
        while (!SDL_AtomicGet(&mouse->exited)) {
            session_evmouse_interrupt(mouse);
            SDL_Delay(5);
        }
        SDL_WaitThread(mouse->thread, NULL);
        mouse->thread = NULL;
    }
    SDL_DestroyMutex(mouse->lock);
    mouse->lock = NULL;
    SDL_DestroyCond(mouse->cond);
    mouse->cond = NULL;
}

void session_evmouse_wait_ready(session_evmouse_t *mouse) {
    SDL_LockMutex(mouse->lock);
    // This fork: ready (not dev), so a failed open doesn't wait forever
    while (!mouse->ready) {
        SDL_CondWait(mouse->cond, mouse->lock);
    }
    SDL_UnlockMutex(mouse->lock);
}

void session_evmouse_interrupt(session_evmouse_t *mouse) {
    SDL_LockMutex(mouse->lock);
    // This fork: dev may be NULL when the worker is still opening the device
    // (or already exited); the stopped latch makes the request stick in either
    // case.
    mouse->stopped = SDL_TRUE;
    if (mouse->dev != NULL) {
        evmouse_interrupt(mouse->dev);
    }
    SDL_UnlockMutex(mouse->lock);
}

void session_evmouse_disable(session_evmouse_t *mouse) {
    SDL_LockMutex(mouse->lock);
    if (!mouse->disabled) {
        commons_log_info("Session", "EvMouse disable input");
        mouse->disabled = SDL_TRUE;
    }
    SDL_UnlockMutex(mouse->lock);
}

void session_evmouse_enable(session_evmouse_t *mouse) {
    SDL_LockMutex(mouse->lock);
    if (mouse->disabled) {
        commons_log_info("Session", "EvMouse enable input");
        mouse->disabled = SDL_FALSE;
    }
    SDL_UnlockMutex(mouse->lock);
}

static int mouse_worker(session_evmouse_t *mouse) {
    evmouse_t *dev = evmouse_open_default();
    // This fork: a stop asked while the device was being opened must not be lost
    // (evmouse_listen would overwrite an interrupt issued before it starts):
    // publish the open result and read the latch in one critical section, and
    // skip listening entirely when a stop already came in.
    SDL_LockMutex(mouse->lock);
    mouse->dev = dev;
    mouse->ready = SDL_TRUE;
    SDL_bool stopped = mouse->stopped;
    SDL_CondSignal(mouse->cond);
    SDL_UnlockMutex(mouse->lock);
    if (dev == NULL) {
        commons_log_warn("Session", "No mouse device available");
        SDL_AtomicSet(&mouse->exited, 1);
        return ENODEV;
    }
    if (!stopped) {
        commons_log_info("Session", "EvMouse opened");
        evmouse_listen(dev, mouse_listener, mouse);
    }
    set_evmouse(mouse, NULL);
    evmouse_close(dev);
    commons_log_info("Session", "EvMouse closed");
    SDL_AtomicSet(&mouse->exited, 1);
    return 0;
}

static void set_evmouse(session_evmouse_t *mouse, evmouse_t *dev) {
    SDL_LockMutex(mouse->lock);
    mouse->dev = dev;
    SDL_CondSignal(mouse->cond);
    SDL_UnlockMutex(mouse->lock);
}

static void mouse_listener(const evmouse_event_t *event, void *userdata) {
    session_evmouse_t *mouse = userdata;
    SDL_LockMutex(mouse->lock);
    session_t *session = mouse->session;
    if (!session_accepting_input(session) || mouse->disabled) {
        SDL_UnlockMutex(mouse->lock);
        return;
    }
    switch (event->type) {
        case SDL_MOUSEBUTTONDOWN:
        case SDL_MOUSEBUTTONUP: {
            commons_log_info("Session", "Mouse button %d %s", event->button.button,
                             event->type == SDL_MOUSEBUTTONDOWN ? "down" : "up");
            stream_input_handle_mbutton(&session->input, &event->button);
            break;
        }
        case SDL_MOUSEMOTION: {
            stream_input_handle_mmotion(&session->input, &event->motion, true);
            break;
        }
        case SDL_MOUSEWHEEL: {
            stream_input_handle_mwheel(&session->input, &event->wheel);
            break;
        }
    }
    SDL_UnlockMutex(mouse->lock);
}