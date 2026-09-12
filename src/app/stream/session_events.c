#include "session_events.h"
#include "session_priv.h"

#include "ctm_bridge_glue.h"

/* Last pointer state for the CTM TV-pointer synthesizer: the feed always
 * sends a full absolute report, so wheel/button events reuse the last
 * position. bit0=left bit1=right bit2=middle (glue contract). */
static int s_ctm_ptr_x, s_ctm_ptr_y;
static unsigned s_ctm_ptr_buttons;

static unsigned ctm_ptr_button_bit(Uint8 sdl_button) {
    switch (sdl_button) {
        case SDL_BUTTON_LEFT: return 1u;
        case SDL_BUTTON_RIGHT: return 2u;
        case SDL_BUTTON_MIDDLE: return 4u;
        default: return 0;
    }
}

/* Remote D-pad/OK keys routed through the CTM pointer's keyboard report while
 * it is bridged (SDL scancodes for these equal the HID usages). webOS special
 * scancodes (EXIT/HOME/BACK/CH/colored) are NOT listed: they keep their local
 * overlay/ribbon semantics via the normal key path. */
static unsigned ctm_ptr_key_usage(SDL_Scancode sc) {
    switch (sc) {
        case SDL_SCANCODE_UP:
        case SDL_SCANCODE_DOWN:
        case SDL_SCANCODE_LEFT:
        case SDL_SCANCODE_RIGHT:
        case SDL_SCANCODE_RETURN:
            return (unsigned) sc;
        case SDL_SCANCODE_KP_ENTER:
            return (unsigned) SDL_SCANCODE_RETURN;
        default:
            return 0;
    }
}

bool session_handle_input_event(session_t *session, const SDL_Event *event) {
    if (!session_accepting_input(session)) {
        return false;
    }
    stream_input_t *input = &session->input;
    switch (event->type) {
        case SDL_KEYDOWN:
        case SDL_KEYUP: {
            if (ctm_bridge_pointer_active()) {
                unsigned usage = ctm_ptr_key_usage(event->key.keysym.scancode);
                if (usage != 0) {
                    /* Tap per KEYDOWN (repeats included) and swallow KEYUP:
                     * LG SDL KEYUP delivery is unreliable, so held state via
                     * the synthesizer could stick a key on the host. */
                    if (event->type == SDL_KEYDOWN) {
                        ctm_bridge_pointer_feed_key(usage, true);
                        ctm_bridge_pointer_feed_key(usage, false);
                    }
                    break;
                }
            }
            stream_input_handle_key(input, &event->key);
            break;
        }
        case SDL_CONTROLLERAXISMOTION: {
            stream_input_handle_caxis(input, &event->caxis);
            break;
        }
        case SDL_CONTROLLERBUTTONDOWN:
        case SDL_CONTROLLERBUTTONUP: {
            stream_input_handle_cbutton(input, &event->cbutton);
            break;
        }
        case SDL_CONTROLLERSENSORUPDATE: {
            stream_input_handle_csensor(input, &event->csensor);
            break;
        }
        case SDL_CONTROLLERTOUCHPADDOWN:
        case SDL_CONTROLLERTOUCHPADMOTION:
        case SDL_CONTROLLERTOUCHPADUP: {
            stream_input_handle_ctouchpad(input, &event->ctouchpad);
            break;
        }
        case SDL_JOYDEVICEADDED:
        case SDL_JOYDEVICEREMOVED: {
            stream_input_handle_jdevice(input, &event->jdevice);
            break;
        }
        case SDL_CONTROLLERDEVICEADDED:
        case SDL_CONTROLLERDEVICEREMOVED: {
            stream_input_handle_cdevice(input, &event->cdevice);
            break;
        }
        case SDL_MOUSEMOTION: {
            /* CTM TV pointer bridged -> the synthesizer is the single mouse
             * authority on the host; never double-send via moonlight. */
            if (ctm_bridge_pointer_active()) {
                s_ctm_ptr_x = event->motion.x;
                s_ctm_ptr_y = event->motion.y;
                ctm_bridge_pointer_feed(s_ctm_ptr_x, s_ctm_ptr_y,
                                        session->display_width, session->display_height,
                                        s_ctm_ptr_buttons, 0);
                break;
            }
            stream_input_handle_mmotion(input, &event->motion, false);
            break;
        }
        case SDL_MOUSEWHEEL: {
            if (ctm_bridge_pointer_active()) {
                ctm_bridge_pointer_feed(s_ctm_ptr_x, s_ctm_ptr_y,
                                        session->display_width, session->display_height,
                                        s_ctm_ptr_buttons, event->wheel.y);
                break;
            }
            if (!input->view_only && !input->no_sdl_mouse) {
                stream_input_handle_mwheel(input, &event->wheel);
            }
            break;
        }
        case SDL_MOUSEBUTTONDOWN:
        case SDL_MOUSEBUTTONUP: {
            if (ctm_bridge_pointer_active()) {
                unsigned bit = ctm_ptr_button_bit(event->button.button);
                if (event->type == SDL_MOUSEBUTTONDOWN) s_ctm_ptr_buttons |= bit;
                else s_ctm_ptr_buttons &= ~bit;
                s_ctm_ptr_x = event->button.x;
                s_ctm_ptr_y = event->button.y;
                ctm_bridge_pointer_feed(s_ctm_ptr_x, s_ctm_ptr_y,
                                        session->display_width, session->display_height,
                                        s_ctm_ptr_buttons, 0);
                break;
            }
            if (!input->view_only && !input->no_sdl_mouse) {
                stream_input_handle_mbutton(input, &event->button);
            }
            break;
        }
        case SDL_TEXTINPUT: {
            stream_input_handle_text(input, &event->text);
            break;
        }
        case SDL_FINGERDOWN:
        case SDL_FINGERUP:
        case SDL_FINGERMOTION: {
            stream_input_handle_touch(input, &event->tfinger);
            break;
        }
        default:
            return false;
    }
    return true;
}

void session_update_touchpad_tap_hold(session_t *session) {
    if (!session_accepting_input(session)) {
        return;
    }

    stream_input_t *input = &session->input;
    if (input->touchpads != NULL) {
        stream_input_update_touchpad_tap_hold(input);
    }
}
