/* The TV's pointer in Aurora, with its tip as the hot spot. The why is in the
 * header. */

#include "tv_cursor.h"

#if defined(TARGET_WEBOS)

#include <limits.h>
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
static bool find_tip(SDL_Surface *image, int *tip_x, int *tip_y) {
    SDL_Surface *rgba = SDL_ConvertSurfaceFormat(image, SDL_PIXELFORMAT_RGBA32, 0);
    if (rgba == NULL) {
        return false;
    }
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
    SDL_FreeSurface(rgba);
    return found;
}

void tv_cursor_init(void) {
    FILE *f = ctm_log_open(TV_CURSOR_LOG, "w");   /* a few lines a start: begun again each time */
    if (f != NULL) {
        fclose(f);
    }
    s_image = IMG_Load(TV_CURSOR_IMAGE);
    if (s_image == NULL) {
        cursor_log("%s could not be read (%s): webOS's own pointer stays", TV_CURSOR_IMAGE, IMG_GetError());
        return;
    }
    int tip_x = 0, tip_y = 0;
    if (!find_tip(s_image, &tip_x, &tip_y)) {
        cursor_log("%s (%dx%d): no tip found: webOS's own pointer stays", TV_CURSOR_IMAGE, s_image->w, s_image->h);
        SDL_FreeSurface(s_image);
        s_image = NULL;
        return;
    }
    s_cursor = SDL_CreateColorCursor(s_image, tip_x, tip_y);
    if (s_cursor == NULL) {
        cursor_log("SDL refused the cursor (%s): webOS's own pointer stays", SDL_GetError());
        SDL_FreeSurface(s_image);
        s_image = NULL;
        return;
    }
    SDL_SetCursor(s_cursor);
    cursor_log("webOS's pointer %s (%dx%d) set as Aurora's, its tip %d,%d the hot spot",
               TV_CURSOR_IMAGE, s_image->w, s_image->h, tip_x, tip_y);
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

void tv_cursor_init(void) {
}

void tv_cursor_deinit(void) {
}

#endif
