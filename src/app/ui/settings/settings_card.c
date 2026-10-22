/* The Settings section's card, taller. The why is in the header. */

#include "settings_card.h"

#include "../launcher/launcher.controller.h"   /* LAUNCHER_TOPBAR_DPX */

/* The gap left under the card, to the screen's bottom edge. */
#define BOTTOM_GAP_DPX 20

void settings_card_under_topbar(lv_obj_t *mbox) {
    if (mbox == NULL) {
        return;
    }
    const lv_coord_t top = LV_DPX(LAUNCHER_TOPBAR_DPX);
    const lv_coord_t height = lv_disp_get_ver_res(lv_obj_get_disp(mbox)) - top - LV_DPX(BOTTOM_GAP_DPX);
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
    lv_obj_align(mbox, LV_ALIGN_TOP_MID, 0, top);
}
