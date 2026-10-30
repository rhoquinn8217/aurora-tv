/* The streaming overlay's buttons of this fork's: USB Bridge, which opens the
 * panel (ctm_panel.h); DS5-USBIP, which asks the USB server on the host for its
 * settings window; and Controllers, which opens the Bluetooth controllers panel
 * (ui/common/bt_panel.h, 2026-10-29).
 *
 * ⭐ THIRD TO FIFTH IN THE ROW: Full keyboard, Virtual Mouse, then these.
 * streaming.view.c makes them right after Virtual Mouse, so that both their
 * place in the row and their place in the focus order fall to its right.
 *
 * ⭐ USB Bridge and DS5-USBIP are HIDDEN RATHER THAN ABSENT when device bridging
 * is switched off: the overlay's focus order is built from the row's children,
 * and removing one shifts everything after it. ⓘ A hidden object keeps its place
 * and takes no focus. Controllers is always shown: pairing to the TV is not
 * bridging.
 *
 * ⭐ AND A CHECK ON UPSTREAM'S VIRTUAL MOUSE BUTTON WHILE IT IS ON
 * (rhoquinn8217, 2026-10-28): a white circle with a green check on its top
 * right corner (bridge_overlay.c). */

#ifndef BRIDGE_OVERLAY_H
#define BRIDGE_OVERLAY_H

#include "lvgl.h"

#include "streaming.controller.h"

/* Makes the three buttons in the overlay's row, `actions`, with the overlay's
 * own button styles, and connects them; and the check on Virtual Mouse, the
 * row's last button when this is called. */
void bridge_overlay_buttons_create(streaming_controller_t *controller, lv_obj_t *actions);

/* A key the overlay heard: Up from its row selects the stats' pin. True when
 * it took the key. From the overlay's own key handler (streaming.controller.c). */
bool stats_pin_overlay_key(streaming_controller_t *controller, uint32_t key);

#endif /* BRIDGE_OVERLAY_H */
