/* The TV's Bluetooth controllers, from Aurora. The why is in the header. */

#include "bt_pairing.h"

#if defined(TARGET_WEBOS)

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <SDL.h>
#include "lunasynccall.h"   /* the TV's Bluetooth controller service */

/* What happened, where it can be read over SSH (tv_cursor.c says why). */
#define BT_PAIRING_LOG "bt-pairing.log"
FILE *ctm_log_open(const char *name, const char *mode);

#define BT_SERVICE "luna://com.webos.service.bthidmanager/"
#define BT_WAIT_MS 5000

/* Only the service's own methods, so the control port cannot be pointed at
 * anything else on the TV. */
static const char *const s_methods[] = {
        "getPairedDeviceLists", "subscribeDevicesStatus", "requestDiscovery", "subscribeDiscovery",
        "cancelDiscovery", "requestConnect", "requestDelete", "requestDisconnect",
};

static void bt_log(const char *fmt, ...) {
    FILE *f = ctm_log_open(BT_PAIRING_LOG, "a");
    if (f == NULL) {
        return;
    }
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    fprintf(f, "%lld.%03ld ", (long long) ts.tv_sec, ts.tv_nsec / 1000000L);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

/* One call, made on a thread of its own so the caller can stop waiting. The
 * call is not cancelled then: whichever side is last to let go frees it. */
typedef struct {
    char uri[96];
    char *payload;
    char *reply;
    bool called;
    bool done;
    bool abandoned;
    SDL_mutex *mutex;
    SDL_cond *cond;
} bt_call_t;

static void call_free(bt_call_t *call) {
    SDL_DestroyCond(call->cond);
    SDL_DestroyMutex(call->mutex);
    free(call->payload);
    free(call->reply);
    free(call);
}

static int call_thread(void *data) {
    bt_call_t *call = data;
    char *reply = NULL;
    const bool called = HLunaServiceCallSync(call->uri, call->payload, true, &reply);
    SDL_LockMutex(call->mutex);
    call->called = called;
    call->reply = reply;
    call->done = true;
    const bool abandoned = call->abandoned;
    SDL_CondSignal(call->cond);
    SDL_UnlockMutex(call->mutex);
    if (abandoned) {
        bt_log("late answer to %s: %s", call->uri, reply != NULL ? reply : "(none)");
        call_free(call);
    }
    return 0;
}

char *bt_pairing_call(const char *method, const char *payload) {
    bool known = false;
    for (size_t i = 0; i < sizeof s_methods / sizeof s_methods[0]; i++) {
        if (method != NULL && strcmp(method, s_methods[i]) == 0) {
            known = true;
            break;
        }
    }
    if (!known) {
        return NULL;
    }
    if (payload == NULL || payload[0] == '\0') {
        payload = "{}";
    }
    bt_call_t *call = calloc(1, sizeof *call);
    if (call == NULL) {
        return NULL;
    }
    snprintf(call->uri, sizeof call->uri, BT_SERVICE "%s", method);
    call->payload = strdup(payload);
    call->mutex = SDL_CreateMutex();
    call->cond = SDL_CreateCond();
    if (call->payload == NULL || call->mutex == NULL || call->cond == NULL) {
        call_free(call);
        return NULL;
    }
    SDL_Thread *thread = SDL_CreateThread(call_thread, "bt-pairing", call);
    if (thread == NULL) {
        bt_log("%s %s: no thread for the call", method, payload);
        call_free(call);
        return NULL;
    }
    SDL_DetachThread(thread);

    const Uint32 deadline = SDL_GetTicks() + BT_WAIT_MS;
    SDL_LockMutex(call->mutex);
    while (!call->done) {
        const Uint32 now = SDL_GetTicks();
        if (SDL_TICKS_PASSED(now, deadline)) {
            break;
        }
        SDL_CondWaitTimeout(call->cond, call->mutex, deadline - now);
    }
    if (!call->done) {
        call->abandoned = true;
        SDL_UnlockMutex(call->mutex);
        bt_log("%s %s: no answer in %d ms", method, call->payload, BT_WAIT_MS);
        return NULL;
    }
    SDL_UnlockMutex(call->mutex);

    char *reply = NULL;
    if (call->called && call->reply != NULL) {
        reply = call->reply;
        call->reply = NULL;
    }
    bt_log("%s %s -> %s", method, call->payload, reply != NULL ? reply : "(the call could not be made)");
    call_free(call);
    return reply;
}

#else

char *bt_pairing_call(const char *method, const char *payload) {
    (void) method;
    (void) payload;
    return NULL;
}

#endif
