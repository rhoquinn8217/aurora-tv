/* Where the on-screen keyboard sits. The why is in the header. */

#include "keyboard_place.h"

#include "keyboard_tabs.h"
#include "ui/common/pad_marks.h"

static bool s_top;
static lv_obj_t *s_panel;

static void place_apply(void) {
    if (s_panel != NULL) {
        lv_obj_align(s_panel, s_top ? LV_ALIGN_TOP_MID : LV_ALIGN_BOTTOM_MID, 0, 0);
    }
    /* The tabs go to the edge that faces the middle now, Move with its new words. */
    keyboard_tabs_place();
}

static void panel_deleted(lv_event_t *event) {
    (void) event;
    s_panel = NULL;
}

void keyboard_place_attach(lv_obj_t *panel) {
    s_panel = panel;
    lv_obj_add_event_cb(panel, panel_deleted, LV_EVENT_DELETE, NULL);
    place_apply();
}

void keyboard_place_toggle(void) {
    s_top = !s_top;
    place_apply();
}

bool keyboard_place_button(SDL_JoystickID which, Uint8 button, bool pressed) {
    if (pressed) {
        pad_family_note(which);
    }
    if (button != SDL_CONTROLLER_BUTTON_BACK) {
        return false;
    }
    if (pressed) {
        keyboard_place_toggle();
    }
    return true;
}

bool keyboard_place_at_top(void) {
    return s_top;
}

const char *keyboard_place_name(void) {
    return s_top ? "top" : "bottom";
}
