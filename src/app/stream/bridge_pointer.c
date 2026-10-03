#include "bridge_pointer.h"

#include "session_priv.h"
#include "ctm_bridge_glue.h"

/* Last pointer state for the bridged TV pointer: the feed always sends a full
 * absolute report, so wheel and button events reuse the last position.
 * bit0=left bit1=right bit2=middle (the glue's contract). */
static int s_pointer_x, s_pointer_y;
static unsigned s_pointer_buttons;

static unsigned button_bit(Uint8 sdl_button) {
    switch (sdl_button) {
        case SDL_BUTTON_LEFT: return 1u;
        case SDL_BUTTON_RIGHT: return 2u;
        case SDL_BUTTON_MIDDLE: return 4u;
        default: return 0;
    }
}

/* Remote D-pad/OK keys routed through the pointer's keyboard report while it
 * is bridged (SDL scancodes for these equal the HID usages). webOS special
 * scancodes (EXIT/HOME/BACK/CH/colored) are NOT listed: they keep their local
 * overlay/ribbon semantics via the normal key path. */
static unsigned key_usage(SDL_Scancode sc) {
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

bool bridge_pointer_event(session_t *session, const SDL_Event *event) {
    /* ⓘ Each case asks whether the pointer is bridged only for its own kind of
     * event, as the hooks inside upstream's switch did, so a controller's
     * events never pay for the question. */
    switch (event->type) {
        case SDL_KEYDOWN:
        case SDL_KEYUP: {
            if (!ctm_bridge_pointer_active()) {
                return false;
            }
            unsigned usage = key_usage(event->key.keysym.scancode);
            if (usage == 0) {
                return false;
            }
            /* Tap per KEYDOWN (repeats included) and swallow KEYUP: LG SDL
             * KEYUP delivery is unreliable, so held state via the synthesizer
             * could stick a key on the host. */
            if (event->type == SDL_KEYDOWN) {
                ctm_bridge_pointer_feed_key(usage, true);
                ctm_bridge_pointer_feed_key(usage, false);
            }
            return true;
        }
        case SDL_MOUSEMOTION: {
            /* The pointer bridged -> the synthesizer is the single mouse
             * authority on the host; never double-send via moonlight. */
            if (!ctm_bridge_pointer_active()) {
                return false;
            }
            s_pointer_x = event->motion.x;
            s_pointer_y = event->motion.y;
            ctm_bridge_pointer_feed(s_pointer_x, s_pointer_y,
                                    session->display_width, session->display_height,
                                    s_pointer_buttons, 0);
            return true;
        }
        case SDL_MOUSEWHEEL: {
            if (!ctm_bridge_pointer_active()) {
                return false;
            }
            ctm_bridge_pointer_feed(s_pointer_x, s_pointer_y,
                                    session->display_width, session->display_height,
                                    s_pointer_buttons, event->wheel.y);
            return true;
        }
        case SDL_MOUSEBUTTONDOWN:
        case SDL_MOUSEBUTTONUP: {
            if (!ctm_bridge_pointer_active()) {
                return false;
            }
            unsigned bit = button_bit(event->button.button);
            if (event->type == SDL_MOUSEBUTTONDOWN) s_pointer_buttons |= bit;
            else s_pointer_buttons &= ~bit;
            s_pointer_x = event->button.x;
            s_pointer_y = event->button.y;
            ctm_bridge_pointer_feed(s_pointer_x, s_pointer_y,
                                    session->display_width, session->display_height,
                                    s_pointer_buttons, 0);
            return true;
        }
        default:
            return false;
    }
}
