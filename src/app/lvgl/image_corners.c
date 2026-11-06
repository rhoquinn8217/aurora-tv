/*
 * Keeps the corners of rounded pictures.
 *
 * What went wrong: after a stream, every cover in the launcher had a black
 * square at each corner where the rounding should be (in the lab's
 * screenshots of every run, the oldest looked at from 2026-10-19).
 *
 * Why, in LVGL's SDL renderer (third_party/lvgl/src/draw/sdl): a rounded
 * background is drawn with a small mask texture, one per radius and cached,
 * and the background's colour and opacity are set on that texture as it is
 * drawn (draw_bg_color in lv_draw_sdl_rect.c). A rounded picture's corners
 * are cut with the SAME texture (img_rounded_frag_obtain in
 * lv_draw_sdl_img.c), which uses it as it finds it, and the picture is
 * multiplied into it. So a picture's corners come out in the colour of the
 * last rounded background of that radius. The stream's overlay draws one:
 * its statistics box is black, with the covers' radius (LV_DPX(6)). A cover
 * whose corners were made after that got black ones, and kept them, since
 * the corners are cached with the cover.
 *
 * The fix, here because LVGL is a submodule of someone else's: after each
 * rectangle that LVGL drew with that mask, set the mask back to plain white
 * and fully opaque, which is what a newly made one is. A setting changed
 * afterwards does not alter a draw already made: SDL takes a texture's colour
 * and opacity when the draw is queued.
 */

#include "image_corners.h"

#include <SDL.h>

#include "draw/sdl/lv_draw_sdl.h"
#include "draw/sdl/lv_draw_sdl_rect.h"

static void (*lvgl_draw_rect)(lv_draw_ctx_t *draw_ctx, const lv_draw_rect_dsc_t *dsc, const lv_area_t *coords);

static void draw_rect_leave_mask_plain(lv_draw_ctx_t *draw_ctx, const lv_draw_rect_dsc_t *dsc,
                                       const lv_area_t *coords)
{
    lvgl_draw_rect(draw_ctx, dsc, coords);

    /*
     * Only where draw_bg_color took the mask, by its own tests: a background
     * that shows, one colour, rounded, and on the screen at all. Anywhere
     * else, asking for the mask would make one that nothing has used.
     */
    if (dsc->bg_opa == 0 || dsc->radius <= 0) {
        return;
    }
    if (dsc->bg_grad.dir != LV_GRAD_DIR_NONE && dsc->bg_grad.stops_count != 1) {
        return;
    }
    lv_area_t shown;
    if (!_lv_area_intersect(&shown, coords, draw_ctx->clip_area)) {
        return;
    }

    lv_coord_t w = lv_area_get_width(coords), h = lv_area_get_height(coords);
    lv_coord_t radius = LV_MIN3(w / 2, h / 2, dsc->radius);
    bool in_cache = false;
    SDL_Texture *mask = lv_draw_sdl_rect_bg_frag_obtain((lv_draw_sdl_ctx_t *) draw_ctx, radius, &in_cache);
    if (mask == NULL) {
        return;
    }
    if (!in_cache) {
        /* Not kept, so nothing else can find it: a picture gets a new one. */
        SDL_DestroyTexture(mask);
        return;
    }
    SDL_SetTextureColorMod(mask, 0xFF, 0xFF, 0xFF);
    SDL_SetTextureAlphaMod(mask, 0xFF);
}

void image_corners_install(lv_disp_t *disp)
{
    if (disp == NULL || disp->driver == NULL || disp->driver->draw_ctx == NULL) {
        return;
    }
    lv_draw_ctx_t *draw_ctx = disp->driver->draw_ctx;
    if (draw_ctx->draw_rect == NULL || draw_ctx->draw_rect == draw_rect_leave_mask_plain) {
        return;
    }
    lvgl_draw_rect = draw_ctx->draw_rect;
    draw_ctx->draw_rect = draw_rect_leave_mask_plain;
}
