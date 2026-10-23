/* The "Connecting..." box's Cancel. The why is in stream/connect_cancel.h. */

#include "connecting_cancel.h"

#include "app.h"
#include "stream/connect_cancel.h"
#include "util/i18n.h"

/* ⚠️ The box can outlive its screen, so the button holds the app, never the
 * screen, and acts only on a session still connecting. */
static void cancel_pressed(lv_event_t *event) {
    app_t *app = lv_event_get_user_data(event);
    lv_obj_t *btns = lv_event_get_current_target(event);
    if (lv_btnmatrix_has_btn_ctrl(btns, 0, LV_BTNMATRIX_CTRL_DISABLED) || !connect_cancel_now(app->session)) {
        return;
    }
    lv_btnmatrix_set_btn_ctrl(btns, 0, LV_BTNMATRIX_CTRL_DISABLED);
    /* Upstream's words beside the spinner say what happens until the box goes. */
    lv_obj_t *content = lv_msgbox_get_content(lv_obj_get_parent(btns));
    for (uint32_t i = 0; content != NULL && i < lv_obj_get_child_cnt(content); i++) {
        lv_obj_t *child = lv_obj_get_child(content, (int32_t) i);
        if (lv_obj_check_type(child, &lv_label_class)) {
            lv_label_set_text(child, locstr("Cancelling..."));
            break;
        }
    }
}

void connecting_cancel_button(lv_obj_t *box, app_t *app) {
    if (box == NULL || app == NULL) {
        return;
    }
    /* Upstream fixes the box at 100 DPX high, for its spinner's row: the
     * button takes a row more. */
    lv_obj_set_height(box, LV_SIZE_CONTENT);
    static const char *texts[] = {"", ""};
    texts[0] = locstr("Cancel");
    /* A child of the box, so the theme puts it in the box's focus group, where
     * it has the focus (lv_theme_moonlight.c), and lays it under the spinner,
     * on the right, as every window's buttons are. Sized as LVGL sizes a
     * message box's own buttons (lv_msgbox_create), so it looks like them. */
    lv_obj_t *btns = lv_btnmatrix_create(box);
    lv_btnmatrix_set_map(btns, texts);
    lv_btnmatrix_set_btn_ctrl_all(btns, LV_BTNMATRIX_CTRL_CLICK_TRIG | LV_BTNMATRIX_CTRL_NO_REPEAT);
    const lv_font_t *font = lv_obj_get_style_text_font(btns, LV_PART_ITEMS);
    lv_obj_set_size(btns, 2 * LV_DPI_DEF / 3, lv_font_get_line_height(font) + LV_DPI_DEF / 10);
    lv_obj_add_event_cb(btns, cancel_pressed, LV_EVENT_VALUE_CHANGED, app);
    lv_obj_add_event_cb(btns, cancel_pressed, LV_EVENT_CANCEL, app);
}
