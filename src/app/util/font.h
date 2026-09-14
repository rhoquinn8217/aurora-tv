#pragma once

#include "lvgl.h"

typedef struct app_fontset_t {
    int small_size;
    int normal_size;
    int large_size;
    lv_font_t *small;
    lv_font_t *normal;
    lv_font_t *large;
    /* The small size in bold, for a line that must not be missed. NULL in a set
     * that has none, as the icon fonts do. */
    lv_font_t *small_bold;
    struct app_fontset_t *fallback;
} app_fontset_t;

typedef struct app_fonts_t {
    app_fontset_t fonts;
    app_fontset_t icons;
} app_fonts_t;

int app_font_init(app_fonts_t *fonts, int dpi);

void app_font_deinit(app_fonts_t *fonts);
