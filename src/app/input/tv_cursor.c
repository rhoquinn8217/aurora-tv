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
#include <string.h>
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

/* ⭐ WHERE THE TIP GOES, AND HOW THIN THE OUTLINE IS (rhoquinn8217,
 * 2026-10-11, on the second try: "Still slightly off. Make the outline
 * thinner and move it slightly down and to the right. Also the area inside
 * the tear drop need to be completely transparent"). The shift is in the
 * app's pixels, down and to the right; the thinning is in the picture's own
 * pixels, taken off the outline's inner side. ⓘ Both can be tried live from
 * the control port (`cursor`), and are not saved there. */
#define TV_CURSOR_SHIFT_X 1
#define TV_CURSOR_SHIFT_Y 1
#define TV_CURSOR_THIN 1

/* Clear room around the picture, so the hot spot can move past its edge. */
#define TV_CURSOR_PAD 8

static SDL_Surface *s_full = NULL;     /* webOS's picture, as read */
static int s_tip_x = 0, s_tip_y = 0;   /* its tip, in its own pixels */
static double s_scale = 1.0;           /* how much the TV enlarges the app */
static int s_shift_x = TV_CURSOR_SHIFT_X, s_shift_y = TV_CURSOR_SHIFT_Y;
static int s_thin = TV_CURSOR_THIN;
static SDL_Surface *s_image = NULL;    /* the cursor's picture */
static SDL_Cursor *s_cursor = NULL;
static int s_hot_x = 0, s_hot_y = 0;

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
 * teardrop points up and to the left. */
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

/* ⭐ NOTHING INSIDE THE DROP, AND A THINNER LINE. webOS's picture is a white
 * outline about three pixels wide around a black fill at about 30% and a soft
 * dark edge outside. Here the outline is every pixel at least middle grey
 * (its inner edge blends into the fill), everything the picture's border
 * reaches without crossing it is outside, and the rest is the inside:
 * cleared. Then `thin` times, each outline pixel beside the inside is
 * cleared too. ⓘ The dark edge outside stays: on a light picture it is what
 * keeps a white line visible. */
static void hollow(SDL_Surface *s, int thin) {
    const int w = s->w, h = s->h, n = w * h;
    Uint8 *kind = calloc((size_t) n, 1);   /* 0 not yet known, 1 outline, 2 outside, 3 inside */
    int *queue = malloc(sizeof(int) * (size_t) n);
    if (kind == NULL || queue == NULL || SDL_LockSurface(s) != 0) {
        free(kind);
        free(queue);
        return;
    }
    for (int y = 0; y < h; ++y) {
        const Uint8 *row = (const Uint8 *) s->pixels + (size_t) y * (size_t) s->pitch;
        for (int x = 0; x < w; ++x) {
            const Uint8 *p = row + x * 4;
            const int lum = (p[0] * 299 + p[1] * 587 + p[2] * 114) / 1000;
            if (p[3] >= 16 && lum >= 90) {
                kind[y * w + x] = 1;
            }
        }
    }
    int head = 0, tail = 0;
    for (int x = 0; x < w; ++x) {
        const int top = x, bottom = (h - 1) * w + x;
        if (kind[top] == 0) { kind[top] = 2; queue[tail++] = top; }
        if (kind[bottom] == 0) { kind[bottom] = 2; queue[tail++] = bottom; }
    }
    for (int y = 0; y < h; ++y) {
        const int left = y * w, right = y * w + w - 1;
        if (kind[left] == 0) { kind[left] = 2; queue[tail++] = left; }
        if (kind[right] == 0) { kind[right] = 2; queue[tail++] = right; }
    }
    while (head < tail) {
        const int i = queue[head++];
        const int x = i % w, y = i / w;
        const int next[4] = { x > 0 ? i - 1 : -1, x < w - 1 ? i + 1 : -1, y > 0 ? i - w : -1, y < h - 1 ? i + w : -1 };
        for (int k = 0; k < 4; ++k) {
            if (next[k] >= 0 && kind[next[k]] == 0) {
                kind[next[k]] = 2;
                queue[tail++] = next[k];
            }
        }
    }
    for (int i = 0; i < n; ++i) {
        if (kind[i] == 0) {
            kind[i] = 3;
        }
    }
    for (int pass = 0; pass < thin; ++pass) {
        int took = 0;
        for (int i = 0; i < n; ++i) {
            if (kind[i] != 1) {
                continue;
            }
            const int x = i % w, y = i / w;
            if ((x > 0 && kind[i - 1] == 3) || (x < w - 1 && kind[i + 1] == 3) ||
                (y > 0 && kind[i - w] == 3) || (y < h - 1 && kind[i + w] == 3)) {
                queue[took++] = i;
            }
        }
        for (int k = 0; k < took; ++k) {
            kind[queue[k]] = 3;
        }
    }
    for (int i = 0; i < n; ++i) {
        if (kind[i] == 3) {
            Uint8 *p = (Uint8 *) s->pixels + (size_t) (i / w) * (size_t) s->pitch + (size_t) (i % w) * 4;
            p[0] = p[1] = p[2] = p[3] = 0;
        }
    }
    SDL_UnlockSurface(s);
    free(kind);
    free(queue);
}

/* ⭐ SMALLER, BY AS MUCH AS THE TV ENLARGES AURORA (rhoquinn8217, 2026-10-11,
 * on the first try: "make it smaller and sharper"). webOS draws its own
 * pointer at the panel's resolution, and an app's cursor at the app's, then
 * enlarges it with the app: on the C1 a 1920 x 1080 app on a 3840 x 2160
 * panel, so the pointer came out twice its size and soft. So the picture is
 * shrunk by that factor first, each pixel the average of the ones it covers,
 * in premultiplied alpha so the white edge does not darken, into a canvas
 * with `pad` clear pixels around it. ⓘ It cannot be as fine as webOS's own:
 * the app draws at its own resolution. */
static SDL_Surface *shrink(SDL_Surface *src, double scale, int pad) {
    const int w = (int) lround(src->w / scale);
    const int h = (int) lround(src->h / scale);
    if (w < 1 || h < 1) {
        return NULL;
    }
    SDL_Surface *dst = SDL_CreateRGBSurfaceWithFormat(0, w + 2 * pad, h + 2 * pad, 32, SDL_PIXELFORMAT_RGBA32);
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
        Uint8 *out = (Uint8 *) dst->pixels + (size_t) (y + pad) * (size_t) dst->pitch;
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
            Uint8 *q = out + (x + pad) * 4;
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

/* The cursor from webOS's picture with the shift and the thinning that stand
 * now. False, and the cursor that was there stays, when it cannot be made. */
static bool build(void) {
    SDL_Surface *work = SDL_ConvertSurfaceFormat(s_full, SDL_PIXELFORMAT_RGBA32, 0);
    if (work == NULL) {
        cursor_log("the picture could not be copied (%s)", SDL_GetError());
        return false;
    }
    hollow(work, s_thin);
    const double scale = s_scale > 1.0 ? s_scale : 1.0;
    SDL_Surface *image = shrink(work, scale, TV_CURSOR_PAD);
    SDL_FreeSurface(work);
    if (image == NULL) {
        cursor_log("the shrink failed (%s)", SDL_GetError());
        return false;
    }
    int hot_x = (int) floor(s_tip_x / scale) + TV_CURSOR_PAD - s_shift_x;
    int hot_y = (int) floor(s_tip_y / scale) + TV_CURSOR_PAD - s_shift_y;
    if (hot_x < 0) hot_x = 0;
    if (hot_y < 0) hot_y = 0;
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
    cursor_log("cursor made: %dx%d, hot spot %d,%d, shifted %d,%d (down and right), outline thinned %d",
               image->w, image->h, hot_x, hot_y, s_shift_x, s_shift_y, s_thin);
    return true;
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
    s_full = SDL_ConvertSurfaceFormat(loaded, SDL_PIXELFORMAT_RGBA32, 0);
    SDL_FreeSurface(loaded);
    if (s_full == NULL) {
        cursor_log("%s could not be converted (%s): webOS's own pointer stays", TV_CURSOR_IMAGE, SDL_GetError());
        return;
    }
    if (!find_tip(s_full, &s_tip_x, &s_tip_y)) {
        cursor_log("%s (%dx%d): no tip found: webOS's own pointer stays", TV_CURSOR_IMAGE, s_full->w, s_full->h);
        SDL_FreeSurface(s_full);
        s_full = NULL;
        return;
    }
    /* How much the TV enlarges Aurora: the panel against the window. */
    int win_w = 0, win_h = 0, panel_w = 0, panel_h = 0;
    if (window != NULL) {
        SDL_GetWindowSize(window, &win_w, &win_h);
    }
    if (win_w > 0 && SDL_webOSGetPanelResolution(&panel_w, &panel_h) && panel_w > win_w) {
        s_scale = (double) panel_w / (double) win_w;
    }
    cursor_log("webOS's pointer %s (%dx%d, tip %d,%d) for a window %dx%d on a %dx%d panel, shrunk %.2f times",
               TV_CURSOR_IMAGE, s_full->w, s_full->h, s_tip_x, s_tip_y, win_w, win_h, panel_w, panel_h, s_scale);
    if (!build()) {
        cursor_log("webOS's own pointer stays");
    }
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
    if (s_full != NULL) {
        SDL_FreeSurface(s_full);
        s_full = NULL;
    }
}

bool tv_cursor_adjust(int shift_x, int shift_y, int thin) {
    if (s_full == NULL) {
        return false;
    }
    s_shift_x = shift_x;
    s_shift_y = shift_y;
    if (thin >= 0) {
        s_thin = thin > 3 ? 3 : thin;   /* below 0: keep the thinning that stands */
    }
    return build();
}

void tv_cursor_describe(char *out, size_t len) {
    if (s_full == NULL || s_cursor == NULL) {
        snprintf(out, len, "webOS's own pointer (Aurora's could not be made: see logs/tv-cursor.log)");
        return;
    }
    snprintf(out, len, "shift=%d,%d thin=%d hot=%d,%d image=%dx%d scale=%.2f",
             s_shift_x, s_shift_y, s_thin, s_hot_x, s_hot_y, s_image->w, s_image->h, s_scale);
}

#else

#include <stdio.h>

void tv_cursor_init(SDL_Window *window) {
    (void) window;
}

void tv_cursor_deinit(void) {
}

bool tv_cursor_adjust(int shift_x, int shift_y, int thin) {
    (void) shift_x;
    (void) shift_y;
    (void) thin;
    return false;
}

void tv_cursor_describe(char *out, size_t len) {
    snprintf(out, len, "not on webOS");
}

#endif
