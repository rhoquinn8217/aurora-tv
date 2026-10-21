/* A window closes when the screen around it is clicked. The why is in the
 * header. */

#include "click_away.h"

static void backdrop_clicked(lv_event_t *event) {
    lv_obj_t *backdrop = lv_event_get_current_target(event);
    if (lv_event_get_target(event) != backdrop) {
        return;     /* inside the box: the box's own */
    }
    lv_msgbox_close_async(lv_event_get_user_data(event));
}

void click_away_closes(lv_obj_t *msgbox) {
    if (msgbox == NULL) {
        return;
    }
    lv_obj_t *backdrop = lv_obj_get_parent(msgbox);
    /* ⓘ Only LVGL's backdrop: a box placed in a screen of its own has none,
     * and a click on that screen is not this box's to answer. */
    if (backdrop == NULL || backdrop == lv_layer_top() || backdrop == lv_layer_sys() ||
        backdrop == lv_scr_act()) {
        return;
    }
    lv_obj_add_flag(backdrop, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(backdrop, backdrop_clicked, LV_EVENT_CLICKED, msgbox);
}
