#pragma once

#include <SDL_thread.h>

typedef struct session_t session_t;

typedef struct session_evmouse_t {
    session_t *session;
    SDL_mutex *lock;
    SDL_cond *cond;
    SDL_Thread *thread;
    struct evmouse_t *dev;
    SDL_bool disabled;
    /* Latched stop request: set by interrupt/deinit so a worker still inside
     * evmouse_open_default() exits instead of listening forever (the interrupt
     * would otherwise hit a NULL dev and be lost). */
    SDL_bool stopped;
    /* Set once the worker published its open result (dev != NULL on success);
     * wait_ready waits on this instead of dev so a failed open (no mouse
     * device) doesn't block the session forever. */
    SDL_bool ready;
} session_evmouse_t;

void session_evmouse_init(session_evmouse_t *mouse, session_t *session);

void session_evmouse_deinit(session_evmouse_t *mouse);

void session_evmouse_wait_ready(session_evmouse_t *mouse);

void session_evmouse_interrupt(session_evmouse_t *mouse);

void session_evmouse_disable(session_evmouse_t *mouse);

void session_evmouse_enable(session_evmouse_t *mouse);