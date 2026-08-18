/* The CTM bridge panel. Lifted out of streaming.controller.c on 2026-08-17.
 *
 * ⭐ THIS HEADER IS THE SEAM, AND IT IS MEANT TO STAY THIS SMALL. Two entry
 * points: one to open the panel, one for the owner fragment's teardown. If a
 * third appears, ask whether it belongs on this side instead.
 */

#pragma once

#include "lvgl.h"

/* ⚠️ The real type, not a forward declaration. streaming_controller_t is an
 * ANONYMOUS struct typedef, so `typedef struct streaming_controller_t ...`
 * declares a DIFFERENT type and the compiler says so. */
#include "streaming.controller.h"

/* Opens the panel. Wired to the CTM button's LV_EVENT_CLICKED. */
void ctm_panel_open(lv_event_t *event);

/* Release the panel's focus groups and cancel its deferred teardown.
 *
 * ⛔ MUST be called from the owner fragment's delete hook, not from the panel's
 * own close: the fragment can be torn down with the panel still open, and the
 * groups would outlive the input device pointing at them. */
void ctm_panel_on_owner_deleted(streaming_controller_t *controller);
