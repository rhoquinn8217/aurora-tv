/*
 * Gives LVGL's line drawing the argument it reads.
 *
 * What went wrong: the app died in lv_draw_sdl_draw_line the first time the
 * streaming overlay drew its Bluetooth mark (the lab, 2026-10-30: SIGSEGV,
 * writing to the address 0x3cf).
 *
 * Why, in LVGL's SDL renderer (third_party/lvgl/src/draw/sdl): that function is
 * defined with five arguments in lv_draw_sdl_line.c, the last a bool * it writes
 * through whenever a line of a new length and width has no texture yet. But
 * lv_draw_sdl.c declares it with four and lv_draw_line calls it with four, so
 * the pointer is whatever the stack holds. Each new line writes one byte there.
 * Mostly that lands somewhere it goes unnoticed, as it did for the top bar's
 * Bluetooth mark and the Virtual Mouse tick; the overlay's mark hit an address
 * with nothing behind it.
 *
 * The fix, here because LVGL is a submodule of someone else's: the display draws
 * its lines through this function, which passes a real bool.
 */

#include "draw_line_fix.h"

#include <stdbool.h>

/* LVGL's definition, as lv_draw_sdl_line.c has it. No header declares it. */
void lv_draw_sdl_draw_line(lv_draw_ctx_t *draw_ctx, const lv_draw_line_dsc_t *dsc, const lv_point_t *point1,
                           const lv_point_t *point2, bool *in_cache);

static void draw_line_with_its_argument(lv_draw_ctx_t *draw_ctx, const lv_draw_line_dsc_t *dsc,
                                        const lv_point_t *point1, const lv_point_t *point2)
{
    bool in_cache = false;
    lv_draw_sdl_draw_line(draw_ctx, dsc, point1, point2, &in_cache);
}

void draw_line_fix_install(lv_disp_t *disp)
{
    if (disp == NULL || disp->driver == NULL || disp->driver->draw_ctx == NULL) {
        return;
    }
    disp->driver->draw_ctx->draw_line = draw_line_with_its_argument;
}
