/* The touchpad as the TV's mouse. The why is in the header. */

#include "touchpad_pointer.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <SDL.h>

#include "app.h"
#include "app_input.h"
#include "tv_cursor.h"
#include "ui/root.h"
#include "ui/ui_input.h"
#if defined(TARGET_WEBOS)
#include "ctm_bridge_gesture.h"           /* which of SDL's pads is bridged */
#include "ctmbridge/ctm_bridge_glue.h"    /* the bridged devices' nodes and touches */
#include "stream/session_priv.h"          /* the stream's display size */
#include "stream/input/session_input.h"

#include <Limelight.h>
#endif

/* What happened, where it can be read over SSH (tv_cursor.c says why). */
#define TOUCHPAD_POINTER_LOG "touchpad-pointer.log"
FILE *ctm_log_open(const char *name, const char *mode);

#define PADS 16
#define BRIDGED 8
/* The pointer goes once the touchpad has been left alone this long. */
#define HIDE_AFTER_MS 4000
/* A stroke across the whole pad crosses this many widths of the screen. */
#define SPEED_SCREENS 1.25f
/* SDL gives a finger's place from 0 to 1 each way; the pads are wider than
 * they are tall, by SDL's own scale for each (1920 x 1070 and 1920 x 920), so
 * a move up and down is shortened by as much to go as far as one across. The
 * core gives a bridged DualSense's in its own units, 1920 x 1080 (held_touch.inl
 * in the bridge core). */
#define DS5_ASPECT (1070.0f / 1920.0f)
#define DS4_ASPECT (920.0f / 1920.0f)
#define DS5_RAW_W 1920.0f
#define DS5_RAW_H 1080.0f
/* A light tap clicks (rhoquinn8217, 2026-10-20: "Light touch tap should
 * click"): a finger down and up again within this long, moving less than
 * this much of the pad's width, without the pad pressed in. The click is held
 * this long, long enough for LVGL to read a press and a release. */
#define TAP_MAX_MS 250
#define TAP_MAX_TRAVEL 0.03f
#define TAP_CLICK_MS 60

typedef struct {
    bool tracking;      /* one finger down last pass */
    float x, y;         /* where it was, 0 to 1 */
    int id;             /* the touch's own count, where the core gives it; -1 */
    bool bridged;       /* at a node a bridge holds: the core's, not SDL's */
    bool seen;          /* logged once */
    /* The touch now, for a tap: when it began, how far it went, whether the
     * pad was pressed in or a second finger came, and whether the pointer
     * showed when it began (a tap on a hidden pointer only shows it). */
    bool touching;
    Uint32 touch_at;
    float travel;
    bool spoiled;
    bool pointer_was_shown;
} pad_t;

/* What one pass of every pad came to. */
typedef struct {
    bool moved, pressed, tapped;
} pass_t;

static pad_t s_pads[PADS];
#if defined(TARGET_WEBOS)
/* A bridged pad's touch, kept by the node it is at. */
static pad_t s_bridged[BRIDGED];
static char s_bridged_node[BRIDGED][64];
#endif
static app_t *s_app = NULL;
static lv_indev_drv_t s_drv;
static lv_indev_t *s_indev = NULL;
static lv_obj_t *s_cursor = NULL;
static lv_img_dsc_t *s_picture = NULL;
static unsigned s_picture_generation = 0;
static float s_px = 0, s_py = 0;
static bool s_pressed = false;
static Uint32 s_tap_until = 0;
static bool s_shown = false;
static bool s_active = false;
static Uint32 s_used_at = 0;
/* In a stream (touchpad_pointer_stream_move): the cursor up, when it last
 * moved, and where SDL last had the mouse, to tell when a mouse moved. */
static bool s_stream_shown = false;
static Uint32 s_stream_used_at = 0;
static int s_mouse_x = -1, s_mouse_y = -1;

static void tp_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

static void tp_log(const char *fmt, ...) {
    FILE *f = ctm_log_open(TOUCHPAD_POINTER_LOG, "a");
    if (f == NULL) {
        return;
    }
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    fprintf(f, "%lld.%03ld ", (long long) ts.tv_sec, ts.tv_nsec / 1000000L);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

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
    if (s_cursor == NULL || image == NULL || (s_picture != NULL && generation == s_picture_generation)) {
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
    tp_log("cursor: the TV cursor's picture, %dx%d, tip %d,%d", w, h, tip_x, tip_y);
}

void touchpad_pointer_init(app_t *app) {
    s_app = app;
    FILE *f = ctm_log_open(TOUCHPAD_POINTER_LOG, "w");   /* begun again each start */
    if (f != NULL) {
        fclose(f);
    }
}

/* ⛔ NOT AT THE APP'S START. LVGL's display is made when the interface opens
 * (root.c, app_ui_open), after the app's start, and an input device asked for
 * before then is refused: build 552 had no pointer at all for exactly that
 * (2026-10-20). So the device and its cursor come and go with the display,
 * beside the app's own input devices. */
void touchpad_pointer_ui_opened(app_t *app) {
    s_app = app;
    if (s_indev != NULL) {
        return;
    }
    lv_indev_drv_init(&s_drv);
    s_drv.type = LV_INDEV_TYPE_POINTER;
    s_drv.read_cb = pointer_read;
    s_indev = lv_indev_drv_register(&s_drv);
    if (s_indev == NULL) {
        tp_log("no input device: LVGL refused it");
        return;
    }
    s_px = (float) lv_disp_get_hor_res(NULL) / 2.0f;
    s_py = (float) lv_disp_get_ver_res(NULL) / 2.0f;
    s_cursor = lv_img_create(lv_layer_sys());
    lv_obj_clear_flag(s_cursor, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_cursor, LV_OBJ_FLAG_HIDDEN);
    lv_indev_set_cursor(s_indev, s_cursor);
    s_picture_generation = 0;
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
        tp_log("cursor: a plain dot (no picture of the TV cursor)");
    }
    tp_log("ready on a %dx%d display", (int) lv_disp_get_hor_res(NULL), (int) lv_disp_get_ver_res(NULL));
}

static void forget(pad_t *p) {
    p->tracking = false;
    p->touching = false;
}

static void stand_down(void) {
    for (size_t i = 0; i < PADS; ++i) {
        forget(&s_pads[i]);
    }
#if defined(TARGET_WEBOS)
    for (size_t i = 0; i < BRIDGED; ++i) {
        forget(&s_bridged[i]);
        s_bridged_node[i][0] = '\0';
    }
#endif
    s_pressed = false;
    s_tap_until = 0;
    if (s_cursor != NULL && s_shown) {
        lv_obj_add_flag(s_cursor, LV_OBJ_FLAG_HIDDEN);
    }
    s_shown = false;
}

void touchpad_pointer_ui_closing(void) {
    stand_down();
    s_active = false;
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

void touchpad_pointer_deinit(void) {
    touchpad_pointer_ui_closing();
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

/* One pad's touch this pass, from SDL or from the core alike: `down` fingers,
 * the first at fx, fy, from 0 to 1 each way; `aspect` the pad's height over
 * its width in those terms; `id` the touch's own count where the core gives
 * it (a new count is a new touch, never a slide), -1 where it does not;
 * `pad_in` the touchpad pressed in. */
static void feed(pad_t *p, int down, float fx, float fy, float aspect, int id, bool pad_in, Uint32 now, float k,
                 pass_t *pass) {
    /* The touch, for a tap: begun, carried on, or ended. */
    if (down >= 1 && !p->touching) {
        p->touching = true;
        p->touch_at = now;
        p->travel = 0;
        p->spoiled = false;
        p->pointer_was_shown = s_shown;
    } else if (down == 0 && p->touching) {
        p->touching = false;
        if (!p->spoiled && (Sint32) (now - p->touch_at) <= TAP_MAX_MS && p->travel <= TAP_MAX_TRAVEL) {
            if (p->pointer_was_shown) {
                pass->tapped = true;
            }
            pass->moved = true;   /* a tap on a hidden pointer shows it, and does no more */
        }
    }
    if (down >= 2 || pad_in) {
        p->spoiled = true;
    }
    if (down == 1) {
        if (p->tracking && (id < 0 || id == p->id)) {
            const float dx = (fx - p->x) * k;
            const float dy = (fy - p->y) * k * aspect;
            p->travel += (fx > p->x ? fx - p->x : p->x - fx) + (fy > p->y ? fy - p->y : p->y - fy) * aspect;
            if (dx != 0 || dy != 0) {
                s_px += dx;
                s_py += dy;
                pass->moved = true;
            }
        }
        p->tracking = true;
        p->x = fx;
        p->y = fy;
        p->id = id;
    } else {
        /* Lifted, or two fingers: the bridge gesture's, not the pointer's. */
        p->tracking = false;
    }
    if (down <= 1 && pad_in) {
        pass->pressed = true;
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

/* ⭐ A BRIDGED PAD'S TOUCH, from the core: only in a stream, and only while
 * the overlay or a bridge's question holds the pad's input, the only time the
 * core reads it (its held_touch.inl: a DualSense or an Edge on a cable or a
 * dongle so far). Each kept by its node, so a pad that goes is forgotten. */
static void feed_bridged(Uint32 now, float k, pass_t *pass) {
    if (s_app == NULL || s_app->session == NULL) {
        return;
    }
    ctm_bridge_touch_t touches[BRIDGED];
    const int n = ctm_bridge_held_touches(touches, BRIDGED);
    bool used[BRIDGED] = {false};
    for (int t = 0; t < n; ++t) {
        if (!touches[t].reading) {
            continue;
        }
        int slot = -1;
        for (int i = 0; i < BRIDGED && slot < 0; ++i) {
            if (strcmp(s_bridged_node[i], touches[t].node) == 0) {
                slot = i;
            }
        }
        for (int i = 0; i < BRIDGED && slot < 0; ++i) {
            if (s_bridged_node[i][0] == '\0') {
                slot = i;
                snprintf(s_bridged_node[i], sizeof(s_bridged_node[i]), "%s", touches[t].node);
                memset(&s_bridged[i], 0, sizeof(s_bridged[i]));
                tp_log("bridged pad at %s: its touch read by the core", touches[t].node);
            }
        }
        if (slot < 0) {
            continue;
        }
        used[slot] = true;
        feed(&s_bridged[slot], touches[t].fingers, (float) touches[t].x / DS5_RAW_W,
             (float) touches[t].y / DS5_RAW_H, DS5_RAW_H / DS5_RAW_W, touches[t].id, touches[t].click, now, k, pass);
    }
    for (int i = 0; i < BRIDGED; ++i) {
        if (!used[i] && s_bridged_node[i][0] != '\0') {
            forget(&s_bridged[i]);
            s_bridged_node[i][0] = '\0';
        }
    }
}
#endif

void touchpad_pointer_tick(struct app_input_t *input, bool ui_has_input) {
    if (s_indev == NULL || input == NULL) {
        return;
    }
    if (!ui_has_input || !app_configuration->bridge.touchpad_pointer) {
        if (s_active) {
            s_active = false;
            stand_down();
            tp_log("off: %s", ui_has_input ? "the setting is off" : "a stream has the input");
        }
        /* The stream's cursor (touchpad_pointer_stream_move) goes 4 seconds
         * after its last movement, or as the TV cursor is switched off. */
        if (!ui_has_input && s_stream_shown &&
            (!tv_cursor_mode_on() || (Sint32) (SDL_GetTicks() - s_stream_used_at) >= HIDE_AFTER_MS)) {
            s_stream_shown = false;
            show(false);
        }
        return;
    }
    if (s_stream_shown) {
        s_stream_shown = false;
        s_used_at = s_stream_used_at;   /* the overlay over it: its 4 seconds go on */
    }
    if (!s_active) {
        s_active = true;
        tp_log("on: Aurora's screens have the input");
    }
#if SDL_VERSION_ATLEAST(2, 0, 14)
    const Uint32 now = SDL_GetTicks();
    const size_t slots = input->max_num_gamepads < PADS ? input->max_num_gamepads : PADS;
#if defined(TARGET_WEBOS)
    match_bridged(input, slots, now);
#endif
    const float screen_w = (float) lv_disp_get_hor_res(NULL);
    const float screen_h = (float) lv_disp_get_ver_res(NULL);
    const float k = screen_w * SPEED_SCREENS;
    pass_t pass = {false, false, false};
    for (size_t i = 0; i < slots; ++i) {
        app_gamepad_state_t *pad = &input->gamepads[i];
        pad_t *p = &s_pads[i];
        if (pad->instance_id == -1 || pad->controller == NULL) {
            forget(p);
            p->seen = false;
            continue;
        }
        if (p->bridged) {
            forget(p);
            continue;
        }
        SDL_GameController *controller = pad->controller;
        const SDL_GameControllerType type = SDL_GameControllerGetType(controller);
        /* ⓘ Sony's vendor id as well as SDL's type: an SDL that does not
         * know the Edge yet names no type for it. */
        const bool playstation = type == SDL_CONTROLLER_TYPE_PS4 || type == SDL_CONTROLLER_TYPE_PS5 ||
                                 SDL_GameControllerGetVendor(controller) == 0x054c;
        const int touchpads = SDL_GameControllerGetNumTouchpads(controller);
        if (!p->seen) {
            p->seen = true;
            tp_log("pad %d: %s, type %d, vendor %04x, %d touchpad(s)%s", (int) i,
                   SDL_GameControllerName(controller) ? SDL_GameControllerName(controller) : "?", (int) type,
                   SDL_GameControllerGetVendor(controller), touchpads,
                   playstation && touchpads > 0 ? "" : ": not one the pointer reads");
        }
        if (!playstation || touchpads < 1) {
            forget(p);
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
        const bool pad_in = SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_TOUCHPAD) != 0;
        const float aspect = type == SDL_CONTROLLER_TYPE_PS4 ? DS4_ASPECT : DS5_ASPECT;
        feed(p, down, fx, fy, aspect, -1, pad_in, now, k, &pass);
    }
#if defined(TARGET_WEBOS)
    feed_bridged(now, k, &pass);
#endif
    if (s_px < 0) s_px = 0;
    if (s_py < 0) s_py = 0;
    if (s_px > screen_w - 1) s_px = screen_w - 1;
    if (s_py > screen_h - 1) s_py = screen_h - 1;
    bool pressed = pass.pressed;
    bool moved = pass.moved;
    if (pass.tapped) {
        s_tap_until = now + TAP_CLICK_MS;
    }
    if (s_tap_until != 0 && (Sint32) (now - s_tap_until) < 0) {
        pressed = true;
    } else {
        s_tap_until = 0;
    }
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
        if (!s_shown) {
            tp_log("shown at %d,%d", (int) s_px, (int) s_py);
        }
        show(true);
    } else if (s_shown && (Sint32) (now - s_used_at) >= HIDE_AFTER_MS) {
        show(false);
    }
#endif
}

bool touchpad_pointer_stream_move(stream_input_t *input, float dx, float dy) {
    if (!tv_cursor_mode_on() || s_indev == NULL || s_cursor == NULL || input == NULL || input->session == NULL) {
        return false;
    }
    const float screen_w = (float) lv_disp_get_hor_res(NULL);
    const float screen_h = (float) lv_disp_get_ver_res(NULL);
    if (screen_w < 1 || screen_h < 1) {
        return false;
    }
    /* The host's frame, as a connected mouse's point is given in it
     * (stream/input/session_mouse.c): the window's. */
    const float ref_w = input->session->display_width > 0 ? (float) input->session->display_width : screen_w;
    const float ref_h = input->session->display_height > 0 ? (float) input->session->display_height : screen_h;
    /* A connected mouse moved since: carry on from where it left the pointer. */
    int mx = 0, my = 0;
    SDL_GetMouseState(&mx, &my);
    if (mx != s_mouse_x || my != s_mouse_y) {
        if (s_mouse_x >= 0) {
            s_px = (float) mx * screen_w / ref_w;
            s_py = (float) my * screen_h / ref_h;
        }
        s_mouse_x = mx;
        s_mouse_y = my;
    }
    const float k = screen_w * SPEED_SCREENS;
    s_px += dx * k;
    s_py += dy * k;
    if (s_px < 0) s_px = 0;
    if (s_py < 0) s_py = 0;
    if (s_px > screen_w - 1) s_px = screen_w - 1;
    if (s_py > screen_h - 1) s_py = screen_h - 1;
    LiSendMousePositionEvent((short) (s_px * ref_w / screen_w), (short) (s_py * ref_h / screen_h), (short) ref_w,
                             (short) ref_h);
    s_stream_used_at = SDL_GetTicks();
    if (!s_stream_shown) {
        s_stream_shown = true;
        tp_log("stream: the touchpad moves the TV cursor, from %d,%d", (int) s_px, (int) s_py);
    }
    show(true);
    return true;
}
