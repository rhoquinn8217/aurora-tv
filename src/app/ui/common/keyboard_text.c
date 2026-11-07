/* aurora-tv's keyboard typing into a box. The why is in the header. */

#include "keyboard_text.h"

#include "app.h"
#include "platform/sdl/navkey_sdl.h"
#include "stream/input/vk.h"
#include "ui/streaming/keyboard_place.h"
#include "ui/streaming/soft_keyboard.h"
#include "util/navkey.h"

static lv_obj_t *s_keyboard;
static lv_group_t *s_group;
static lv_obj_t *s_box;
static lv_obj_t *s_window;
static void (*s_closed)(bool enter, void *user);
static void *s_user;

/* What a key types on a US keyboard, as a host would read it; 0 for none. */
static char key_char(short vk, bool shift) {
    if (vk >= VK_A && vk <= VK_Z) {
        return (char) ((shift ? 'A' : 'a') + (vk - VK_A));
    }
    if (vk >= VK_0 && vk <= VK_9) {
        static const char shifted[] = ")!@#$%^&*(";
        return shift ? shifted[vk - VK_0] : (char) ('0' + (vk - VK_0));
    }
    switch (vk) {
        case VK_SPACE:
            return ' ';
        case VK_OEM_1:
            return shift ? ':' : ';';
        case VK_OEM_PLUS:
            return shift ? '+' : '=';
        case VK_OEM_COMMA:
            return shift ? '<' : ',';
        case VK_OEM_MINUS:
            return shift ? '_' : '-';
        case VK_OEM_PERIOD:
            return shift ? '>' : '.';
        case VK_OEM_2:
            return shift ? '?' : '/';
        case VK_OEM_3:
            return shift ? '~' : '`';
        case VK_OEM_4:
            return shift ? '{' : '[';
        case VK_OEM_5:
            return shift ? '|' : '\\';
        case VK_OEM_6:
            return shift ? '}' : ']';
        case VK_OEM_7:
            return shift ? '"' : '\'';
        default:
            return 0;
    }
}

/* Gives the input back to the window and forgets the keyboard, once. */
static void let_go(void) {
    if (s_group != NULL && global != NULL) {
        app_input_remove_modal_group(&global->ui.input, s_group);
    }
    s_keyboard = NULL;
    s_group = NULL;
    s_box = NULL;
    s_window = NULL;
    s_closed = NULL;
    s_user = NULL;
}

/* Deleted by something else: the input goes back all the same. */
static void keyboard_deleted(lv_event_t *event) {
    if (lv_event_get_target(event) == s_keyboard) {
        let_go();
    }
}

static void finish(bool enter) {
    lv_obj_t *keyboard = s_keyboard;
    if (keyboard == NULL) {
        return;
    }
    void (*closed)(bool, void *) = s_closed;
    void *user = s_user;
    let_go();
    /* Later: it can close from inside a press of one of its own keys. */
    lv_obj_del_async(keyboard);
    if (closed != NULL) {
        closed(enter, user);
    }
}

/* The remote's Back and a keyboard's Esc close it too, as Circle does. */
static void keys_key(lv_event_t *event) {
    if (lv_event_get_key(event) == LV_KEY_ESC) {
        finish(false);
    }
}

void keyboard_text_open(lv_obj_t *box, lv_obj_t *window, void (*closed)(bool enter, void *user), void *user) {
    if (box == NULL || s_keyboard != NULL || global == NULL) {
        return;
    }
    /* With no stream it has no host to send to, and every key comes here. */
    lv_obj_t *keyboard = soft_keyboard_create(lv_layer_top(), NULL, NULL, NULL);
    lv_group_t *group = soft_keyboard_get_group(keyboard);
    if (group == NULL) {
        lv_obj_del(keyboard);
        return;
    }
    s_keyboard = keyboard;
    s_group = group;
    s_box = box;
    s_window = window;
    s_closed = closed;
    s_user = user;
    /* Not dimmed: the window keeps its own backdrop over what is behind it. */
    lv_obj_set_style_bg_opa(keyboard, LV_OPA_TRANSP, 0);
    lv_obj_add_event_cb(keyboard, keyboard_deleted, LV_EVENT_DELETE, NULL);
    /* soft_keyboard.c: the panel is its first child, and the keys the panel's. */
    lv_obj_t *panel = lv_obj_get_child(keyboard, 0);
    lv_obj_t *keys = panel != NULL ? lv_obj_get_child(panel, 0) : NULL;
    if (keys != NULL) {
        lv_obj_add_event_cb(keys, keys_key, LV_EVENT_KEY, NULL);
    }
    /* The controller's presses and a keyboard's keys go to its keys now. */
    app_input_push_modal_group(&global->ui.input, group);
    soft_keyboard_focus_keys(keyboard);
    keyboard_text_place();
}

bool keyboard_text_shown(void) {
    return s_keyboard != NULL;
}

void keyboard_text_close(bool enter) {
    finish(enter);
}

void keyboard_text_drop(void) {
    lv_obj_t *keyboard = s_keyboard;
    if (keyboard == NULL) {
        return;
    }
    let_go();
    lv_obj_del_async(keyboard);
}

bool keyboard_text_send(short vk, bool down, bool shift) {
    if (s_box == NULL) {
        return false;
    }
    /* Enter and Esc close it as their key is let go: closed on the press, the
     * release would land on the window's OK, and the window's buttons act on a
     * release. */
    if (vk == VK_RETURN || vk == VK_ESCAPE) {
        if (!down) {
            finish(vk == VK_RETURN);
        }
        return true;
    }
    if (!down) {
        return true;
    }
    switch (vk) {
        case VK_BACK:
            lv_textarea_del_char(s_box);
            break;
        case VK_DELETE:
            lv_textarea_del_char_forward(s_box);
            break;
        case VK_LEFT:
            lv_textarea_cursor_left(s_box);
            break;
        case VK_RIGHT:
            lv_textarea_cursor_right(s_box);
            break;
        case VK_HOME:
            lv_textarea_set_cursor_pos(s_box, 0);
            break;
        case VK_END:
            lv_textarea_set_cursor_pos(s_box, LV_TEXTAREA_CURSOR_LAST);
            break;
        default: {
            /* The box keeps only what it accepts: an address's characters. */
            const char c = key_char(vk, shift);
            if (c != 0) {
                lv_textarea_add_char(s_box, (uint32_t) (unsigned char) c);
            }
            break;
        }
    }
    return true;
}

bool keyboard_text_button(const SDL_Event *event) {
    if (s_keyboard == NULL) {
        return false;
    }
    bool pressed = event->cbutton.state == SDL_PRESSED;
    const Uint8 button = event->cbutton.button;
    /* Create moves it, as in the stream; a press also says whose marks to draw. */
    if (keyboard_place_button(event->cbutton.which, button, pressed)) {
        return true;
    }
    if (button == SDL_CONTROLLER_BUTTON_LEFTSHOULDER || button == SDL_CONTROLLER_BUTTON_RIGHTSHOULDER) {
        soft_keyboard_gamepad_shortcut(button == SDL_CONTROLLER_BUTTON_LEFTSHOULDER ? SOFT_KBD_GP_ARROW_LEFT
                                                                                     : SOFT_KBD_GP_ARROW_RIGHT,
                                       pressed);
        return true;
    }
    switch (navkey_from_sdl(event, &pressed)) {
        case NAVKEY_UP:
        case NAVKEY_DOWN:
        case NAVKEY_LEFT:
        case NAVKEY_RIGHT:
        case NAVKEY_CONFIRM:
            return false;
        case NAVKEY_ALTERNATIVE:
            if (pressed) {
                keyboard_text_send(VK_SPACE, true, false);
            }
            return true;
        case NAVKEY_NEGATIVE:
            if (pressed) {
                keyboard_text_send(VK_BACK, true, false);
            }
            return true;
        case NAVKEY_START:
            if (pressed) {
                finish(true);
            }
            return true;
        case NAVKEY_CANCEL:
            if (pressed) {
                finish(false);
            }
            return true;
        default:
            return true;
    }
}

void keyboard_text_place(void) {
    if (s_keyboard == NULL || s_window == NULL) {
        return;
    }
    /* soft_keyboard.c: the panel that holds the keys is its first child. */
    lv_obj_t *panel = lv_obj_get_child(s_keyboard, 0);
    lv_obj_update_layout(s_keyboard);
    const lv_coord_t taken = panel != NULL ? lv_obj_get_height(panel) : 0;
    /* In the middle of what the keyboard leaves free. */
    lv_obj_align(s_window, LV_ALIGN_CENTER, 0, keyboard_place_at_top() ? taken / 2 : -taken / 2);
}
