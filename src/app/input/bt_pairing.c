/* The TV's Bluetooth controllers, from Aurora. The why is in the header. */

#include "bt_pairing.h"

#include <string.h>

#if defined(TARGET_WEBOS)

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include <sys/types.h>
#include <time.h>

#include <SDL.h>
#include <pbnjson.h>
#include <webos-helpers/libhelpers.h>   /* calls that answer more than once */
#include "lunasynccall.h"   /* the TV's Bluetooth controller service */
#include "lvgl.h"

#include "app.h"
#include "util/bus.h"

/* What happened, where it can be read over SSH (tv_cursor.c says why). */
#define BT_PAIRING_LOG "bt-pairing.log"
/* Started again past this, rather than left to grow: a search restarts every
 * few seconds, and each restart writes a few lines. */
#define BT_PAIRING_LOG_MAX_BYTES (256 * 1024)
FILE *ctm_log_open(const char *name, const char *mode);

#define BT_SERVICE "luna://com.webos.service.bthidmanager/"
#define BT_WAIT_MS 5000
/* How long the switch keeps searching, round after round, before it gives up. */
#define BT_SEARCH_MS (3 * 60 * 1000)
/* How often the TV's search is started afresh while the switch is on. */
#define BT_RESTART_MS 6000

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
    if (fseek(f, 0, SEEK_END) == 0 && ftell(f) > BT_PAIRING_LOG_MAX_BYTES) {
        fclose(f);
        f = ctm_log_open(BT_PAIRING_LOG, "w");
        if (f == NULL) {
            return;
        }
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

/* ---------------------------------------------------------------------------
 * One call, waited for: the control port's, and the worker's.
 * ------------------------------------------------------------------------- */

/* Made on a thread of its own so the caller can stop waiting. The call is not
 * cancelled then: whichever side is last to let go frees it. */
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

/* ---------------------------------------------------------------------------
 * The TV's answers, read.
 * ------------------------------------------------------------------------- */

static jvalue_ref parse_reply(const char *reply, jdomparser_ref *parser) {
    *parser = NULL;
    if (reply == NULL) {
        return NULL;
    }
    JSchemaInfo schema;
    jschema_info_init(&schema, jschema_all(), NULL, NULL);
    *parser = jdomparser_create(&schema, 0);
    if (*parser == NULL || !jdomparser_feed(*parser, reply, (int) strlen(reply)) || !jdomparser_end(*parser)) {
        return NULL;
    }
    return jdomparser_get_result(*parser);
}

/* A string member into `out`, or false when there is none. */
static bool string_member(jvalue_ref object, const char *name, char *out, size_t size) {
    if (!jis_object(object)) {
        return false;
    }
    jvalue_ref v = jobject_get(object, j_cstr_to_buffer(name));
    if (v == NULL || !jis_string(v)) {
        return false;
    }
    raw_buffer buf = jstring_get_fast(v);
    snprintf(out, size, "%.*s", (int) buf.m_len, buf.m_str);
    return true;
}

/* A boolean member, or -1 when there is none. */
static int bool_member(jvalue_ref object, const char *name) {
    if (!jis_object(object)) {
        return -1;
    }
    jvalue_ref v = jobject_get(object, j_cstr_to_buffer(name));
    bool b = false;
    if (v == NULL || !jis_boolean(v) || jboolean_get(v, &b) != CONV_OK) {
        return -1;
    }
    return b ? 1 : 0;
}

/* ---------------------------------------------------------------------------
 * The state, and whom to tell.
 * ------------------------------------------------------------------------- */

static SDL_mutex *s_lock;
static bt_pairing_state_t s_state;
static void (*s_listener)(void);
static SDL_atomic_t s_notify_queued;

/* The device being paired, while it is: one at a time. */
static char s_pairing_addr[24];
static char s_pairing_name[96];
static Uint32 s_search_since;
/* The restart timer, which search it belongs to (a search switched off and on
 * again within a period is a new one), and whether a restart already waits. */
static SDL_TimerID s_restart_timer;
static int s_search_gen;
static bool s_restart_queued;

static void set_status(const char *fmt, ...) {
    char line[sizeof s_state.status];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    SDL_LockMutex(s_lock);
    snprintf(s_state.status, sizeof s_state.status, "%s", line);
    SDL_UnlockMutex(s_lock);
    bt_log("status: %s", line);
}

static void call_listener(void *data) {
    (void) data;
    SDL_AtomicSet(&s_notify_queued, 0);
    if (s_listener != NULL) {
        s_listener();
    }
}

/* ⛔ The bus runs its actions while it holds SDL's event queue, so the panel's
 * rebuild is handed on to LVGL from there, as the control port's commands are. */
static void notify_on_bus(void *data) {
    if (lv_async_call(call_listener, data) != LV_RES_OK) {
        call_listener(data);
    }
}

static void notify(void) {
    if (SDL_AtomicCAS(&s_notify_queued, 0, 1) && global != NULL) {
        if (!app_bus_post(global, notify_on_bus, NULL)) {
            SDL_AtomicSet(&s_notify_queued, 0);
        }
    }
}

/* ---------------------------------------------------------------------------
 * The worker: every call to the TV is made here, in the order asked.
 * ------------------------------------------------------------------------- */

typedef enum {
    JOB_REFRESH,
    JOB_FORGET,        /* addr: the devices with that address, an empty one included */
    JOB_FORGET_ALL,    /* every device in the list */
    JOB_SEARCH_ON,
    JOB_SEARCH_OFF,
    JOB_SEARCH_DONE,   /* paired: the switch goes off, the list is read again */
    JOB_CONNECT,       /* name, addr: one the search found that pairs by itself */
    JOB_PAIR,          /* name, addr: one from the found list, asked for */
    JOB_RESTART,       /* the TV's search afresh: every BT_RESTART_MS, and after a failed pairing */
} job_kind_t;

typedef struct {
    job_kind_t kind;
    char addr[24];
    char name[96];
} job_t;

#define JOB_QUEUE 16
static job_t s_jobs[JOB_QUEUE];
static int s_job_head, s_job_count;
static SDL_sem *s_wake;
static SDL_Thread *s_worker;

static void enqueue(job_kind_t kind, const char *addr, const char *name) {
    SDL_LockMutex(s_lock);
    if (s_job_count < JOB_QUEUE) {
        job_t *job = &s_jobs[(s_job_head + s_job_count) % JOB_QUEUE];
        memset(job, 0, sizeof *job);
        job->kind = kind;
        snprintf(job->addr, sizeof job->addr, "%s", addr != NULL ? addr : "");
        snprintf(job->name, sizeof job->name, "%s", name != NULL ? name : "");
        s_job_count++;
    } else {
        bt_log("queue full: job %d dropped", (int) kind);
    }
    SDL_UnlockMutex(s_lock);
    SDL_SemPost(s_wake);
}

static bool dequeue(job_t *out) {
    SDL_LockMutex(s_lock);
    const bool have = s_job_count > 0;
    if (have) {
        *out = s_jobs[s_job_head];
        s_job_head = (s_job_head + 1) % JOB_QUEUE;
        s_job_count--;
    }
    SDL_UnlockMutex(s_lock);
    return have;
}

/* A call whose answer only matters as returnValue; its errorText into `why`. */
static bool call_ok(const char *method, const char *payload, char *why, size_t why_size) {
    char *reply = bt_pairing_call(method, payload);
    jdomparser_ref parser = NULL;
    jvalue_ref root = parse_reply(reply, &parser);
    const bool ok = bool_member(root, "returnValue") == 1;
    if (why != NULL && why_size > 0) {
        why[0] = '\0';
        if (!ok && !string_member(root, "errorText", why, why_size)) {
            snprintf(why, why_size, "%s", reply == NULL ? "no answer" : "refused");
        }
    }
    if (parser != NULL) {
        jdomparser_release(&parser);
    }
    free(reply);
    return ok;
}

/* ---------------------------------------------------------------------------
 * What the search found and did not pair by itself.
 * ------------------------------------------------------------------------- */

/* ⭐ A DualSense or an Edge, by the name it gives, while the panel's check box
 * "Automatically pair DualSense/DualSense Edge Controllers" is ticked
 * (rhoquinn8217, 2026-10-29). Nothing else pairs unasked: a DS4 waits for its
 * Pair button as an Xbox controller does. */
static bool pairs_by_itself(const char *name) {
    return app_configuration != NULL && app_configuration->bridge.bt_auto_pair &&
           strcasestr(name, "dualsense") != NULL;
}

static int found_index(const char *addr) {
    for (int i = 0; i < s_state.found_count; i++) {
        if (strcasecmp(s_state.found[i].addr, addr) == 0) {
            return i;
        }
    }
    return -1;
}

static void found_add(const char *addr, const char *name, bool gamepad) {
    SDL_LockMutex(s_lock);
    if (found_index(addr) < 0 && s_state.found_count < BT_PAIRING_MAX) {
        bt_pairing_found_t *f = &s_state.found[s_state.found_count++];
        memset(f, 0, sizeof *f);
        snprintf(f->addr, sizeof f->addr, "%s", addr);
        snprintf(f->name, sizeof f->name, "%s", name[0] != '\0' ? name : addr);
        f->gamepad = gamepad;
    }
    SDL_UnlockMutex(s_lock);
}

static void found_remove(const char *addr) {
    SDL_LockMutex(s_lock);
    const int i = found_index(addr);
    if (i >= 0) {
        memmove(&s_state.found[i], &s_state.found[i + 1],
                (size_t) (s_state.found_count - i - 1) * sizeof s_state.found[0]);
        s_state.found_count--;
    }
    SDL_UnlockMutex(s_lock);
}

static void found_set_pairing(const char *addr, bool pairing) {
    SDL_LockMutex(s_lock);
    const int i = found_index(addr);
    if (i >= 0) {
        s_state.found[i].pairing = pairing;
    }
    SDL_UnlockMutex(s_lock);
}

/* ⭐ READ AGAIN AFTER A PAIRING, more than once (2026-10-29, the C1): the TV says
 * "paired" a few seconds before its list has the device, and a list read at
 * once came back without it twice, so a new controller showed only once the
 * panel was opened again. */
static Uint32 refresh_later_cb(Uint32 interval, void *param) {
    (void) interval;
    (void) param;
    enqueue(JOB_REFRESH, NULL, NULL);
    return 0;
}

static void refresh_later(void) {
    SDL_AddTimer(2000, refresh_later_cb, NULL);
    SDL_AddTimer(5000, refresh_later_cb, NULL);
    SDL_AddTimer(10000, refresh_later_cb, NULL);
}

static void do_refresh(void) {
    char *reply = bt_pairing_call("getPairedDeviceLists", "{}");
    jdomparser_ref parser = NULL;
    jvalue_ref root = parse_reply(reply, &parser);
    if (bool_member(root, "returnValue") != 1) {
        char why[120] = "no answer";
        string_member(root, "errorText", why, sizeof why);
        set_status("The TV did not give Aurora its Bluetooth devices: %s", why);
    } else {
        bt_pairing_state_t fresh;
        memset(&fresh, 0, sizeof fresh);
        jvalue_ref list = jobject_get(root, j_cstr_to_buffer("deviceLists"));
        const ssize_t n = jis_array(list) ? jarray_size(list) : 0;
        for (ssize_t i = 0; i < n && fresh.count < BT_PAIRING_MAX; i++) {
            jvalue_ref d = jarray_get(list, i);
            bt_pairing_device_t *dev = &fresh.devices[fresh.count];
            /* ⓘ The C1 lists its empty slots too, as "Not Paired" at "N/A"
             * (2026-10-29): only what is paired is a device. */
            if (!string_member(d, "deviceAddr", dev->addr, sizeof dev->addr) || strcmp(dev->addr, "N/A") == 0 ||
                bool_member(d, "isPaired") == 0) {
                continue;
            }
            if (!string_member(d, "deviceName", dev->name, sizeof dev->name) || dev->name[0] == '\0') {
                snprintf(dev->name, sizeof dev->name, "%s", dev->addr);
            }
            char type[24] = "";
            string_member(d, "deviceType", type, sizeof type);
            /* ⓘ By its name as well: the C1 calls its DualSense "undefined". */
            dev->gamepad = strcmp(type, "gamepad") == 0 || strcasestr(dev->name, "controller") != NULL ||
                           strcasestr(dev->name, "dualsense") != NULL || strcasestr(dev->name, "gamepad") != NULL;
            dev->connected = bool_member(d, "isConnected") == 1;
            fresh.count++;
        }
        SDL_LockMutex(s_lock);
        memcpy(s_state.devices, fresh.devices, sizeof s_state.devices);
        s_state.count = fresh.count;
        s_state.listed = true;
        SDL_UnlockMutex(s_lock);
        /* A found device the TV now lists is registered, not found. */
        for (int i = 0; i < fresh.count; i++) {
            found_remove(fresh.devices[i].addr);
        }
    }
    if (parser != NULL) {
        jdomparser_release(&parser);
    }
    free(reply);
    notify();
}

/* Forget the devices with the address `addr`, or every device with NULL.
 * ⛔ AN EMPTY ADDRESS IS ONE DEVICE'S, NEVER EVERY DEVICE'S (2026-10-29, the C1):
 * the TV listed a paired record with no name and no address, and an empty
 * address used to mean "all" here, so that row's Forget would have been Forget
 * all. requestDelete with the empty address removed that record alone. */
static void do_forget(const char *addr) {
    bt_pairing_state_t now;
    bt_pairing_state(&now);
    int asked = 0, forgotten = 0;
    char why[120] = "";
    for (int i = 0; i < now.count; i++) {
        if (addr != NULL && strcmp(addr, now.devices[i].addr) != 0) {
            continue;
        }
        char payload[80];
        snprintf(payload, sizeof payload, "{\"deviceAddr\":\"%s\"}", now.devices[i].addr);
        asked++;
        if (call_ok("requestDelete", payload, why, sizeof why)) {
            forgotten++;
        }
    }
    if (asked == 0) {
        set_status("There was nothing to forget.");
    } else if (forgotten == asked) {
        set_status(asked == 1 ? "Forgot 1 device." : "Forgot %d devices.", asked);
    } else {
        set_status("Forgot %d of %d: %s", forgotten, asked, why);
    }
    do_refresh();
}

/* ---------------------------------------------------------------------------
 * The search: two subscriptions, and a round at a time.
 * ------------------------------------------------------------------------- */

static HContext s_disc_ctx;
static HContext s_status_ctx;
static bool s_disc_on, s_status_on;

static bool searching(void) {
    SDL_LockMutex(s_lock);
    const bool on = s_state.searching;
    SDL_UnlockMutex(s_lock);
    return on;
}

static bool registered(const char *addr) {
    bool found = false;
    SDL_LockMutex(s_lock);
    for (int i = 0; i < s_state.count && !found; i++) {
        found = strcasecmp(s_state.devices[i].addr, addr) == 0;
    }
    SDL_UnlockMutex(s_lock);
    return found;
}

/* A restart, unless one waits already: the timer and a failed pairing can both
 * ask for one. */
static void enqueue_restart(void) {
    SDL_LockMutex(s_lock);
    const bool queued = s_restart_queued;
    s_restart_queued = true;
    SDL_UnlockMutex(s_lock);
    if (!queued) {
        enqueue(JOB_RESTART, NULL, NULL);
    }
}

/* On SDL's timer thread, every BT_RESTART_MS while the search that set it is
 * on; it ends itself with the search. */
static Uint32 restart_tick(Uint32 interval, void *param) {
    SDL_LockMutex(s_lock);
    const bool current = s_state.searching && s_search_gen == (int) (intptr_t) param;
    SDL_UnlockMutex(s_lock);
    if (!current) {
        return 0;
    }
    enqueue_restart();
    return interval;
}

/* On the helper library's thread: what the search found, or that a round
 * ended. */
static bool discovery_cb(LSHandle *sh, LSMessage *reply, void *ctx) {
    (void) sh;
    (void) ctx;
    const char *text = HLunaServiceMessage(reply);
    bt_log("subscribeDiscovery: %s", text != NULL ? text : "(none)");
    jdomparser_ref parser = NULL;
    jvalue_ref root = parse_reply(text, &parser);
    char status[32] = "", addr[24] = "", name[96] = "", type[24] = "";
    string_member(root, "status", status, sizeof status);
    const bool device = string_member(root, "address", addr, sizeof addr);
    if (!string_member(root, "name", name, sizeof name)) {
        string_member(root, "deviceName", name, sizeof name);
    }
    string_member(root, "type", type, sizeof type);
    if (parser != NULL) {
        jdomparser_release(&parser);
    }
    /* ⛔ A ROUND THAT ENDS IS NOT STARTED AGAIN FROM HERE: the restart does it
     * within BT_RESTART_MS, and a cancel ends a round with this same
     * "endDiscovery" (the C1, 2026-10-29), so answering it would restart the
     * restart, round after round. */
    if (!searching() || strcmp(status, "endDiscovery") == 0) {
        return true;
    }
    if (device && addr[0] != '\0' && !registered(addr)) {
        if (!pairs_by_itself(name)) {
            found_add(addr, name, strcmp(type, "gamepad") == 0);
            notify();
            return true;
        }
        SDL_LockMutex(s_lock);
        const bool idle = s_pairing_addr[0] == '\0';
        if (idle) {
            snprintf(s_pairing_addr, sizeof s_pairing_addr, "%s", addr);
            snprintf(s_pairing_name, sizeof s_pairing_name, "%s", name[0] != '\0' ? name : addr);
        }
        SDL_UnlockMutex(s_lock);
        if (idle) {
            enqueue(JOB_CONNECT, addr, name[0] != '\0' ? name : addr);
        }
    }
    return true;
}

/* On the helper library's thread: a device paired, connected, or failed. */
static bool status_cb(LSHandle *sh, LSMessage *reply, void *ctx) {
    (void) sh;
    (void) ctx;
    const char *text = HLunaServiceMessage(reply);
    bt_log("subscribeDevicesStatus: %s", text != NULL ? text : "(none)");
    jdomparser_ref parser = NULL;
    jvalue_ref root = parse_reply(text, &parser);
    char status[32] = "", device[24] = "";
    string_member(root, "status", status, sizeof status);
    string_member(root, "deviceAddr", device, sizeof device);
    if (parser != NULL) {
        jdomparser_release(&parser);
    }
    SDL_LockMutex(s_lock);
    const bool pairing = s_pairing_addr[0] != '\0';
    char name[96], addr[24];
    snprintf(name, sizeof name, "%s", s_pairing_name);
    snprintf(addr, sizeof addr, "%s", s_pairing_addr);
    SDL_UnlockMutex(s_lock);
    /* ⓘ Only what is said of the device being paired: the first answer after
     * subscribing speaks of every device connected, and some of nobody. */
    if (!pairing || (device[0] != '\0' && strcasecmp(device, addr) != 0)) {
        return true;
    }
    if (strcmp(status, "paired") == 0 || strcmp(status, "connected") == 0) {
        enqueue(JOB_SEARCH_DONE, addr, name);
    } else if (strcmp(status, "fail") == 0) {
        const bool still = searching();
        set_status(still ? "Pairing %s failed. Still searching: put it in pairing mode again."
                         : "Pairing %s failed: put it in pairing mode and press Pair again.", name);
        found_set_pairing(addr, false);
        SDL_LockMutex(s_lock);
        s_pairing_addr[0] = '\0';
        SDL_UnlockMutex(s_lock);
        notify();
        if (still) {
            enqueue_restart();
        }
    }
    return true;
}

static void subscribe(HContext *ctx, bool *on, const char *method, LSFilterFunc cb) {
    if (*on) {
        return;
    }
    memset(ctx, 0, sizeof *ctx);
    ctx->callback = cb;
    ctx->multiple = 1;
    ctx->pub = 1;
    const int rc = HLunaServiceCall(method, "{\"subscribe\":true}", ctx);
    *on = rc == 0;
    bt_log("%s: subscribed=%d", method, *on ? 1 : 0);
}

static void unsubscribe(HContext *ctx, bool *on, const char *what) {
    if (!*on) {
        return;
    }
    const int rc = HUnregisterServiceCallback(ctx);
    *on = false;
    bt_log("%s: unsubscribed (%d)", what, rc);
}

static void search_stop(const char *why_status) {
    if (s_restart_timer != 0) {
        SDL_RemoveTimer(s_restart_timer);
        s_restart_timer = 0;
    }
    call_ok("cancelDiscovery", "{}", NULL, 0);
    unsubscribe(&s_disc_ctx, &s_disc_on, "subscribeDiscovery");
    unsubscribe(&s_status_ctx, &s_status_on, "subscribeDevicesStatus");
    SDL_LockMutex(s_lock);
    s_state.searching = false;
    s_pairing_addr[0] = '\0';
    SDL_UnlockMutex(s_lock);
    if (why_status != NULL) {
        set_status("%s", why_status);
    }
    notify();
}

static void do_search_on(void) {
    if (searching()) {
        return;
    }
    SDL_LockMutex(s_lock);
    s_state.searching = true;
    s_state.found_count = 0;
    s_pairing_addr[0] = '\0';
    const int gen = ++s_search_gen;
    SDL_UnlockMutex(s_lock);
    s_search_since = SDL_GetTicks();
    /* ⓘ One plain line (rhoquinn8217, 2026-10-29: "Your instruction to put
     * devices in pairing mode is inflated"): the button already says it is
     * searching. */
    set_status("Put your devices in pairing mode");
    notify();
    subscribe(&s_status_ctx, &s_status_on, BT_SERVICE "subscribeDevicesStatus", status_cb);
    subscribe(&s_disc_ctx, &s_disc_on, BT_SERVICE "subscribeDiscovery", discovery_cb);
    char why[120];
    if (!call_ok("requestDiscovery", "{}", why, sizeof why)) {
        char line[200];
        snprintf(line, sizeof line, "The TV would not search: %s", why);
        search_stop(line);
        return;
    }
    s_restart_timer = SDL_AddTimer(BT_RESTART_MS, restart_tick, (void *) (intptr_t) gen);
}

/* ⭐ THE SEARCH STARTS AFRESH EVERY SIX SECONDS (rhoquinn8217, 2026-10-29:
 * "Sometimes searching doesn't find a controller and needs to be restarted ...
 * restart the search in the background every 6 seconds"). On the C1 a round
 * ran 26 and 39 s without seeing a DS4 in pairing mode, and a fresh one found
 * it 1.4 to 2.2 s in (logs/bt-pairing.log). It does what switching the search
 * off and on does, a new discovery subscription and a new round, and keeps
 * what was found. ⓘ Not while a pairing waits for its answer; the three
 * minutes count from the switch, not from the round. */
static void do_restart(void) {
    SDL_LockMutex(s_lock);
    s_restart_queued = false;
    const bool pairing = s_pairing_addr[0] != '\0';
    SDL_UnlockMutex(s_lock);
    if (!searching() || pairing) {
        return;
    }
    if (SDL_TICKS_PASSED(SDL_GetTicks(), s_search_since + BT_SEARCH_MS)) {
        search_stop("Nothing paired in three minutes.");
        return;
    }
    unsubscribe(&s_disc_ctx, &s_disc_on, "subscribeDiscovery");
    call_ok("cancelDiscovery", "{}", NULL, 0);
    subscribe(&s_disc_ctx, &s_disc_on, BT_SERVICE "subscribeDiscovery", discovery_cb);
    char why[120];
    if (!call_ok("requestDiscovery", "{}", why, sizeof why)) {
        char line[200];
        snprintf(line, sizeof line, "The TV stopped searching: %s", why);
        search_stop(line);
    }
}

/* The pairing itself: the device already stands in s_pairing_addr. Its answer
 * comes by subscribeDevicesStatus, which a press of Pair after the search went
 * off has to start again. */
static void do_connect(const char *addr, const char *name) {
    SDL_LockMutex(s_lock);
    const bool current = strcasecmp(s_pairing_addr, addr) == 0;
    SDL_UnlockMutex(s_lock);
    if (!current) {
        return;
    }
    subscribe(&s_status_ctx, &s_status_on, BT_SERVICE "subscribeDevicesStatus", status_cb);
    found_set_pairing(addr, true);
    set_status("Pairing %s...", name);
    notify();
    char payload[200];
    snprintf(payload, sizeof payload, "{\"name\":\"%s\",\"address\":\"%s\"}", name, addr);
    char why[120];
    if (!call_ok("requestConnect", payload, why, sizeof why)) {
        const bool still = searching();
        set_status(still ? "Pairing %s failed: %s. Still searching." : "Pairing %s failed: %s.", name, why);
        found_set_pairing(addr, false);
        SDL_LockMutex(s_lock);
        s_pairing_addr[0] = '\0';
        SDL_UnlockMutex(s_lock);
        notify();
        if (still) {
            enqueue_restart();
        }
    }
}

/* Pair, from the found list: one at a time. */
static void do_pair(const char *addr, const char *name) {
    SDL_LockMutex(s_lock);
    const bool busy = s_pairing_addr[0] != '\0';
    char other[96];
    snprintf(other, sizeof other, "%s", s_pairing_name);
    if (!busy) {
        snprintf(s_pairing_addr, sizeof s_pairing_addr, "%s", addr);
        snprintf(s_pairing_name, sizeof s_pairing_name, "%s", name[0] != '\0' ? name : addr);
    }
    SDL_UnlockMutex(s_lock);
    if (busy) {
        set_status("Still pairing %s: one at a time.", other);
        notify();
        return;
    }
    do_connect(addr, name[0] != '\0' ? name : addr);
}

static int worker_main(void *data) {
    (void) data;
    for (;;) {
        SDL_SemWait(s_wake);
        job_t job;
        while (dequeue(&job)) {
            switch (job.kind) {
                case JOB_REFRESH:
                    do_refresh();
                    break;
                case JOB_FORGET:
                    do_forget(job.addr);
                    break;
                case JOB_FORGET_ALL:
                    do_forget(NULL);
                    break;
                case JOB_SEARCH_ON:
                    do_search_on();
                    break;
                /* ⓘ NO "Search is off." ANYWHERE (rhoquinn8217, 2026-10-29: "No
                 * text and a off colored toggle means the same"): switched off,
                 * the line goes; by itself, it says only why. */
                case JOB_SEARCH_OFF:
                    if (searching()) {
                        search_stop("");
                    }
                    break;
                case JOB_SEARCH_DONE: {
                    char line[200];
                    snprintf(line, sizeof line, "Paired %s.", job.name);
                    search_stop(line);
                    found_remove(job.addr);
                    do_refresh();
                    refresh_later();
                    break;
                }
                case JOB_CONNECT:
                    do_connect(job.addr, job.name);
                    break;
                case JOB_PAIR:
                    do_pair(job.addr, job.name);
                    break;
                case JOB_RESTART:
                    do_restart();
                    break;
            }
        }
    }
    return 0;
}

/* Started at the first use, and left running: it sleeps on its semaphore. */
static bool ensure_worker(void) {
    if (s_worker != NULL) {
        return true;
    }
    if (s_lock == NULL) {
        s_lock = SDL_CreateMutex();
    }
    if (s_wake == NULL) {
        s_wake = SDL_CreateSemaphore(0);
    }
    if (s_lock == NULL || s_wake == NULL) {
        return false;
    }
    s_worker = SDL_CreateThread(worker_main, "bt-pairing-worker", NULL);
    if (s_worker != NULL) {
        SDL_DetachThread(s_worker);
    }
    return s_worker != NULL;
}

/* ---------------------------------------------------------------------------
 * The panel's calls.
 * ------------------------------------------------------------------------- */

void bt_pairing_set_listener(void (*changed)(void)) {
    s_listener = changed;
}

void bt_pairing_state(bt_pairing_state_t *out) {
    if (!ensure_worker()) {
        memset(out, 0, sizeof *out);
        return;
    }
    SDL_LockMutex(s_lock);
    *out = s_state;
    SDL_UnlockMutex(s_lock);
}

void bt_pairing_refresh(void) {
    if (ensure_worker()) {
        enqueue(JOB_REFRESH, NULL, NULL);
    }
}

void bt_pairing_forget(const char *addr) {
    if (ensure_worker()) {
        enqueue(addr != NULL ? JOB_FORGET : JOB_FORGET_ALL, addr, NULL);
    }
}

void bt_pairing_search(bool on) {
    if (ensure_worker()) {
        enqueue(on ? JOB_SEARCH_ON : JOB_SEARCH_OFF, NULL, NULL);
    }
}

void bt_pairing_pair(const char *addr) {
    if (addr == NULL || !ensure_worker()) {
        return;
    }
    char name[96] = "";
    SDL_LockMutex(s_lock);
    const int i = found_index(addr);
    if (i >= 0) {
        snprintf(name, sizeof name, "%s", s_state.found[i].name);
    }
    SDL_UnlockMutex(s_lock);
    enqueue(JOB_PAIR, addr, name);
}

#else

char *bt_pairing_call(const char *method, const char *payload) {
    (void) method;
    (void) payload;
    return NULL;
}

void bt_pairing_set_listener(void (*changed)(void)) {
    (void) changed;
}

void bt_pairing_state(bt_pairing_state_t *out) {
    memset(out, 0, sizeof *out);
}

void bt_pairing_refresh(void) {
}

void bt_pairing_forget(const char *addr) {
    (void) addr;
}

void bt_pairing_search(bool on) {
    (void) on;
}

void bt_pairing_pair(const char *addr) {
    (void) addr;
}

#endif
