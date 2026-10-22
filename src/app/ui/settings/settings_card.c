/* The Settings windows, taller. The why is in the header. */

#include "settings_card.h"

#include "../launcher/launcher.controller.h"   /* LAUNCHER_TOPBAR_DPX */

/* The gap left under them, to the screen's bottom edge. */
#define BOTTOM_GAP_DPX 20

/* From under the top bar to the gap above the screen's bottom. ⓘ From the
 * screen, not the Settings layer, whose height as the window is made is not
 * yet its height once laid out: upstream's window, the layer's height less
 * 120 DPX, came out 500 px tall on the rooted monitor (2026-10-22), from a
 * layer that is at most 684 there (804 less the top bar). */
static lv_coord_t window_height(lv_obj_t *obj) {
    return lv_disp_get_ver_res(lv_obj_get_disp(obj)) - LV_DPX(LAUNCHER_TOPBAR_DPX) - LV_DPX(BOTTOM_GAP_DPX);
}

void settings_window_under_topbar(lv_obj_t *panel) {
    if (panel == NULL) {
        return;
    }
    const lv_coord_t height = window_height(panel);
    if (height <= lv_obj_get_style_height(panel, LV_PART_MAIN)) {
        return;
    }
    /* Its title bar keeps its height and its list, which grows to fill the
     * rest, takes what the window gains. */
    lv_obj_set_height(panel, height);
    lv_obj_align(panel, LV_ALIGN_TOP_MID, 0, 0);
}

void settings_card_under_topbar(lv_obj_t *mbox) {
    if (mbox == NULL) {
        return;
    }
    const lv_coord_t height = window_height(mbox);
    const lv_coord_t grow = height - lv_obj_get_style_height(mbox, LV_PART_MAIN);
    if (grow <= 0) {
        return;
    }
    lv_obj_set_height(mbox, height);
    /* Upstream fixes the content's height, and its largest, so the section
     * scrolls inside the card (settings.controller.c,
     * settings_show_pane_popup): both grow with the card. */
    lv_obj_t *content = lv_msgbox_get_content(mbox);
    if (content != NULL) {
        lv_obj_set_height(content, lv_obj_get_style_height(content, LV_PART_MAIN) + grow);
        lv_obj_set_style_max_height(content, lv_obj_get_style_max_height(content, LV_PART_MAIN) + grow, 0);
    }
    lv_obj_align(mbox, LV_ALIGN_TOP_MID, 0, LV_DPX(LAUNCHER_TOPBAR_DPX));
}
