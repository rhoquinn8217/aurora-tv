/* The "Connecting..." box has a Cancel, which Back presses too. What it does,
 * and why, is in stream/connect_cancel.h. */

#pragma once

#include "lvgl.h"

typedef struct app_t app_t;

/* For upstream's box (ui/common/progress_dialog.c), once streaming.controller.c
 * has made and sized it. */
void connecting_cancel_button(lv_obj_t *box, app_t *app);
