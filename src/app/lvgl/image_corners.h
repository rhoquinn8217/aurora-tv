/*
 * Keeps the corners of rounded pictures, the launcher's covers among them,
 * from being drawn in the colour of an unrelated rounded background.
 */
#pragma once

#include "lvgl.h"

/*
 * Wraps the display's rectangle drawing so that LVGL's shared corner mask is
 * left plain after every rounded background. Call once, right after the
 * display is registered: that is when LVGL makes the drawing context.
 */
void image_corners_install(lv_disp_t *disp);
