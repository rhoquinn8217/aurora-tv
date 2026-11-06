/* A window closes when the screen around it is clicked. The why is in the
 * header. */

#include "click_away.h"

/* A box whose buttons are greyed or hidden is busy (Add server while it adds,
 * for one): Back leaves it alone (the theme's msgbox_cancel), and so does a
 * click outside. ⛔ Closing it then freed the window while its worker was
 * still to answer into it: a crash (2026-10-23). A box with no buttons at all,
 * a menu, closes as before. */
static bool box_is_busy(lv_obj_t *msgbox) {
    lv_obj_t *btns = lv_msgbox_get_btns(msgbox);
    return btns != NULL && (lv_obj_has_flag(btns, LV_OBJ_FLAG_HIDDEN) ||
                            lv_obj_has_state(btns, LV_STATE_DISABLED) ||
                            lv_btnmatrix_has_btn_ctrl(btns, 0, LV_BTNMATRIX_CTRL_DISABLED));
}

static void backdrop_clicked(lv_event_t *event) {
    lv_obj_t *backdrop = lv_event_get_current_target(event);
    if (lv_event_get_target(event) != backdrop) {
        return;     /* inside the box: the box's own */
    }
    lv_obj_t *msgbox = lv_event_get_user_data(event);
    if (box_is_busy(msgbox)) {
        return;
    }
    lv_msgbox_close_async(msgbox);
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
