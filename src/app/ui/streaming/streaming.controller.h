#pragma once

#include <lvgl.h>

#include "client.h"
#include "stream/session.h"

typedef struct app_t app_t;

typedef struct {
    lv_fragment_t base;
    app_t *global;
    lv_obj_t *detached_root;
    lv_obj_t *hint;
    lv_obj_t *overlay;
    lv_group_t *group;
    lv_obj_t *progress;
    lv_obj_t *video;
    lv_obj_t *actions;
    lv_obj_t *kbd_btn, *vmouse_btn;
    lv_obj_t *suspend_btn, *quit_btn;
    lv_obj_t *ctm_btn;
    /* Asks the USB server on the host for its settings window. */
    lv_obj_t *ds5usbip_btn;
    lv_obj_t *stats;
    struct {
        lv_obj_t *header;
        lv_obj_t *game_fps;
        lv_obj_t *total_ms;
        lv_obj_t *ping;
        lv_obj_t *frame_loss;
        lv_obj_t *bandwidth;
        lv_obj_t *resolution;
        lv_obj_t *codec;
        lv_obj_t *decoder;
        lv_obj_t *host_latency;
        lv_obj_t *vdec_latency;
        lv_obj_t *cpu_ram;
        lv_obj_t *audio;
    } stats_items;
    lv_obj_t *stats_compact_label;  /* Single-line stats when show_stats_compact */
    lv_obj_t *stats_quality_indicator;  /* Colored dot: green/yellow/red by latency */
    lv_obj_t *stats_pin;
    lv_obj_t *notice, *notice_label;
    /* The top-left notice that goes away by itself, and the timer that takes
     * it away. Separate from `notice` above, which is the top-right connection
     * notice and is held open by state rather than time -- sharing one would
     * make the two fight. */
    lv_obj_t *mouse_notice, *mouse_notice_label;
    lv_timer_t *mouse_notice_timer;
    lv_obj_t *soft_kbd;
    lv_style_t overlay_button_style;
    lv_style_t overlay_button_style_focused;
    lv_style_t overlay_button_label_style;
    lv_point_t button_points[6];
} streaming_controller_t;

/* Usually references to SERVER_DATA and APP_LIST should not be kept, but in this struct, they will only be used once */
typedef struct {
    app_t *global;
    uuidstr_t uuid;
    APP_LIST app;
} streaming_scene_arg_t;

extern const lv_fragment_class_t streaming_controller_class;

lv_obj_t *streaming_scene_create(lv_fragment_t *self, lv_obj_t *parent);

void streaming_styles_init(streaming_controller_t *controller);

void streaming_styles_reset(streaming_controller_t *controller);

void streaming_overlay_resized(streaming_controller_t *controller);

bool streaming_overlay_shown();

bool streaming_soft_keyboard_shown();

bool streaming_stats_shown();

bool streaming_refresh_stats();

void streaming_toggle_stats_pin(void);

void streaming_notice_show(const char *message);

/* A controller has just been bridged: if the TV's own mouse controls are on,
 * ask whether to turn them off (bridge_prompt.h). Does nothing when they are
 * off, or outside a stream. Safe to call repeatedly -- a second call restarts
 * the question's count. */
void streaming_mouse_mode_warn(void);