/* The TV's pointer in Aurora, with its tip as the hot spot. The why is in the
 * header. */

#include "tv_cursor.h"

#include "app.h"   /* the app, its settings and the mouse grab */
#include "stream/session.h"   /* whether the stream has the input */
#include "mouse_seen.h"   /* whether a mouse is bridged */
#include "ui/streaming/bridge_prompt.h"   /* bridge_prompt_note */
#include "util/i18n.h"


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
#include <SDL_image.h>   /* the regular cursor: webOS's own picture */
#include <pbnjson.h>
#include "lunasynccall.h"   /* the TV's Pointer Size */

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

/* ⭐ THE REGULAR CURSOR (rhoquinn8217, 2026-10-12: "toggles between my small
 * sharp tv cursor and the regular cursor"): webOS's own pointer, read from the
 * TV as webOS draws it, and nothing of LG's copied into the app. The normal
 * state of the size the TV's Pointer Size setting names, small, medium or
 * large ("use whatever is set for the TV"), asked of the settings service as
 * the TV's own Quick Settings ask it (category option, key pointerSize), and
 * medium when it does not say. Its tip, the hot spot, is found in the picture.
 * ⓘ Asked again when the window has the screen back, as the setting is
 * changed in the TV's own menus. */
#define REGULAR_IMAGE "/usr/share/im/cursorTypeAsz%cstN.png"   /* S, M or L */

/* Clear room around the picture, in its own pixels, for the shift and the
 * hot spot. */
#define TV_CURSOR_MARGIN TV_CURSOR_SHIFT_MAX

enum { LOOK_DROP = 0, LOOK_REGULAR = 1 };

typedef struct {
    int x, y;
} shift_t;

static bool s_ready = false;           /* the cursor is Aurora's */
static double s_scale = 1.0;           /* how much the TV enlarges the app */
static int s_look = LOOK_DROP;         /* which cursor shows */
static shift_t s_shift[2];             /* each cursor's shift that stands, right and down */
static int s_thin = TV_CURSOR_THIN;
static SDL_Surface *s_regular = NULL;  /* webOS's picture, as read */
static char s_regular_size = 0;        /* which: 'S', 'M' or 'L'; 0 for none read */
static int s_tip_x = 0, s_tip_y = 0;   /* its tip, in its own pixels */
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

void tv_cursor_log(const char *fmt, ...) {
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
 * on the first try: "make it smaller and sharper"). An app's cursor is
 * enlarged with the app: on the C1 a 1920 x 1080 app on a 3840 x 2160 panel.
 * So the drop, drawn in the TV's own pixels, is shrunk by that factor, each
 * pixel the average of the ones it covers, in premultiplied alpha so the white
 * line does not darken: half the size of webOS's own pointer, the small sharp
 * one asked for. ⛔ Not because webOS draws its pointer at the panel's
 * resolution, as this said until 2026-10-12: webOS's picture shrunk the same
 * way came out "shrunk" (rhoquinn8217), so webOS draws it one of its pixels
 * to one of the app's (enlarge, for the regular cursor). */
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

/* The tip of webOS's picture: of the pixels at least half opaque, the one
 * furthest up and to the left (the smallest x + y; on a tie, the one nearer
 * the diagonal). The teardrop points up and to the left. */
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

/* The TV's Pointer Size as 'S', 'M' or 'L', and the word it gave; 0 and
 * "nothing" when the settings service does not say. */
static char pointer_size(const char **said) {
    char size = 0;
    *said = "nothing";
    char *reply = NULL;
    if (HLunaServiceCallSync("luna://com.webos.settingsservice/getSystemSettings",
                             "{\"category\":\"option\",\"keys\":[\"pointerSize\"]}", true, &reply) &&
        reply != NULL) {
        JSchemaInfo schema;
        jschema_info_init(&schema, jschema_all(), NULL, NULL);
        jdomparser_ref parser = jdomparser_create(&schema, 0);
        if (parser && jdomparser_feed(parser, reply, (int) strlen(reply)) && jdomparser_end(parser)) {
            jvalue_ref settings = jobject_get(jdomparser_get_result(parser), J_CSTR_TO_BUF("settings"));
            jvalue_ref value = jis_object(settings) ? jobject_get(settings, J_CSTR_TO_BUF("pointerSize")) : NULL;
            if (value != NULL && jis_string(value)) {
                raw_buffer buf = jstring_get(value);
                if (buf.m_len == 5 && strncmp(buf.m_str, "small", 5) == 0) {
                    size = 'S';
                    *said = "small";
                } else if (buf.m_len == 6 && strncmp(buf.m_str, "medium", 6) == 0) {
                    size = 'M';
                    *said = "medium";
                } else if (buf.m_len == 5 && strncmp(buf.m_str, "large", 5) == 0) {
                    size = 'L';
                    *said = "large";
                } else {
                    *said = "a size this app does not know";
                }
            }
        }
        if (parser) {
            jdomparser_release(&parser);
        }
    }
    free(reply);
    return size;
}

/* webOS's picture for the size the TV is set to, read unless it is the one
 * read already, with its tip found. False when there is none to use. ⓘ A size
 * the service does not say keeps the picture read before, or reads medium. */
static bool load_regular(void) {
    const char *said = "nothing";
    char size = pointer_size(&said);
    if (size == 0) {
        size = s_regular_size != 0 ? s_regular_size : 'M';
    }
    if (s_regular != NULL && size == s_regular_size) {
        return true;
    }
    char path[64];
    snprintf(path, sizeof(path), REGULAR_IMAGE, size);
    SDL_Surface *loaded = IMG_Load(path);
    if (loaded == NULL) {
        tv_cursor_log("%s could not be read (%s)", path, IMG_GetError());
        return false;
    }
    SDL_Surface *rgba = SDL_ConvertSurfaceFormat(loaded, SDL_PIXELFORMAT_RGBA32, 0);
    SDL_FreeSurface(loaded);
    if (rgba == NULL) {
        tv_cursor_log("%s could not be converted (%s)", path, SDL_GetError());
        return false;
    }
    int tip_x = 0, tip_y = 0;
    if (!find_tip(rgba, &tip_x, &tip_y)) {
        tv_cursor_log("%s (%dx%d): no tip found", path, rgba->w, rgba->h);
        SDL_FreeSurface(rgba);
        return false;
    }
    if (s_regular != NULL) {
        SDL_FreeSurface(s_regular);
    }
    s_regular = rgba;
    s_regular_size = size;
    s_tip_x = tip_x;
    s_tip_y = tip_y;
    tv_cursor_log("webOS's pointer for the TV's Pointer Size (it said %s): %s, %dx%d, tip %d,%d", said, path,
                  rgba->w, rgba->h, tip_x, tip_y);
    return true;
}

/* ⭐ THE REGULAR CURSOR AT WEBOS'S OWN SIZE (rhoquinn8217, 2026-10-12, of the
 * first build of it: "It looks like the correct shape but it's shrunk").
 * webOS draws its picture one of its pixels to one of the app's, so it is not
 * shrunk as the drop is: it is enlarged into the TV's own pixels first, each of
 * its pixels `scale` by `scale` of them, so that its shift is in the TV's
 * pixels as the drop's is, and the shrink after brings it back to its own
 * size. ⓘ A shift saved on that first build carries over: a step is the same
 * half of an app's pixel. */
static SDL_Surface *enlarge(SDL_Surface *src, double scale) {
    const int w = (int) lround(src->w * scale);
    const int h = (int) lround(src->h * scale);
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
        int sy = (int) floor(y / scale);
        if (sy >= src->h) sy = src->h - 1;
        const Uint32 *in = (const Uint32 *) ((const Uint8 *) src->pixels + (size_t) sy * (size_t) src->pitch);
        Uint32 *out = (Uint32 *) ((Uint8 *) dst->pixels + (size_t) y * (size_t) dst->pitch);
        for (int x = 0; x < w; ++x) {
            int sx = (int) floor(x / scale);
            if (sx >= src->w) sx = src->w - 1;
            out[x] = in[sx];
        }
    }
    SDL_UnlockSurface(dst);
    SDL_UnlockSurface(src);
    return dst;
}

/* The cursor showing, with its shift and the drop's line as they stand now.
 * False, and the cursor that was there stays, when it cannot be made. */
static bool build(void) {
    const bool regular = s_look == LOOK_REGULAR;
    const double scale = s_scale > 1.0 ? s_scale : 1.0;
    double width = 3.0 - s_thin;
    if (width < 1.0) width = 1.0;
    SDL_Surface *picture = NULL;
    if (regular) {
        picture = s_regular != NULL ? enlarge(s_regular, scale) : NULL;
        if (picture == NULL) {
            tv_cursor_log("webOS's picture could not be enlarged (%s)", SDL_GetError());
            return false;
        }
    } else {
        picture = draw_drop(width);
        if (picture == NULL) {
            tv_cursor_log("the drop could not be drawn (%s)", SDL_GetError());
            return false;
        }
    }
    /* The picture placed the shift over in a clear canvas, the hot spot where
     * its tip was before the shift. */
    const int m = TV_CURSOR_MARGIN;
    const shift_t shift = s_shift[s_look];
    const int dx = shift.x < -m ? -m : (shift.x > m ? m : shift.x);
    const int dy = shift.y < -m ? -m : (shift.y > m ? m : shift.y);
    SDL_Surface *canvas = SDL_CreateRGBSurfaceWithFormat(0, picture->w + 2 * m, picture->h + 2 * m, 32,
                                                         SDL_PIXELFORMAT_RGBA32);
    if (canvas == NULL) {
        tv_cursor_log("the canvas could not be made (%s)", SDL_GetError());
        SDL_FreeSurface(picture);
        return false;
    }
    SDL_FillRect(canvas, NULL, 0);
    SDL_SetSurfaceBlendMode(picture, SDL_BLENDMODE_NONE);
    SDL_Rect at = {m + dx, m + dy, picture->w, picture->h};
    SDL_BlitSurface(picture, NULL, canvas, &at);
    SDL_FreeSurface(picture);
    SDL_Surface *image = shrink(canvas, scale);
    SDL_FreeSurface(canvas);
    if (image == NULL) {
        tv_cursor_log("the shrink failed (%s)", SDL_GetError());
        return false;
    }
    /* ⓘ The regular picture's tip, in the TV's pixels it was enlarged into. */
    const double tip_x = regular ? s_tip_x * scale : DROP_TIP_X;
    const double tip_y = regular ? s_tip_y * scale : DROP_TIP_Y;
    int hot_x = (int) floor((tip_x + m) / scale);
    int hot_y = (int) floor((tip_y + m) / scale);
    if (hot_x >= image->w) hot_x = image->w - 1;
    if (hot_y >= image->h) hot_y = image->h - 1;
    SDL_Cursor *cursor = SDL_CreateColorCursor(image, hot_x, hot_y);
    if (cursor == NULL) {
        tv_cursor_log("SDL refused the cursor (%s)", SDL_GetError());
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
    if (regular) {
        tv_cursor_log("cursor made, the regular one: %dx%d, hot spot %d,%d, shifted %d,%d of the TV's pixels"
                      " (right, down)", image->w, image->h, hot_x, hot_y, shift.x, shift.y);
    } else {
        tv_cursor_log("cursor made: %dx%d, hot spot %d,%d, shifted %d,%d of the TV's pixels (right, down), line %.1f",
                      image->w, image->h, hot_x, hot_y, shift.x, shift.y, width);
    }
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

void tv_cursor_init(SDL_Window *window) {
    FILE *f = ctm_log_open(TV_CURSOR_LOG, "w");   /* a few lines a start: begun again each time */
    if (f != NULL) {
        fclose(f);
    }
    if (app_configuration != NULL) {
        const bridge_settings_t *saved = &app_configuration->bridge;
        s_shift[LOOK_DROP].x = clamp_shift(saved->tv_cursor_x);
        s_shift[LOOK_DROP].y = clamp_shift(saved->tv_cursor_y);
        s_shift[LOOK_REGULAR].x = clamp_shift(saved->tv_cursor_regular_x);
        s_shift[LOOK_REGULAR].y = clamp_shift(saved->tv_cursor_regular_y);
        s_look = saved->tv_cursor_regular ? LOOK_REGULAR : LOOK_DROP;
    }
    /* How much the TV enlarges Aurora: the panel against the window. */
    int win_w = 0, win_h = 0, panel_w = 0, panel_h = 0;
    if (window != NULL) {
        SDL_GetWindowSize(window, &win_w, &win_h);
    }
    if (win_w > 0 && SDL_webOSGetPanelResolution(&panel_w, &panel_h) && panel_w > win_w) {
        s_scale = (double) panel_w / (double) win_w;
    }
    tv_cursor_log("the %s for a window %dx%d on a %dx%d panel, shrunk %.2f times",
                  s_look == LOOK_REGULAR ? "regular cursor" : "drop", win_w, win_h, panel_w, panel_h, s_scale);
    if (s_look == LOOK_REGULAR && !load_regular()) {
        tv_cursor_log("the regular cursor cannot be shown: the drop instead, this run");
        s_look = LOOK_DROP;
    }
    if (!build()) {
        tv_cursor_log("webOS's own pointer stays");
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
    if (s_regular != NULL) {
        SDL_FreeSurface(s_regular);
        s_regular = NULL;
        s_regular_size = 0;
    }
    s_ready = false;
}

void tv_cursor_tick(void) {
    const int why = SDL_AtomicSet(&s_reapply, 0);
    if (why == 0 || !s_ready || s_cursor == NULL) {
        return;
    }
    /* ⓘ The regular cursor follows the TV's Pointer Size, which is changed in
     * the TV's own menus: asked again as the window comes back from them, not
     * each time the pointer comes in. */
    if (s_look == LOOK_REGULAR && why != SDL_WINDOWEVENT_ENTER) {
        const char before = s_regular_size;
        if (load_regular() && s_regular_size != before && build()) {
            return;   /* made again, and set */
        }
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
        tv_cursor_log("cursor set again: the window %s%s", window_event_name(why),
                   s_reapplied + 1 == TV_CURSOR_REAPPLY_LOGGED ? " (no more of these lines this run)" : "");
    }
    s_reapplied++;
}

bool tv_cursor_adjust(int shift_x, int shift_y, int thin) {
    if (!s_ready) {
        return false;
    }
    s_shift[s_look].x = clamp_shift(shift_x);
    s_shift[s_look].y = clamp_shift(shift_y);
    if (thin >= 0) {
        s_thin = thin > 2 ? 2 : thin;   /* below 0: keep the line that stands */
    }
    return build();
}

void tv_cursor_get_shift(int *shift_x, int *shift_y) {
    if (shift_x != NULL) *shift_x = s_shift[s_look].x;
    if (shift_y != NULL) *shift_y = s_shift[s_look].y;
}

bool tv_cursor_keep(void) {
    if (!s_ready || app_configuration == NULL) {
        return false;
    }
    const shift_t shift = s_shift[s_look];
    if (s_look == LOOK_REGULAR) {
        app_configuration->bridge.tv_cursor_regular_x = shift.x;
        app_configuration->bridge.tv_cursor_regular_y = shift.y;
    } else {
        app_configuration->bridge.tv_cursor_x = shift.x;
        app_configuration->bridge.tv_cursor_y = shift.y;
    }
    settings_save(app_configuration);
    tv_cursor_log("kept as this TV's for the %s: shifted %d,%d of the TV's pixels (right, down)",
                  s_look == LOOK_REGULAR ? "regular cursor" : "drop", shift.x, shift.y);
    return true;
}

bool tv_cursor_drawn(void) {
    return s_ready;
}

bool tv_cursor_regular(void) {
    return s_look == LOOK_REGULAR;
}

bool tv_cursor_set_look(bool regular) {
    if (regular && !load_regular()) {
        tv_cursor_log("the regular cursor cannot be shown: the %s stays",
                      s_look == LOOK_REGULAR ? "regular one" : "drop");
        return false;
    }
    const int was = s_look;
    s_look = regular ? LOOK_REGULAR : LOOK_DROP;
    if (!build()) {
        s_look = was;
        return false;
    }
    if (app_configuration != NULL && app_configuration->bridge.tv_cursor_regular != regular) {
        app_configuration->bridge.tv_cursor_regular = regular;
        settings_save(app_configuration);
    }
    tv_cursor_log("the %s cursor shows, shifted %d,%d of the TV's pixels (right, down)",
                  regular ? "regular" : "sharp", s_shift[s_look].x, s_shift[s_look].y);
    return true;
}

bool tv_cursor_toggle_look(void) {
    return tv_cursor_set_look(s_look != LOOK_REGULAR);
}

void tv_cursor_describe(char *out, size_t len) {
    const char *mode = tv_cursor_mode_on() ? "on" : (tv_cursor_mode_switchable() ? "off" : "off(hardware-mouse)");
    const bool regular = s_look == LOOK_REGULAR;
    int saved_x = 0, saved_y = 0;
    if (app_configuration != NULL) {
        saved_x = regular ? app_configuration->bridge.tv_cursor_regular_x : app_configuration->bridge.tv_cursor_x;
        saved_y = regular ? app_configuration->bridge.tv_cursor_regular_y : app_configuration->bridge.tv_cursor_y;
    }
    if (!s_ready || s_image == NULL) {
        snprintf(out, len, "tv_cursor=%s, webOS's own pointer (Aurora's could not be made: see logs/tv-cursor.log)",
                 mode);
        return;
    }
    const char *look = !regular ? "sharp"
                                : (s_regular_size == 'S' ? "regular(small)"
                                                         : (s_regular_size == 'L' ? "regular(large)" : "regular(medium)"));
    snprintf(out, len,
             "tv_cursor=%s look=%s shift=%d,%d saved=%d,%d thin=%d hot=%d,%d image=%dx%d scale=%.2f reapplied=%d",
             mode, look, s_shift[s_look].x, s_shift[s_look].y, saved_x, saved_y, s_thin, s_hot_x, s_hot_y,
             s_image->w, s_image->h, s_scale, s_reapplied);
}

#else

#include <stdio.h>

void tv_cursor_log(const char *fmt, ...) {
    (void) fmt;
}

void tv_cursor_init(SDL_Window *window) {
    (void) window;
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

bool tv_cursor_regular(void) {
    return false;
}

bool tv_cursor_set_look(bool regular) {
    (void) regular;
    return false;
}

bool tv_cursor_toggle_look(void) {
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
        tv_cursor_log("the TV cursor cannot be switched while Use mouse hardware is on");
        return false;
    }
    /* ⭐ NOT ON WHILE A MOUSE IS BRIDGED (rhoquinn8217, 2026-10-11: "When
     * attempting to turn on TV cursor, if the mouse is bridged, create a pop up
     * message saying that TV cursor can't be turned on with mouse is
     * bridged"). That mouse moves the PC's cursor, and the TV's would stand
     * still. ⓘ Said in the place of the bridge's pop-up, whichever way it was
     * asked for: Ctrl+Alt+Shift+M on any keyboard, or the control port. */
    char which[96];
    if (on && !tv_cursor_mode_on() && mouse_seen_mouse_bridged(which, sizeof(which))) {
        tv_cursor_log("the TV cursor stays off: a mouse is bridged (%s)", which);
        bridge_prompt_note(locstr("TV cursor can't be turned on while the mouse is bridged."));
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
    tv_cursor_log("TV cursor %s: the host is told %s", on ? "on" : "off",
               on ? "where the cursor is" : "only how the mouse moved, and nothing stops at the screen's edge");
    return true;
}

bool tv_cursor_toggle_mode(void) {
    return tv_cursor_set_mode(!tv_cursor_mode_on());
}
