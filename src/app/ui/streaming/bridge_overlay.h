/* The streaming overlay's two buttons of this fork's: USB Bridge, which opens
 * the panel (ctm_panel.h), and DS5-USBIP, which asks the USB server on the
 * host for its settings window.
 *
 * ⭐ THIRD AND FOURTH IN THE ROW: Full keyboard, Virtual Mouse, then these.
 * streaming.view.c makes them right after Virtual Mouse, so that both their
 * place in the row and their place in the focus order fall to its right.
 *
 * ⭐ HIDDEN RATHER THAN ABSENT when device bridging is switched off: the
 * overlay's focus order is built from the row's children, and removing one
 * shifts everything after it. ⓘ A hidden object keeps its place and takes no
 * focus. */

#ifndef BRIDGE_OVERLAY_H
#define BRIDGE_OVERLAY_H

#include "lvgl.h"

#include "streaming.controller.h"

/* Makes the two buttons in the overlay's row, `actions`, with the overlay's
 * own button styles, and connects them. */
void bridge_overlay_buttons_create(streaming_controller_t *controller, lv_obj_t *actions);

#endif /* BRIDGE_OVERLAY_H */
