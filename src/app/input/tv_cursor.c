/* The TV's pointer in Aurora, with its tip as the hot spot. The why is in the
 * header. */

#include "tv_cursor.h"

#include "app.h"   /* the app, its settings and the mouse grab */
#include "stream/session.h"   /* whether the stream has the input */

static void cursor_log(const char *fmt, ...);

#if defined(TARGET_WEBOS)

#include <math.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <SDL.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* What happened, where it can be read over SSH: commons_log goes to PmLog,
 * which a C1 or a C3 does not let the developer account read. */
#define TV_CURSOR_LOG "tv-cursor.log"
FILE *ctm_log_open(const char *name, const char *mode);

/* ⭐⭐ THE DROP, DRAWN (rhoquinn8217, 2026-10-11: "Is it possible to make the
 * tip of the teardrop sharper?", then "yes make the tip sharper"). webOS's
 * own picture has a rounded tip, and no edit of its pixels makes a point of
 * it. So Aurora draws the same drop: two straight edges from a point to a
 * round end, a white line, nothing inside, and a soft dark edge outside so the
 * line shows on a light picture. Measured from webOS's medium pointer
 * (/usr/share/im/cursorTypeAszMstN.png, 60 x 85): edges at 22 and 75 degrees,
 * a round end of radius 19.3, and nothing of LG's is read or copied. In the
 * drop's own pixels, which are the TV's, with the tip on a pixel corner. */
#define DROP_W 60
#define DROP_H 64
#define DROP_TIP_X 8.0
#define DROP_TIP_Y 8.0
#define DROP_CX 36.43
#define DROP_CY 40.14
#define DROP_R 19.32
#define DROP_HALO 2.0
#define DROP_HALO_ALPHA 0.45
#define DROP_SAMPLES 4   /* each pixel sampled 4 x 4 times: the edges are smooth */

/* ⭐ WHERE THE TIP GOES, AND HOW THIN THE LINE IS (rhoquinn8217, 2026-10-11:
 * "Make the outline thinner and move it slightly down and to the right").
 * The line is 3 of the drop's pixels less `thin`; webOS's is about 3.
 * ⭐ THE SHIFT IS IN THE TV'S OWN PIXELS, right and down, so a step can be one
 * of them: in the app's it was two, and the right place fell between two
 * tries (rhoquinn8217: "over jumped"). The drop is placed that far over before
 * the shrink, whose averaging draws the half step. With the tip on a pixel
 * corner, 0,0 puts it on the point the host is told: where it was found by eye
 * on the C1, 2026-10-11, with webOS's rounded tip. ⭐ The shift a TV uses is
 * its own, saved by Align TV cursor (the header says where), and -1, 3 until
 * then. ⓘ Both can be tried live from the control port (`cursor`), and are
 * not saved there. */
#define TV_CURSOR_THIN 1

/* Clear room around the drop, in its own pixels, for the shift and the hot
 * spot. */
#define TV_CURSOR_MARGIN TV_CURSOR_SHIFT_MAX

static bool s_ready = false;           /* the cursor is Aurora's */
static double s_scale = 1.0;           /* how much the TV enlarges the app */
static int s_shift_x = 0, s_shift_y = 0;
static int s_thin = TV_CURSOR_THIN;
static SDL_Surface *s_image = NULL;    /* the cursor's picture */
static SDL_Cursor *s_cursor = NULL;
static int s_hot_x = 0, s_hot_y = 0;

/* ⭐ PUT BACK WHEN THE WINDOW HAS THE SCREEN AGAIN (rhoquinn8217, 2026-10-11:
 * "I suspect that something may reset the orientation of the cursor"). webOS
 * draws the pointer, and an app's cursor is only a request to it; a window
 * shown again, given the focus again or entered again by the pointer asks for
 * it once more. ⓘ Noticed in SDL's event watch, wherever SDL is when it sends
 * the event, and done on the app's loop (tv_cursor_tick). */
static SDL_atomic_t s_reapply;         /* the window event that asked, 0 for none */
static int s_reapplied = 0;            /* how many times, for the log, which stops saying so after a few */
#define TV_CURSOR_REAPPLY_LOGGED 20

static void cursor_log(const char *fmt, ...) {
    FILE *f = ctm_log_open(TV_CURSOR_LOG, "a");
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

/* How far q is from the segment a-b. */
static double seg_dist(double qx, double qy, double ax, double ay, double bx, double by) {
    const double vx = bx - ax, vy = by - ay;
    double t = ((qx - ax) * vx + (qy - ay) * vy) / (vx * vx + vy * vy);
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    return hypot(qx - (ax + t * vx), qy - (ay + t * vy));
}

/* Which side of the line p1-p2 q is on. */
static double side(double x1, double y1, double x2, double y2, double qx, double qy) {
    return (x2 - x1) * (qy - y1) - (y2 - y1) * (qx - x1);
}

/* The drop: the round end and the triangle from the tip to where the edges
 * touch it. A sample inside and within `width` of the edge is the white line,
 * one outside and within the halo is the shadow, fading out. */
static SDL_Surface *draw_drop(double width) {
    SDL_Surface *s = SDL_CreateRGBSurfaceWithFormat(0, DROP_W, DROP_H, 32, SDL_PIXELFORMAT_RGBA32);
    if (s == NULL || SDL_LockSurface(s) != 0) {
        if (s != NULL) SDL_FreeSurface(s);
        return NULL;
    }
    const double tx = DROP_TIP_X, ty = DROP_TIP_Y, cx = DROP_CX, cy = DROP_CY, r = DROP_R;
    const double d = hypot(cx - tx, cy - ty);
    const double phi = acos(r / d);                  /* at the centre, from the tip to a touch point */
    const double base = atan2(ty - cy, tx - cx);     /* from the centre to the tip */
    const double ax = cx + r * cos(base + phi), ay = cy + r * sin(base + phi);
    const double bx = cx + r * cos(base - phi), by = cy + r * sin(base - phi);
    const int n = DROP_SAMPLES * DROP_SAMPLES;
    for (int y = 0; y < DROP_H; ++y) {
        Uint8 *row = (Uint8 *) s->pixels + (size_t) y * (size_t) s->pitch;
        for (int x = 0; x < DROP_W; ++x) {
            double alpha = 0, white = 0;
            for (int j = 0; j < DROP_SAMPLES; ++j) {
                for (int i = 0; i < DROP_SAMPLES; ++i) {
                    const double qx = x + (i + 0.5) / DROP_SAMPLES;
                    const double qy = y + (j + 0.5) / DROP_SAMPLES;
                    const double dc = hypot(qx - cx, qy - cy);
                    const double s1 = side(tx, ty, ax, ay, qx, qy);
                    const double s2 = side(ax, ay, bx, by, qx, qy);
                    const double s3 = side(bx, by, tx, ty, qx, qy);
                    const bool in_tri = (s1 >= 0 && s2 >= 0 && s3 >= 0) || (s1 <= 0 && s2 <= 0 && s3 <= 0);
                    const bool inside = dc <= r || in_tri;
                    const double d1 = seg_dist(qx, qy, tx, ty, ax, ay);
                    const double d2 = seg_dist(qx, qy, tx, ty, bx, by);
                    double diff = atan2(qy - cy, qx - cx) - base;
                    while (diff > M_PI) diff -= 2 * M_PI;
                    while (diff < -M_PI) diff += 2 * M_PI;
                    const double d3 = fabs(diff) >= phi ? fabs(dc - r)
                                                        : fmin(hypot(qx - ax, qy - ay), hypot(qx - bx, qy - by));
                    const double dist = fmin(d1, fmin(d2, d3));
                    if (inside && dist <= width) {
                        alpha += 1.0;
                        white += 1.0;
                    } else if (!inside && dist <= DROP_HALO) {
                        alpha += DROP_HALO_ALPHA * (1.0 - dist / DROP_HALO);
                    }
                }
            }
            Uint8 *p = row + x * 4;
            const Uint8 c = alpha > 0 ? (Uint8) lround(255.0 * white / alpha) : 0;
            p[0] = p[1] = p[2] = c;
            p[3] = (Uint8) lround(255.0 * alpha / n);
        }
    }
    SDL_UnlockSurface(s);
    return s;
}

/* ⭐ SMALLER, BY AS MUCH AS THE TV ENLARGES AURORA (rhoquinn8217, 2026-10-11,
 * on the first try: "make it smaller and sharper"). webOS draws its own
 * pointer at the panel's resolution, and an app's cursor at the app's, then
 * enlarges it with the app: on the C1 a 1920 x 1080 app on a 3840 x 2160
 * panel, so the pointer came out twice its size and soft. So the drop is
 * shrunk by that factor first, each pixel the average of the ones it covers,
 * in premultiplied alpha so the white line does not darken. ⓘ It cannot be as
 * fine as webOS's own: the app draws at its own resolution. */
static SDL_Surface *shrink(SDL_Surface *src, double scale) {
    const int w = (int) lround(src->w / scale);
    const int h = (int) lround(src->h / scale);
    if (w < 1 || h < 1) {
        return NULL;
    }
    SDL_Surface *dst = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_RGBA32);
    if (dst == NULL) {
        return NULL;
    }
    if (SDL_LockSurface(src) != 0) {
        SDL_FreeSurface(dst);
        return NULL;
    }
    if (SDL_LockSurface(dst) != 0) {
        SDL_UnlockSurface(src);
        SDL_FreeSurface(dst);
        return NULL;
    }
    memset(dst->pixels, 0, (size_t) dst->h * (size_t) dst->pitch);
    for (int y = 0; y < h; ++y) {
        int y0 = (int) floor(y * scale), y1 = (int) floor((y + 1) * scale);
        if (y1 <= y0) y1 = y0 + 1;
        if (y1 > src->h) y1 = src->h;
        Uint8 *out = (Uint8 *) dst->pixels + (size_t) y * (size_t) dst->pitch;
        for (int x = 0; x < w; ++x) {
            int x0 = (int) floor(x * scale), x1 = (int) floor((x + 1) * scale);
            if (x1 <= x0) x1 = x0 + 1;
            if (x1 > src->w) x1 = src->w;
            double r = 0, g = 0, b = 0, a = 0;
            int n = 0;
            for (int sy = y0; sy < y1; ++sy) {
                const Uint8 *row = (const Uint8 *) src->pixels + (size_t) sy * (size_t) src->pitch;
                for (int sx = x0; sx < x1; ++sx) {
                    const Uint8 *p = row + sx * 4;
                    r += p[0] * (double) p[3];
                    g += p[1] * (double) p[3];
                    b += p[2] * (double) p[3];
                    a += p[3];
                    ++n;
                }
            }
            Uint8 *q = out + x * 4;
            if (n == 0 || a <= 0) {
                continue;
            }
            q[0] = (Uint8) lround(r / a);
            q[1] = (Uint8) lround(g / a);
            q[2] = (Uint8) lround(b / a);
            q[3] = (Uint8) lround(a / n);
        }
    }
    SDL_UnlockSurface(dst);
    SDL_UnlockSurface(src);
    return dst;
}

/* The cursor with the shift and the line that stand now. False, and the
 * cursor that was there stays, when it cannot be made. */
static bool build(void) {
    double width = 3.0 - s_thin;
    if (width < 1.0) width = 1.0;
    SDL_Surface *drop = draw_drop(width);
    if (drop == NULL) {
        cursor_log("the drop could not be drawn (%s)", SDL_GetError());
        return false;
    }
    /* The drop placed the shift over in a clear canvas, the hot spot where
     * its tip was before the shift. */
    const int m = TV_CURSOR_MARGIN;
    const int dx = s_shift_x < -m ? -m : (s_shift_x > m ? m : s_shift_x);
    const int dy = s_shift_y < -m ? -m : (s_shift_y > m ? m : s_shift_y);
    SDL_Surface *canvas = SDL_CreateRGBSurfaceWithFormat(0, drop->w + 2 * m, drop->h + 2 * m, 32,
                                                         SDL_PIXELFORMAT_RGBA32);
    if (canvas == NULL) {
        cursor_log("the canvas could not be made (%s)", SDL_GetError());
        SDL_FreeSurface(drop);
        return false;
    }
    SDL_FillRect(canvas, NULL, 0);
    SDL_SetSurfaceBlendMode(drop, SDL_BLENDMODE_NONE);
    SDL_Rect at = {m + dx, m + dy, drop->w, drop->h};
    SDL_BlitSurface(drop, NULL, canvas, &at);
    SDL_FreeSurface(drop);
    const double scale = s_scale > 1.0 ? s_scale : 1.0;
    SDL_Surface *image = shrink(canvas, scale);
    SDL_FreeSurface(canvas);
    if (image == NULL) {
        cursor_log("the shrink failed (%s)", SDL_GetError());
        return false;
    }
    int hot_x = (int) floor((DROP_TIP_X + m) / scale);
    int hot_y = (int) floor((DROP_TIP_Y + m) / scale);
    if (hot_x >= image->w) hot_x = image->w - 1;
    if (hot_y >= image->h) hot_y = image->h - 1;
    SDL_Cursor *cursor = SDL_CreateColorCursor(image, hot_x, hot_y);
    if (cursor == NULL) {
        cursor_log("SDL refused the cursor (%s)", SDL_GetError());
        SDL_FreeSurface(image);
        return false;
    }
    SDL_SetCursor(cursor);
    if (s_cursor != NULL) {
        SDL_FreeCursor(s_cursor);
    }
    if (s_image != NULL) {
        SDL_FreeSurface(s_image);
    }
    s_cursor = cursor;
    s_image = image;
    s_hot_x = hot_x;
    s_hot_y = hot_y;
    s_ready = true;
    cursor_log("cursor made: %dx%d, hot spot %d,%d, shifted %d,%d of the TV's pixels (right, down), line %.1f",
               image->w, image->h, hot_x, hot_y, s_shift_x, s_shift_y, width);
    return true;
}

static int clamp_shift(int v) {
    return v < -TV_CURSOR_SHIFT_MAX ? -TV_CURSOR_SHIFT_MAX : (v > TV_CURSOR_SHIFT_MAX ? TV_CURSOR_SHIFT_MAX : v);
}

static int SDLCALL window_watch(void *userdata, SDL_Event *event) {
    (void) userdata;
    if (event->type == SDL_WINDOWEVENT) {
        switch (event->window.event) {
            case SDL_WINDOWEVENT_SHOWN:
            case SDL_WINDOWEVENT_RESTORED:
            case SDL_WINDOWEVENT_FOCUS_GAINED:
            case SDL_WINDOWEVENT_ENTER:
                SDL_AtomicSet(&s_reapply, event->window.event);
                break;
            default:
                break;
        }
    }
    return 1;
}

static const char *window_event_name(int event) {
    switch (event) {
        case SDL_WINDOWEVENT_SHOWN:
            return "was shown again";
        case SDL_WINDOWEVENT_RESTORED:
            return "was restored";
        case SDL_WINDOWEVENT_FOCUS_GAINED:
            return "has the focus again";
        case SDL_WINDOWEVENT_ENTER:
            return "has the pointer again";
        default:
            return "asked";
    }
}

void tv_cursor_init(SDL_Window *window, int shift_x, int shift_y) {
    FILE *f = ctm_log_open(TV_CURSOR_LOG, "w");   /* a few lines a start: begun again each time */
    if (f != NULL) {
        fclose(f);
    }
    s_shift_x = clamp_shift(shift_x);
    s_shift_y = clamp_shift(shift_y);
    /* How much the TV enlarges Aurora: the panel against the window. */
    int win_w = 0, win_h = 0, panel_w = 0, panel_h = 0;
    if (window != NULL) {
        SDL_GetWindowSize(window, &win_w, &win_h);
    }
    if (win_w > 0 && SDL_webOSGetPanelResolution(&panel_w, &panel_h) && panel_w > win_w) {
        s_scale = (double) panel_w / (double) win_w;
    }
    cursor_log("the drop for a window %dx%d on a %dx%d panel, shrunk %.2f times",
               win_w, win_h, panel_w, panel_h, s_scale);
    if (!build()) {
        cursor_log("webOS's own pointer stays");
        return;
    }
    SDL_AtomicSet(&s_reapply, 0);
    SDL_AddEventWatch(window_watch, NULL);
}

void tv_cursor_deinit(void) {
    SDL_DelEventWatch(window_watch, NULL);
    if (s_cursor != NULL) {
        SDL_SetCursor(SDL_GetDefaultCursor());
        SDL_FreeCursor(s_cursor);
        s_cursor = NULL;
    }
    if (s_image != NULL) {
        SDL_FreeSurface(s_image);
        s_image = NULL;
    }
    s_ready = false;
}

void tv_cursor_tick(void) {
    const int why = SDL_AtomicSet(&s_reapply, 0);
    if (why == 0 || !s_ready || s_cursor == NULL) {
        return;
    }
    /* ⓘ Ours again if something else was set; if it is still ours, NULL is
     * SDL's own way to have the cursor that stands drawn again. Either way SDL
     * leaves it hidden where it is hidden: relative mode, or the stream's. */
    if (SDL_GetCursor() != s_cursor) {
        SDL_SetCursor(s_cursor);
    } else {
        SDL_SetCursor(NULL);
    }
    if (s_reapplied < TV_CURSOR_REAPPLY_LOGGED) {
        cursor_log("cursor set again: the window %s%s", window_event_name(why),
                   s_reapplied + 1 == TV_CURSOR_REAPPLY_LOGGED ? " (no more of these lines this run)" : "");
    }
    s_reapplied++;
}

bool tv_cursor_adjust(int shift_x, int shift_y, int thin) {
    if (!s_ready) {
        return false;
    }
    s_shift_x = clamp_shift(shift_x);
    s_shift_y = clamp_shift(shift_y);
    if (thin >= 0) {
        s_thin = thin > 2 ? 2 : thin;   /* below 0: keep the line that stands */
    }
    return build();
}

void tv_cursor_get_shift(int *shift_x, int *shift_y) {
    if (shift_x != NULL) *shift_x = s_shift_x;
    if (shift_y != NULL) *shift_y = s_shift_y;
}

bool tv_cursor_keep(void) {
    if (!s_ready || app_configuration == NULL) {
        return false;
    }
    app_configuration->bridge.tv_cursor_x = s_shift_x;
    app_configuration->bridge.tv_cursor_y = s_shift_y;
    settings_save(app_configuration);
    cursor_log("kept as this TV's: shifted %d,%d of the TV's pixels (right, down)", s_shift_x, s_shift_y);
    return true;
}

bool tv_cursor_drawn(void) {
    return s_ready;
}

void tv_cursor_describe(char *out, size_t len) {
    const char *mode = tv_cursor_mode_on() ? "on" : (tv_cursor_mode_switchable() ? "off" : "off(hardware-mouse)");
    const int saved_x = app_configuration != NULL ? app_configuration->bridge.tv_cursor_x : 0;
    const int saved_y = app_configuration != NULL ? app_configuration->bridge.tv_cursor_y : 0;
    if (!s_ready || s_image == NULL) {
        snprintf(out, len, "tv_cursor=%s, webOS's own pointer (Aurora's could not be made: see logs/tv-cursor.log)",
                 mode);
        return;
    }
    snprintf(out, len, "tv_cursor=%s shift=%d,%d saved=%d,%d thin=%d hot=%d,%d image=%dx%d scale=%.2f reapplied=%d",
             mode, s_shift_x, s_shift_y, saved_x, saved_y, s_thin, s_hot_x, s_hot_y, s_image->w, s_image->h,
             s_scale, s_reapplied);
}

#else

#include <stdio.h>

static void cursor_log(const char *fmt, ...) {
    (void) fmt;
}

void tv_cursor_init(SDL_Window *window, int shift_x, int shift_y) {
    (void) window;
    (void) shift_x;
    (void) shift_y;
}

void tv_cursor_deinit(void) {
}

void tv_cursor_tick(void) {
}

bool tv_cursor_adjust(int shift_x, int shift_y, int thin) {
    (void) shift_x;
    (void) shift_y;
    (void) thin;
    return false;
}

void tv_cursor_get_shift(int *shift_x, int *shift_y) {
    if (shift_x != NULL) *shift_x = 0;
    if (shift_y != NULL) *shift_y = 0;
}

bool tv_cursor_keep(void) {
    return false;
}

bool tv_cursor_drawn(void) {
    return false;
}

void tv_cursor_describe(char *out, size_t len) {
    snprintf(out, len, "tv_cursor=%s, not on webOS", tv_cursor_mode_on() ? "on" : "off");
}

#endif

/* ⭐ THE SWITCH ITSELF IS UPSTREAM'S, "Absolute mouse mode" (Settings, Input),
 * so the menu, the shortcut and Settings all show the same thing, and it is
 * saved as that: the next stream starts the way this one was left
 * (rhoquinn8217, 2026-10-11, asked whether it should: "yes"). ⓘ Saved at once
 * rather than when the app leaves Settings or closes, as the others are: an
 * install or a crash would lose it otherwise. */
bool tv_cursor_mode_on(void) {
    return app_configuration != NULL && app_configuration->absmouse && !app_configuration->hardware_mouse;
}

bool tv_cursor_mode_switchable(void) {
    return app_configuration != NULL && !app_configuration->hardware_mouse;
}

bool tv_cursor_set_mode(bool on) {
    if (!tv_cursor_mode_switchable()) {
        cursor_log("the TV cursor cannot be switched while Use mouse hardware is on");
        return false;
    }
    if (app_configuration->absmouse != on) {
        app_configuration->absmouse = on;
        settings_save(app_configuration);
    }
    /* ⓘ The stream holds the mouse while nothing of the interface is up, and
     * is given the new mode at once. Otherwise the interface has it, and gives
     * it back in the new mode as it closes (app_set_mouse_grab, in the
     * overlay's close). */
    if (global != NULL && global->session != NULL && session_accepting_input(global->session)) {
        app_set_mouse_grab(&global->input, true);
    }
    cursor_log("TV cursor %s: the host is told %s", on ? "on" : "off",
               on ? "where the cursor is" : "only how the mouse moved, and nothing stops at the screen's edge");
    return true;
}

bool tv_cursor_toggle_mode(void) {
    return tv_cursor_set_mode(!tv_cursor_mode_on());
}
