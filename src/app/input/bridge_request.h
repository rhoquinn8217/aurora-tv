/* Bridging and releasing one device: the single place that decides HOW.
 *
 * ⭐⭐ ONE IMPLEMENTATION FOR EVERY CALLER. The USB Bridge panel's rows, Auto
 * Bridge and the terminal control port all come through here, so a test driven
 * from a terminal exercises exactly what pressing a row does, and a change to
 * the handover reaches all three at once. ⓘ Until 2026-09-12 the panel and Auto
 * Bridge each carried their own copy of this decision.
 *
 * ⚠️ Only the ROUTE is decided here. Whether the listener is up, and what a
 * screen shows while a bridge is on its way, stay with the caller. */

#ifndef BRIDGE_REQUEST_H
#define BRIDGE_REQUEST_H

#include <stdbool.h>

#if defined(TARGET_WEBOS)

#include "ctm_bridge_glue.h"

typedef enum {
    /* The gesture took the request. It retires the emulated pad and plugs on a
     * later tick, so the device does not read as bridged yet. */
    BRIDGE_REQUEST_ASKED,
    /* No SDL controller was found behind the node, so it was plugged directly. */
    BRIDGE_REQUEST_PLUGGED,
    /* The direct plug failed. */
    BRIDGE_REQUEST_FAILED,
} bridge_request_result_t;

bridge_request_result_t bridge_request_device(const ctm_bridge_dev_t *dev);

void bridge_release_device(const ctm_bridge_dev_t *dev);

/* "asked", "plugged" or "failed", for logs and replies. */
const char *bridge_request_result_name(bridge_request_result_t result);

#endif /* TARGET_WEBOS */

#endif /* BRIDGE_REQUEST_H */
