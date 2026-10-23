#pragma once

/* This fork (2026-10-22): one HTTPS request at a time to each host, and an
 * answer the host spoiled is asked again.
 *
 * Vibepollo 2.0 answers HTTPS requests from a paired TV that overlap each other
 * as if the TV were not paired: serverinfo says PairStatus 0, applist sends one
 * placeholder app ("Permission denied - enable "List applications" ...", id
 * 114514), appasset a 401. Measured from the rooted monitor with its own
 * certificate: two serverinfo requests at once, 11 of 30 answers wrong; four at
 * once, 17 of 40; one at a time, 0 of 15; one overlapping a plain-HTTP request,
 * 0 of 20. The TV overlaps its own requests all the time (status checks, the app
 * list and covers run on the shared executor and the discovery thread), and the
 * launcher showed a paired PC as "Not paired", with a Pair button.
 *
 * A host that really does not know the TV says so differently: a 200 whose XML
 * carries status_code 401. That answer is never asked again, so "Not paired"
 * still shows at once when it is true. */

#include "http.h"

#include <stdbool.h>

typedef struct host_gate host_gate_t;

/* Waits for the host's previous HTTPS request from this TV, for a while.
 * NULL for a plain-HTTP URL, a wait that ran out, or once `aborted` is set:
 * the request then goes ahead on its own. */
host_gate_t *host_gate_enter(const char *url, const int *aborted);

void host_gate_leave(host_gate_t *gate);

/* Whether the answer to `url` is one the host gives only when two requests
 * overlapped, so it is worth asking again; if so, waits a moment first.
 * `attempt` counts from 0, and the third "again" is the last. */
bool host_gate_ask_again(const char *url, int ret, HTTP_DATA *data, int attempt, const int *aborted);
