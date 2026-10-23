/* One HTTPS request at a time to each host. The why is in the header. */

#include "host_gate.h"

#include "errors.h"
#include "logging.h"
#include "xml.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define GATE_SLOTS 32
/* A request that holds its host longer than this (a launch the host is slow to
 * answer, a host that stopped answering) lets the next one through rather than
 * stalling it. Longer than a launch usually takes, so a launch is not overlapped. */
#define GATE_WAIT_MS 8000
#define GATE_SLICE_MS 100

/* The pauses before each "again": the request that spoiled an answer overlapped
 * it, and lasts tens of milliseconds. */
static const int again_pause_ms[] = {150, 300, 600};

struct host_gate {
    char host[128];
    /* Requests holding or waiting for this host; 0 frees the slot. */
    int users;
    pthread_mutex_t lock;
};

static pthread_mutex_t s_table_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_once_t s_table_once = PTHREAD_ONCE_INIT;
static struct host_gate s_table[GATE_SLOTS];

static void table_init(void) {
    for (int i = 0; i < GATE_SLOTS; i++) {
        pthread_mutex_init(&s_table[i].lock, NULL);
    }
}

static bool is_aborted(const int *aborted) {
    return aborted != NULL && __atomic_load_n(aborted, __ATOMIC_ACQUIRE);
}

static void sleep_ms(int ms) {
    struct timespec ts = {.tv_sec = ms / 1000, .tv_nsec = (long) (ms % 1000) * 1000000L};
    nanosleep(&ts, NULL);
}

/* "address:port" of an https:// URL, and where its path starts; NULL for any other URL. */
static const char *https_host(const char *url, char *host, size_t len) {
    static const char scheme[] = "https://";
    if (strncmp(url, scheme, sizeof(scheme) - 1) != 0) {
        return NULL;
    }
    const char *start = url + sizeof(scheme) - 1;
    size_t n = strcspn(start, "/?");
    if (n == 0 || n >= len) {
        return NULL;
    }
    memcpy(host, start, n);
    host[n] = '\0';
    return start + n;
}

static void gate_release(host_gate_t *gate) {
    pthread_mutex_lock(&s_table_lock);
    gate->users--;
    pthread_mutex_unlock(&s_table_lock);
}

host_gate_t *host_gate_enter(const char *url, const int *aborted) {
    char host[sizeof(s_table[0].host)];
    if (https_host(url, host, sizeof(host)) == NULL) {
        return NULL;
    }
    pthread_once(&s_table_once, table_init);
    pthread_mutex_lock(&s_table_lock);
    host_gate_t *gate = NULL, *empty = NULL;
    for (int i = 0; i < GATE_SLOTS && gate == NULL; i++) {
        if (s_table[i].users > 0 && strcmp(s_table[i].host, host) == 0) {
            gate = &s_table[i];
        } else if (s_table[i].users == 0 && empty == NULL) {
            empty = &s_table[i];
        }
    }
    if (gate == NULL && empty != NULL) {
        gate = empty;
        strcpy(gate->host, host);
    }
    if (gate != NULL) {
        gate->users++;
    }
    pthread_mutex_unlock(&s_table_lock);
    if (gate == NULL) {
        /* More hosts busy at once than slots: no gate. */
        return NULL;
    }
    /* In slices, so a Cancel (gs_abort) does not wait out the host's other request. */
    for (int waited = 0; waited < GATE_WAIT_MS && !is_aborted(aborted); waited += GATE_SLICE_MS) {
        struct timespec until;
        clock_gettime(CLOCK_REALTIME, &until);
        until.tv_nsec += GATE_SLICE_MS * 1000000L;
        if (until.tv_nsec >= 1000000000L) {
            until.tv_sec++;
            until.tv_nsec -= 1000000000L;
        }
        if (pthread_mutex_timedlock(&gate->lock, &until) == 0) {
            return gate;
        }
    }
    if (!is_aborted(aborted)) {
        commons_log_info("GameStream", "%s still busy after %d ms; not waiting longer", host, GATE_WAIT_MS);
    }
    gate_release(gate);
    return NULL;
}

void host_gate_leave(host_gate_t *gate) {
    if (gate == NULL) {
        return;
    }
    pthread_mutex_unlock(&gate->lock);
    gate_release(gate);
}

/* Whether this is an answer Vibepollo gives a paired TV only when two of its
 * requests overlapped (the header). Only for the three requests that change
 * nothing on the host, so that asking twice is harmless. */
static bool answer_spoiled(const char *url, int ret, HTTP_DATA *data) {
    char host[sizeof(s_table[0].host)];
    const char *path = https_host(url, host, sizeof(host));
    if (path == NULL || *path != '/') {
        return false;
    }
    path++;
    const bool serverinfo = strncmp(path, "serverinfo?", 11) == 0;
    const bool applist = strncmp(path, "applist?", 8) == 0;
    if (!serverinfo && !applist && strncmp(path, "appasset?", 9) != 0) {
        return false;
    }
    if (ret == GS_FAILED) {
        /* An HTTP error status, as appasset's 401. A host that does not know the
         * TV answers with a 200 and status_code 401 in the XML instead. */
        return true;
    }
    if (ret != GS_OK || data->size == 0 || xml_status(data->memory, data->size) != GS_OK) {
        return false;
    }
    if (serverinfo) {
        char *pair = NULL;
        bool unpaired = xml_search(data->memory, data->size, "PairStatus", &pair) == GS_OK && pair != NULL &&
                        strcmp(pair, "0") == 0;
        free(pair);
        return unpaired;
    }
    if (applist) {
        /* One app, the placeholder for a TV without the List applications permission. */
        const char *first = strstr(data->memory, "<App>");
        return first != NULL && strstr(first + 1, "<App>") == NULL && strstr(data->memory, "<ID>114514</ID>") != NULL;
    }
    return false;
}

bool host_gate_ask_again(const char *url, int ret, HTTP_DATA *data, int attempt, const int *aborted) {
    const int agains = (int) (sizeof(again_pause_ms) / sizeof(again_pause_ms[0]));
    if (is_aborted(aborted) || !answer_spoiled(url, ret, data)) {
        return false;
    }
    if (attempt >= agains) {
        commons_log_warn("GameStream", "Request %p: the host answered as if not paired %d times; taking it", data,
                         attempt + 1);
        return false;
    }
    commons_log_info("GameStream", "Request %p: the host answered as if not paired (ret %d); asking again", data, ret);
    sleep_ms(again_pause_ms[attempt]);
    return true;
}
