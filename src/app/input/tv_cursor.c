/* The TV's pointer in Aurora, with its tip as the hot spot. The why is in the
 * header. */

#include "tv_cursor.h"

#if defined(TARGET_WEBOS)

#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include <SDL.h>
#include <SDL_image.h>

/* webOS's own pointer, the normal state of the medium size. ⓘ The small and
 * large ones sit beside it (cursorTypeAszSstN.png, cursorTypeAszLstN.png);
 * the TV's pointer-size setting is not followed yet. */
#define TV_CURSOR_IMAGE "/usr/share/im/cursorTypeAszMstN.png"

/* What happened, where it can be read over SSH: commons_log goes to PmLog,
 * which a C1 or a C3 does not let the developer account read. */
#define TV_CURSOR_LOG "tv-cursor.log"
FILE *ctm_log_open(const char *name, const char *mode);

static SDL_Surface *s_image = NULL;
static SDL_Cursor *s_cursor = NULL;

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

/* The tip: of the pixels at least half opaque, the one furthest up and to the
 * left (the smallest x + y; on a tie, the one nearer the diagonal). The
 * teardrop points up and to the left, and its shadow falls the other way. */
static bool find_tip(SDL_Surface *rgba, int *tip_x, int *tip_y) {
    bool found = false;
    int best_sum = INT_MAX, best_off = INT_MAX;
    if (SDL_LockSurface(rgba) == 0) {
        for (int y = 0; y < rgba->h; ++y) {
            const Uint8 *row = (const Uint8 *) rgba->pixels + (size_t) y * (size_t) rgba->pitch;
            for (int x = 0; x < rgba->w; ++x) {
                if (row[x * 4 + 3] < 128) {
                    continue;
                }
                const int sum = x + y;
                const int off = abs(x - y);
                if (sum < best_sum || (sum == best_sum && off < best_off)) {
                    best_sum = sum;
                    best_off = off;
                    *tip_x = x;
                    *tip_y = y;
                    found = true;
                }
            }
        }
        SDL_UnlockSurface(rgba);
    }
    return found;
}

/* ⭐ SMALLER, BY AS MUCH AS THE TV ENLARGES AURORA (rhoquinn8217, 2026-10-11,
 * on the first try: "make it smaller and sharper"). webOS draws its own
 * pointer at the panel's resolution, and an app's cursor at the app's, then
 * enlarges it with the app: on the C1 a 1920 x 1080 app on a 3840 x 2160
 * panel, so the pointer came out twice its size and soft. So the image is
 * shrunk by that factor first, each pixel the average of the ones it covers,
 * in premultiplied alpha so the white edge does not darken; enlarged again,
 * it is webOS's size. ⓘ It cannot be as fine as webOS's own: the app draws at
 * its own resolution. */
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
                q[0] = q[1] = q[2] = q[3] = 0;
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

void tv_cursor_init(SDL_Window *window) {
    FILE *f = ctm_log_open(TV_CURSOR_LOG, "w");   /* a few lines a start: begun again each time */
    if (f != NULL) {
        fclose(f);
    }
    SDL_Surface *loaded = IMG_Load(TV_CURSOR_IMAGE);
    if (loaded == NULL) {
        cursor_log("%s could not be read (%s): webOS's own pointer stays", TV_CURSOR_IMAGE, IMG_GetError());
        return;
    }
    SDL_Surface *full = SDL_ConvertSurfaceFormat(loaded, SDL_PIXELFORMAT_RGBA32, 0);
    SDL_FreeSurface(loaded);
    if (full == NULL) {
        cursor_log("%s could not be converted (%s): webOS's own pointer stays", TV_CURSOR_IMAGE, SDL_GetError());
        return;
    }
    int tip_x = 0, tip_y = 0;
    if (!find_tip(full, &tip_x, &tip_y)) {
        cursor_log("%s (%dx%d): no tip found: webOS's own pointer stays", TV_CURSOR_IMAGE, full->w, full->h);
        SDL_FreeSurface(full);
        return;
    }

    /* How much the TV enlarges Aurora: the panel against the window. */
    int win_w = 0, win_h = 0, panel_w = 0, panel_h = 0;
    if (window != NULL) {
        SDL_GetWindowSize(window, &win_w, &win_h);
    }
    double scale = 1.0;
    if (win_w > 0 && SDL_webOSGetPanelResolution(&panel_w, &panel_h) && panel_w > win_w) {
        scale = (double) panel_w / (double) win_w;
    }
    s_image = full;
    int hot_x = tip_x, hot_y = tip_y;
    if (scale > 1.0) {
        SDL_Surface *small = shrink(full, scale);
        if (small != NULL) {
            SDL_FreeSurface(full);
            s_image = small;
            hot_x = (int) floor(tip_x / scale);
            hot_y = (int) floor(tip_y / scale);
            if (hot_x >= s_image->w) hot_x = s_image->w - 1;
            if (hot_y >= s_image->h) hot_y = s_image->h - 1;
        } else {
            cursor_log("the shrink failed: the pointer is used at its full size");
        }
    }

    s_cursor = SDL_CreateColorCursor(s_image, hot_x, hot_y);
    if (s_cursor == NULL) {
        cursor_log("SDL refused the cursor (%s): webOS's own pointer stays", SDL_GetError());
        SDL_FreeSurface(s_image);
        s_image = NULL;
        return;
    }
    SDL_SetCursor(s_cursor);
    cursor_log("webOS's pointer %s set as Aurora's: window %dx%d on a %dx%d panel, shrunk %.2f times to %dx%d, "
               "its tip %d,%d (%d,%d in the full image) the hot spot",
               TV_CURSOR_IMAGE, win_w, win_h, panel_w, panel_h, scale, s_image->w, s_image->h,
               hot_x, hot_y, tip_x, tip_y);
}

void tv_cursor_deinit(void) {
    if (s_cursor != NULL) {
        SDL_SetCursor(SDL_GetDefaultCursor());
        SDL_FreeCursor(s_cursor);
        s_cursor = NULL;
    }
    if (s_image != NULL) {
        SDL_FreeSurface(s_image);
        s_image = NULL;
    }
}

#else

void tv_cursor_init(SDL_Window *window) {
    (void) window;
}

void tv_cursor_deinit(void) {
}

#endif
