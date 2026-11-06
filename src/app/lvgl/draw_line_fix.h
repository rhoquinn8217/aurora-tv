#pragma once

#include "lvgl.h"

/* Gives LVGL's line drawing the argument it reads. Why: draw_line_fix.c. Once,
 * for the display, after it is registered. */
void draw_line_fix_install(lv_disp_t *disp);
