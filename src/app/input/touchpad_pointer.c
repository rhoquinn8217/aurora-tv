/* The touchpad as the TV's mouse. The why is in the header. */

#include "touchpad_pointer.h"

#include <stdlib.h>
#include <string.h>

#include <SDL.h>

#include "app.h"
#include "app_input.h"
#include "tv_cursor.h"
#include "ui/root.h"
#include "ui/ui_input.h"
#if defined(TARGET_WEBOS)
#include "ctm_bridge_gesture.h"           /* which of SDL's pads is bridged */
#include "ctmbridge/ctm_bridge_glue.h"    /* the bridged devices' nodes */
#endif

#define PADS 16
/* The pointer goes once the touchpad has been left alone this long. */
#define HIDE_AFTER_MS 4000
/* A stroke across the whole pad crosses this many widths of the screen. */
#define SPEED_SCREENS 1.25f
/* SDL gives a finger's place from 0 to 1 each way; the pads are wider than
 * they are tall, by SDL's own scale for each (1920 x 1070 and 1920 x 920), so
 * a move up and down is shortened by as much to go as far as one across. */
#define DS5_ASPECT (1070.0f / 1920.0f)
#define DS4_ASPECT (920.0f / 1920.0f)

typedef struct {
    bool tracking;      /* one finger down last pass */
    float x, y;         /* where it was, 0 to 1 */
    bool bridged;       /* at a node a bridge holds: the core's, not SDL's */
} pad_t;

static pad_t s_pads[PADS];
static app_t *s_app = NULL;
static lv_indev_drv_t s_drv;
static lv_indev_t *s_indev = NULL;
static lv_obj_t *s_cursor = NULL;
static lv_img_dsc_t *s_picture = NULL;
static unsigned s_picture_generation = 0;
static float s_px = 0, s_py = 0;
static bool s_pressed = false;
static bool s_shown = false;
static bool s_active = false;
static Uint32 s_used_at = 0;

static void pointer_read(lv_indev_drv_t *drv, lv_indev_data_t *data) {
    (void) drv;
    data->point.x = (lv_coord_t) s_px;
    data->point.y = (lv_coord_t) s_py;
    data->state = s_pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

/* The TV cursor's picture, made over into LVGL's: SDL keeps red, green, blue
 * and alpha in that order, LVGL at 32 bits blue, green, red and alpha. ⓘ A
 * picture made again (its look switched) is a new one, never the old one
 * changed: LVGL's SDL renderer keeps a texture per picture, found by its
 * address, and would go on drawing the old. The old one is let go of, a few
 * kilobytes a switch. */
static void take_picture(void) {
    int tip_x = 0, tip_y = 0;
    unsigned generation = 0;
    const SDL_Surface *image = tv_cursor_picture(&tip_x, &tip_y, &generation);
    if (image == NULL || (s_picture != NULL && generation == s_picture_generation)) {
        return;
    }
    const int w = image->w, h = image->h;
    uint8_t *data = malloc((size_t) w * (size_t) h * 4);
    lv_img_dsc_t *picture = calloc(1, sizeof(lv_img_dsc_t));
    if (data == NULL || picture == NULL) {
        free(data);
        free(picture);
        return;
    }
    for (int y = 0; y < h; ++y) {
        const uint8_t *in = (const uint8_t *) image->pixels + (size_t) y * (size_t) image->pitch;
        uint8_t *out = data + (size_t) y * (size_t) w * 4;
        for (int x = 0; x < w; ++x, in += 4, out += 4) {
            out[0] = in[2];
            out[1] = in[1];
            out[2] = in[0];
            out[3] = in[3];
        }
    }
    picture->header.cf = LV_IMG_CF_TRUE_COLOR_ALPHA;
    picture->header.w = w;
    picture->header.h = h;
    picture->data_size = (uint32_t) w * (uint32_t) h * 4;
    picture->data = data;
    lv_img_set_src(s_cursor, picture);
    /* The tip, not the corner, on the point. */
    lv_obj_set_style_translate_x(s_cursor, -tip_x, 0);
    lv_obj_set_style_translate_y(s_cursor, -tip_y, 0);
    s_picture = picture;
    s_picture_generation = generation;
}

void touchpad_pointer_init(app_t *app) {
    s_app = app;
    lv_indev_drv_init(&s_drv);
    s_drv.type = LV_INDEV_TYPE_POINTER;
    s_drv.read_cb = pointer_read;
    s_indev = lv_indev_drv_register(&s_drv);
    if (s_indev == NULL) {
        return;
    }
    s_px = (float) lv_disp_get_hor_res(NULL) / 2.0f;
    s_py = (float) lv_disp_get_ver_res(NULL) / 2.0f;
    s_cursor = lv_img_create(lv_layer_sys());
    lv_obj_clear_flag(s_cursor, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_cursor, LV_OBJ_FLAG_HIDDEN);
    lv_indev_set_cursor(s_indev, s_cursor);
    take_picture();
    if (s_picture == NULL) {
        /* No cursor of Aurora's (a desktop build): a plain dot. */
        lv_obj_set_size(s_cursor, 14, 14);
        lv_obj_set_style_radius(s_cursor, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(s_cursor, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(s_cursor, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(s_cursor, lv_color_black(), 0);
        lv_obj_set_style_border_width(s_cursor, 2, 0);
        lv_obj_set_style_translate_x(s_cursor, -7, 0);
        lv_obj_set_style_translate_y(s_cursor, -7, 0);
    }
}

void touchpad_pointer_deinit(void) {
    if (s_indev != NULL) {
        lv_indev_delete(s_indev);
        s_indev = NULL;
    }
    if (s_cursor != NULL) {
        lv_obj_del(s_cursor);
        s_cursor = NULL;
    }
    if (s_picture != NULL) {
        free((void *) s_picture->data);
        free(s_picture);
        s_picture = NULL;
    }
}

static void show(bool on) {
    if (s_cursor == NULL || on == s_shown) {
        return;
    }
    s_shown = on;
    if (on) {
        take_picture();
        lv_obj_clear_flag(s_cursor, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_cursor, LV_OBJ_FLAG_HIDDEN);
    }
}

#if defined(TARGET_WEBOS)
/* ⛔ A BRIDGED PAD IS THE CORE'S. SDL can still read one (overlay_volume.c
 * says when), so a pad of SDL's at a node a bridge holds is left out. Matched
 * every quarter second, not every pass: the match can look in /sys. */
static void match_bridged(struct app_input_t *input, size_t slots, Uint32 now) {
    static Uint32 s_matched_at = 0;
    static int s_matched_n = -1;
    if (s_app == NULL || s_app->session == NULL) {
        if (s_matched_n != 0) {
            for (size_t i = 0; i < PADS; ++i) {
                s_pads[i].bridged = false;
            }
            s_matched_n = 0;
        }
        return;
    }
    ctm_bridge_stick_t bridged[8];
    const int n = ctm_bridge_held_sticks(bridged, 8);
    if (n == s_matched_n && (Sint32) (now - s_matched_at) < 250) {
        return;
    }
    s_matched_n = n;
    s_matched_at = now;
    for (size_t i = 0; i < slots; ++i) {
        app_gamepad_state_t *pad = &input->gamepads[i];
        s_pads[i].bridged = false;
        if (pad->instance_id == -1 || pad->controller == NULL) {
            continue;
        }
        const char *path = SDL_GameControllerPath(pad->controller);
        for (int b = 0; b < n && !s_pads[i].bridged; ++b) {
            s_pads[i].bridged = ctm_bridge_gesture_path_is_node(path, bridged[b].node);
        }
    }
}
#endif

static void stand_down(void) {
    for (size_t i = 0; i < PADS; ++i) {
        s_pads[i].tracking = false;
    }
    s_pressed = false;
    show(false);
}

void touchpad_pointer_tick(struct app_input_t *input, bool ui_has_input) {
    if (s_indev == NULL || input == NULL) {
        return;
    }
    if (!ui_has_input || !app_configuration->bridge.touchpad_pointer) {
        if (s_active) {
            s_active = false;
            stand_down();
        }
        return;
    }
    s_active = true;
#if SDL_VERSION_ATLEAST(2, 0, 14)
    const Uint32 now = SDL_GetTicks();
    const size_t slots = input->max_num_gamepads < PADS ? input->max_num_gamepads : PADS;
#if defined(TARGET_WEBOS)
    match_bridged(input, slots, now);
#endif
    const float screen_w = (float) lv_disp_get_hor_res(NULL);
    const float screen_h = (float) lv_disp_get_ver_res(NULL);
    bool pressed = false, moved = false;
    for (size_t i = 0; i < slots; ++i) {
        app_gamepad_state_t *pad = &input->gamepads[i];
        pad_t *p = &s_pads[i];
        if (pad->instance_id == -1 || pad->controller == NULL || p->bridged) {
            p->tracking = false;
            continue;
        }
        SDL_GameController *controller = pad->controller;
        const SDL_GameControllerType type = SDL_GameControllerGetType(controller);
        /* ⓘ Sony's vendor id as well as SDL's type: an SDL that does not
         * know the Edge yet names no type for it. */
        const bool playstation = type == SDL_CONTROLLER_TYPE_PS4 || type == SDL_CONTROLLER_TYPE_PS5 ||
                                 SDL_GameControllerGetVendor(controller) == 0x054c;
        if (!playstation || SDL_GameControllerGetNumTouchpads(controller) < 1) {
            p->tracking = false;
            continue;
        }
        int down = 0;
        float fx = 0, fy = 0;
        const int fingers = SDL_GameControllerGetNumTouchpadFingers(controller, 0);
        for (int f = 0; f < fingers; ++f) {
            Uint8 state = 0;
            float x = 0, y = 0, pressure = 0;
            if (SDL_GameControllerGetTouchpadFinger(controller, 0, f, &state, &x, &y, &pressure) == 0 && state) {
                if (down == 0) {
                    fx = x;
                    fy = y;
                }
                ++down;
            }
        }
        if (down == 1) {
            if (p->tracking) {
                const float k = screen_w * SPEED_SCREENS;
                const float aspect = type == SDL_CONTROLLER_TYPE_PS4 ? DS4_ASPECT : DS5_ASPECT;
                const float dx = (fx - p->x) * k;
                const float dy = (fy - p->y) * k * aspect;
                if (dx != 0 || dy != 0) {
                    s_px += dx;
                    s_py += dy;
                    moved = true;
                }
            }
            p->tracking = true;
            p->x = fx;
            p->y = fy;
        } else {
            /* Lifted, or two fingers: the bridge gesture's, not the pointer's. */
            p->tracking = false;
        }
        if (down <= 1 && SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_TOUCHPAD)) {
            pressed = true;
        }
    }
    if (s_px < 0) s_px = 0;
    if (s_py < 0) s_py = 0;
    if (s_px > screen_w - 1) s_px = screen_w - 1;
    if (s_py > screen_h - 1) s_py = screen_h - 1;
    if (pressed != s_pressed) {
        s_pressed = pressed;
        moved = true;
        if (pressed && s_app != NULL) {
            /* As a mouse's click does: the interface stops showing the key's
             * focus as if a key had moved it. */
            ui_set_input_mode(&s_app->ui.input, UI_INPUT_MODE_MOUSE);
        }
    }
    if (moved || s_pressed) {
        s_used_at = now;
        show(true);
    } else if (s_shown && (Sint32) (now - s_used_at) >= HIDE_AFTER_MS) {
        show(false);
    }
#endif
}
