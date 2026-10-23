/* A window's box fills from a keyboard. The why is in the header. */

#include "type_into_box.h"

static lv_obj_t *s_window;
static lv_obj_t *s_box;
static lv_obj_t *s_buttons;
static uint16_t s_ok;

static void window_deleted(lv_event_t *event) {
    if (lv_event_get_target(event) == s_window) {
        s_window = NULL;
        s_box = NULL;
        s_buttons = NULL;
    }
}

void type_into_box(lv_obj_t *window, lv_obj_t *box, lv_obj_t *buttons, uint16_t ok) {
    s_window = window;
    s_box = box;
    s_buttons = buttons;
    s_ok = ok;
    lv_obj_add_event_cb(window, window_deleted, LV_EVENT_DELETE, NULL);
}

/* The character a key types, or 0: a box's characters only (an address or a
 * host name), from the main keys or the number pad. */
static char typed(const SDL_Keysym *key) {
    const bool shift = (key->mod & KMOD_SHIFT) != 0;
    const SDL_Keycode sym = key->sym;
    if (sym >= SDLK_a && sym <= SDLK_z) {
        const bool capital = shift != ((key->mod & KMOD_CAPS) != 0);
        return (char) (capital ? sym - SDLK_a + 'A' : sym);
    }
    if (sym >= SDLK_0 && sym <= SDLK_9) {
        return shift ? 0 : (char) sym;
    }
    if (sym >= SDLK_KP_1 && sym <= SDLK_KP_9) {
        return (char) ('1' + (sym - SDLK_KP_1));
    }
    switch (sym) {
        case SDLK_KP_0:
            return '0';
        case SDLK_PERIOD:
            return shift ? 0 : '.';
        case SDLK_KP_PERIOD:
            return '.';
        case SDLK_MINUS:
            return shift ? '_' : '-';
        case SDLK_KP_MINUS:
            return '-';
        case SDLK_UNDERSCORE:
            return '_';
        case SDLK_SEMICOLON:
            return shift ? ':' : 0;
        case SDLK_COLON:
        case SDLK_KP_COLON:
            return ':';
        case SDLK_LEFTBRACKET:
            return shift ? 0 : '[';
        case SDLK_RIGHTBRACKET:
            return shift ? 0 : ']';
        default:
            return 0;
    }
}

bool type_into_box_key(const SDL_KeyboardEvent *event) {
    if (s_box == NULL || event->type != SDL_KEYDOWN || SDL_IsTextInputActive()) {
        return false;
    }
    /* Ctrl and Alt make shortcuts, not characters. A box made unusable (the
     * window is adding the computer) takes nothing. */
    if ((event->keysym.mod & (KMOD_CTRL | KMOD_ALT | KMOD_GUI)) != 0 || lv_obj_has_state(s_box, LV_STATE_DISABLED)) {
        return false;
    }
    if (event->keysym.sym == SDLK_BACKSPACE) {
        lv_textarea_del_char(s_box);
        return true;
    }
    const char c = typed(&event->keysym);
    if (c == 0) {
        return false;
    }
    /* As the box takes a character itself: its accepted characters, its
     * length, and its value-changed event, which enables OK for an address. */
    lv_textarea_add_char(s_box, (uint32_t) c);
    if (s_buttons != NULL) {
        lv_btnmatrix_set_selected_btn(s_buttons, s_ok);
    }
    return true;
}
