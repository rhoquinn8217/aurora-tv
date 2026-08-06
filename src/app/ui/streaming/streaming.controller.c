#include "app.h"
#include "app_session.h"
#include "streaming.controller.h"
#include "soft_keyboard.h"

#include <SDL.h>
#include "stream/video/session_video.h"
#include "ui/root.h"
#include "ui/common/progress_dialog.h"
#include "lvgl/lv_ext_utils.h"
#include "lvgl/util/lv_app_utils.h"

#include "util/bus.h"
#include "stream/session.h"
#include "util/user_event.h"
#include "util/i18n.h"
#include "logging.h"

#include <stdio.h>
#include <string.h>

#include "ctm_bridge_glue.h"

static void exit_streaming(lv_event_t *event);

static void suspend_streaming(lv_event_t *event);

static void open_keyboard(lv_event_t *event);

static void toggle_vmouse(lv_event_t *event);

static void stream_fragment_del_timer_cb(lv_timer_t *timer);

static void hide_overlay(lv_event_t *event);
static void hide_overlay_impl(streaming_controller_t *controller);

static void on_cancel_key(lv_event_t *event);

static void soft_keyboard_close_cb(void *userdata);

static bool show_overlay(streaming_controller_t *controller);

static void on_view_created(lv_fragment_t *self, lv_obj_t *view);

static void on_delete_obj(lv_fragment_t *self, lv_obj_t *view);

static void on_obj_deleted(lv_fragment_t *self, lv_obj_t *view);

static bool on_event(lv_fragment_t *self, int code, void *userdata);

static void constructor(lv_fragment_t *self, void *args);

static void controller_dtor(lv_fragment_t *self);

static void overlay_key_cb(lv_event_t *e);

static void update_buttons_layout(streaming_controller_t *controller);

static void pin_toggle(lv_event_t *e);

static void open_ctm_panel(lv_event_t *e);

static void ctm_close_panel(void);
static void ctm_request_close(void);
static void ctm_teardown_async(void *p);
static void ctm_panel_refresh(void);
static void ctm_request_refresh(void);
static void ctm_build_detail(int row);
static void ctm_enter_detail(void);
static void ctm_leave_detail(void);
static const char *ctm_audio_name(int m);

static void streaming_set_stats_pinned(streaming_controller_t *controller, bool pinned);

/** Pretty codec label for stats (matches session_video video_format_name strings). */
static const char *streaming_codec_display(const char *fmt) {
    if (fmt == NULL || fmt[0] == '\0') {
        return "-";
    }
    if (strcmp(fmt, "H264") == 0) {
        return "H.264";
    }
    if (strcmp(fmt, "H265") == 0) {
        return "H.265";
    }
    if (strcmp(fmt, "H265 10bit") == 0) {
        return "H.265 10bit";
    }
    if (strcmp(fmt, "AV1 8bit") == 0) {
        return "AV1";
    }
    if (strcmp(fmt, "AV1 10bit") == 0) {
        return "AV1 10bit";
    }
    return fmt;
}

/** Compact codec label matching Moonlight Qt (e.g. "AV1 10-bit", "H.265"). */
static const char *streaming_codec_compact_text(const char *fmt) {
    if (fmt == NULL || fmt[0] == '\0') {
        return "-";
    }
    if (strcmp(fmt, "H264") == 0) {
        return "H.264";
    }
    if (strcmp(fmt, "H265") == 0) {
        return "H.265";
    }
    if (strcmp(fmt, "H265 10bit") == 0) {
        return "H.265 10-bit";
    }
    if (strcmp(fmt, "AV1 8bit") == 0) {
        return "AV1";
    }
    if (strcmp(fmt, "AV1 10bit") == 0) {
        return "AV1 10-bit";
    }
    return fmt;
}

static float streaming_render_fps(float decodedFps) {
    float renderFps = decodedFps;
#if defined(TARGET_WEBOS)
    int displayRate = 60;
    if (SDL_webOSGetRefreshRate(&displayRate) && displayRate > 0 && renderFps > (float) displayRate) {
        renderFps = (float) displayRate;
    }
#else
    SDL_DisplayMode mode;
    if (SDL_GetCurrentDisplayMode(0, &mode) == 0 && mode.refresh_rate > 0
        && renderFps > (float) mode.refresh_rate) {
        renderFps = (float) mode.refresh_rate;
    }
#endif
    return renderFps;
}

const lv_fragment_class_t streaming_controller_class = {
        .constructor_cb = constructor,
        .destructor_cb = controller_dtor,
        .create_obj_cb = streaming_scene_create,
        .obj_created_cb = on_view_created,
        .obj_will_delete_cb = on_delete_obj,
        .obj_deleted_cb = on_obj_deleted,
        .event_cb = on_event,
        .instance_size = sizeof(streaming_controller_t),
};

static bool overlay_showing = false, overlay_pinned = false;
static streaming_controller_t *current_controller = NULL;

/* CTM Bridge overlay panel state (see the master/detail implementation below).
 * Declared up here because on_delete_obj() references it for teardown cleanup. */
enum { CTM_F_PLUG = 1, CTM_F_AUDIO, CTM_F_HVOL, CTM_F_SVOL, CTM_F_LAT, CTM_F_HAP };

typedef struct {
    int field;
    int cur, min, max, step;
    int gindex;             /* g_devices index (plug toggle) */
    bool ds4_audio;         /* audio row cycles the DS4 subset (Auto/Headphones/Split) */
    lv_obj_t *row;
    lv_obj_t *slider;       /* NULL for non-slider rows */
    lv_obj_t *value_lbl;
} ctm_row_t;

/* DS4 audio row: cur is a POSITION in this subset, translated to/from the
 * shared audio_mode value at read/write. Headset(3) forces the Layout B route
 * 0xFF, Both(4) doubles as "Split" 0xDF (see controller_ds4.c patch_output);
 * Auto leaves the service map's jack auto-route in charge. */
static const int k_ctm_ds4_modes[] = {0 /* Auto */, 3 /* Headphones */, 4 /* Split */};

static lv_obj_t   *s_ctm_panel      = NULL;   /* full-screen backdrop */
static lv_obj_t   *s_ctm_sidebar    = NULL;
static lv_obj_t   *s_ctm_detail     = NULL;
static lv_obj_t   *s_ctm_status_lbl = NULL;
static lv_group_t *s_ctm_nav_group    = NULL;
static lv_group_t *s_ctm_detail_group = NULL;
static streaming_controller_t *s_ctm_owner = NULL;

static ctm_bridge_dev_t s_ctm_devs[16];
static lv_obj_t        *s_ctm_dev_rows[16];
static int  s_ctm_ndev = 0;
static int  s_ctm_sel  = 0;
static bool s_ctm_detail_open = false;

static ctm_row_t s_ctm_rows[8];
static int       s_ctm_nrows = 0;

/* Deferred-teardown holders: the panel is hidden synchronously on close (so the
 * remote Back produces a same-frame UI change and webOS doesn't background the
 * app), then these are freed on the next loop (can't delete the focused row from
 * inside its own Back event). */
static lv_obj_t   *s_ctm_dead_panel  = NULL;
static lv_group_t *s_ctm_dead_nav    = NULL;
static lv_group_t *s_ctm_dead_detail = NULL;

bool streaming_overlay_shown() {
    return overlay_showing;
}

bool streaming_soft_keyboard_shown() {
    return current_controller != NULL && current_controller->soft_kbd != NULL;
}

bool streaming_stats_shown() {
    return overlay_showing || overlay_pinned;
}

bool streaming_refresh_stats() {
    streaming_controller_t *controller = current_controller;
    if (!controller) { return false; }
    if (!streaming_stats_shown()) {
        return false;
    }
    app_t *app = controller->global;
    const struct VIDEO_STATS *dst = &vdec_summary_stats;
    const struct VIDEO_INFO *info = &vdec_stream_info;

    if (controller->stats_compact_label != NULL) {
        char stats_line[384];

        const char *codec = streaming_codec_compact_text(vdec_stream_info.format);
        const char *hdr_suffix = app_configuration->hdr ? " HDR" : "";
        int w = info->width > 0 ? info->width : 0;
        int h = info->height > 0 ? info->height : 0;
        float lossPct = (dst->totalFrames > 0)
            ? (float) dst->networkDroppedFrames / (float) dst->totalFrames * 100.0f
            : 0.0f;
        float bitrateMbps = (float) dst->currentBitrateKbps / 1000000.0f;

        float hostMs = 0.0f;
        float submitMs = 0.0f;
        float decOnlyMs = 0.0f;
        bool have_render = false;
        bool have_decode = false;
        bool have_encode = false;
        if (dst->submittedFrames) {
            submitMs = (float) dst->totalSubmitTime / (float) dst->submittedFrames;
            have_render = true;
            if (vdec_stream_info.has_host_latency) {
                hostMs = (float) dst->totalCaptureLatency / (float) dst->submittedFrames / 10.0f;
                have_encode = true;
            }
            if (vdec_stream_info.has_decoder_latency) {
                decOnlyMs = dst->avgDecoderLatency;
                have_decode = true;
            }
        }
        float totalMs = (float) dst->rtt + hostMs + submitMs + decOnlyMs;

        const char *audio_ch = audio_stream_info.channels;
        if (audio_ch == NULL || audio_ch[0] == '\0') {
            audio_ch = "-";
        } else if (strcmp(audio_ch, "5.1ch") == 0) {
            audio_ch = "5.1";
        } else if (strcmp(audio_ch, "7.1ch") == 0) {
            audio_ch = "7.1";
        }

        int len = snprintf(stats_line, sizeof(stats_line),
                           "%dx%d %s%s FPS %.1f "
                           "N %u \xb1 %ums FD %.2f%% BW %.2f Mbps",
                           w, h, codec, hdr_suffix,
                           dst->receivedFps,
                           (unsigned) dst->rtt, (unsigned) dst->rttVariance,
                           lossPct, bitrateMbps);
        if (len > 0 && (size_t) len < sizeof(stats_line) && (have_render || have_decode || have_encode)) {
            len += snprintf(stats_line + len, sizeof(stats_line) - (size_t) len, " |");
            bool first = true;
            if (have_render && len > 0 && (size_t) len < sizeof(stats_line)) {
                len += snprintf(stats_line + len, sizeof(stats_line) - (size_t) len,
                                "%sS %.2fms", first ? "" : " \xb7 ", submitMs);
                first = false;
            }
            if (have_decode && len > 0 && (size_t) len < sizeof(stats_line)) {
                len += snprintf(stats_line + len, sizeof(stats_line) - (size_t) len,
                                "%sD %.2fms", first ? "" : " \xb7 ", decOnlyMs);
                first = false;
            }
            if (have_encode && len > 0 && (size_t) len < sizeof(stats_line)) {
                len += snprintf(stats_line + len, sizeof(stats_line) - (size_t) len,
                                "%sEn %.1fms", first ? "" : " \xb7 ", hostMs);
                first = false;
            }
            (void) first;
        }
        if (len > 0 && (size_t) len < sizeof(stats_line)) {
            snprintf(stats_line + len, sizeof(stats_line) - (size_t) len, " | %s", audio_ch);
        }
        lv_label_set_text(controller->stats_compact_label, stats_line);
        /* Quality dot: green ≤25ms, yellow ≤30ms, red >30ms */
        if (controller->stats_quality_indicator) {
            lv_color_t qc = totalMs <= 25.0f ? lv_palette_main(LV_PALETTE_GREEN)
                          : totalMs <= 30.0f ? lv_palette_main(LV_PALETTE_YELLOW)
                          : lv_palette_main(LV_PALETTE_RED);
            lv_obj_set_style_text_color(controller->stats_quality_indicator, qc, 0);
        }
        return true;
    }

    const char *hdr_str = app_configuration->hdr ? "HDR" : "SDR";
    const char *codec = streaming_codec_display(vdec_stream_info.format);
    if (info->width > 0 && info->height > 0) {
        lv_label_set_text_fmt(controller->stats_items.decoder, "%d\u00d7%d %s %s (%s)", info->width, info->height, hdr_str,
                              codec, SS4S_ModuleInfoGetId(app->ss4s.selection.video_module));
    } else {
        lv_label_set_text_fmt(controller->stats_items.decoder, "%s %s (%s)", codec, hdr_str,
                              SS4S_ModuleInfoGetId(app->ss4s.selection.video_module));
    }
    lv_label_set_text_fmt(controller->stats_items.audio, "%s, %s (%s)", audio_stream_info.format,
                          audio_stream_info.channels, SS4S_ModuleInfoGetId(app->ss4s.selection.audio_module));
    lv_label_set_text_fmt(controller->stats_items.rtt, "%d ms (var. %d ms)", dst->rtt, dst->rttVariance);
    lv_label_set_text_fmt(controller->stats_items.net_fps, "%.2f FPS", dst->receivedFps);
    float renderFps = streaming_render_fps(dst->decodedFps);
    lv_label_set_text_fmt(controller->stats_items.render_fps, "%.2f FPS", renderFps);
    lv_label_set_text_fmt(controller->stats_items.bitrate, "%u Mbps", dst->currentBitrateKbps / 1000000);

        if (dst->submittedFrames) {
            lv_label_set_text_fmt(controller->stats_items.drop_rate, "%.2f%%",
                                  (float) dst->networkDroppedFrames / (float) dst->totalFrames * 100);
            if (vdec_stream_info.has_host_latency) {
                float avgCapLatency = (float) dst->totalCaptureLatency / (float) dst->submittedFrames / 10.0f;
                lv_label_set_text_fmt(controller->stats_items.host_latency, "avg %.2f ms", avgCapLatency);
            } else {
                lv_label_set_text_fmt(controller->stats_items.host_latency, "not available");
            }
            if (vdec_stream_info.has_decoder_latency) {
                float avgSubmitTime = (float) dst->totalSubmitTime / (float) dst->submittedFrames;
                lv_label_set_text_fmt(controller->stats_items.vdec_latency, "submit %.2f ms + decode %.2f ms",
                                      avgSubmitTime, dst->avgDecoderLatency);
            } else {
                lv_label_set_text_fmt(controller->stats_items.vdec_latency, "not available");
            }
    } else {
        lv_label_set_text(controller->stats_items.drop_rate, "-");
        lv_label_set_text_fmt(controller->stats_items.host_latency, "-");
        lv_label_set_text_fmt(controller->stats_items.vdec_latency, "-");
    }

    return true;
}

void streaming_notice_show(const char *message) {
    streaming_controller_t *controller = current_controller;
    if (!controller) { return; }
    lv_label_set_text(controller->notice_label, message);
    if (message && message[0]) {
        lv_obj_clear_flag(controller->notice, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(controller->notice, LV_OBJ_FLAG_HIDDEN);
    }
}

static void constructor(lv_fragment_t *self, void *args) {
    streaming_controller_t *controller = (streaming_controller_t *) self;
    current_controller = controller;

    overlay_showing = false;

    streaming_styles_init(controller);

    const streaming_scene_arg_t *arg = (streaming_scene_arg_t *) args;
    controller->global = arg->global;
    if (app_session_begin(arg->global, &arg->uuid, &arg->app) != 0) {
        commons_log_error("Streaming", "Failed to start session");
        lv_async_call((lv_async_cb_t) lv_fragment_del, controller);
    }
}

static void controller_dtor(lv_fragment_t *self) {
    streaming_controller_t *fragment = (streaming_controller_t *) self;
    streaming_styles_reset(fragment);
    if (current_controller == fragment) {
        current_controller = NULL;
    }
    fragment->soft_kbd = NULL; /* Will be deleted with parent */
}

static bool on_event(lv_fragment_t *self, int code, void *userdata) {
    LV_UNUSED(userdata);
    streaming_controller_t *controller = (streaming_controller_t *) self;
    switch (code) {
        case USER_STREAM_CONNECTING: {
            controller->progress = progress_dialog_create(locstr("Connecting..."));
            if (lv_obj_check_type(controller->progress->parent, &lv_msgbox_backdrop_class)) {
                lv_obj_set_style_bg_opa(controller->progress->parent, LV_OPA_TRANSP, 0);
            }
            lv_obj_set_width(controller->progress, LV_DPX(300));
            lv_obj_set_height(controller->progress, LV_DPX(100));
            lv_obj_add_flag(controller->overlay, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(controller->hint, LV_OBJ_FLAG_HIDDEN);
            return true;
        }
        case USER_STREAM_OPEN: {
            if (controller->progress) {
                lv_msgbox_close(controller->progress);
                controller->progress = NULL;
            }
            lv_obj_add_flag(controller->overlay, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(controller->hint, LV_OBJ_FLAG_HIDDEN);

            if (app_configuration->show_stats_on_start) {
                streaming_set_stats_pinned(controller, true);
            }
            break;
        }
        case USER_STREAM_CLOSE: {
            /* A failed auto-reconnect arrives here with the "Connecting..."
             * dialog still open; close it before showing the next one. */
            if (controller->progress) {
                lv_msgbox_close(controller->progress);
            }
            controller->progress = progress_dialog_create(locstr("Disconnecting..."));
            lv_obj_add_flag(controller->overlay, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(controller->stats, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(controller->hint, LV_OBJ_FLAG_HIDDEN);
            break;
        }
        case USER_STREAM_FINISHED: {
            if (controller->progress) {
                lv_msgbox_close(controller->progress);
                controller->progress = NULL;
            }
            if (streaming_errno != 0) {
                lv_timer_create(stream_fragment_del_timer_cb, 50, controller);
            } else {
                lv_async_call((lv_async_cb_t) lv_fragment_del, controller);
            }
            break;
        }
        case USER_OPEN_OVERLAY: {
            show_overlay(controller);
            return true;
        }
        case USER_CLOSE_SOFT_KEYBOARD: {
            if (streaming_soft_keyboard_shown()) {
                soft_keyboard_close_cb(controller);
            }
            return true;
        }
        case USER_TOGGLE_STATS_PIN: {
            streaming_toggle_stats_pin();
            return true;
        }
        case USER_TOGGLE_VMOUSE: {
            if (controller->global->session) {
                session_toggle_vmouse(controller->global->session);
            }
            return true;
        }
        case USER_OPEN_SOFT_KEYBOARD: {
            if (controller->soft_kbd) {
                return true;
            }
            hide_overlay_impl(controller);
            session_screen_keyboard_opened(controller->global->session);
            controller->soft_kbd = soft_keyboard_create(
                controller->detached_root,
                controller->global->session,
                soft_keyboard_close_cb,
                controller);
            lv_group_t *kbd_group = soft_keyboard_get_group(controller->soft_kbd);
            if (kbd_group) {
                app_input_set_group(&controller->global->ui.input, kbd_group);
            }
            app_set_mouse_grab(&controller->global->input, false);
            return true;
        }
        case USER_SIZE_CHANGED: {
            update_buttons_layout(controller);
            streaming_overlay_resized(controller);
            return false;
        }
        default: {
            break;
        }
    }
    return false;
}

static void on_view_created(lv_fragment_t *self, lv_obj_t *view) {
    streaming_controller_t *controller = (streaming_controller_t *) self;
    app_input_set_group(&controller->global->ui.input, controller->group);
    lv_obj_add_event_cb(controller->quit_btn, exit_streaming, LV_EVENT_CLICKED, self);
    lv_obj_add_event_cb(controller->suspend_btn, suspend_streaming, LV_EVENT_CLICKED, self);
    lv_obj_add_event_cb(controller->kbd_btn, open_keyboard, LV_EVENT_CLICKED, self);
    lv_obj_add_event_cb(controller->vmouse_btn, toggle_vmouse, LV_EVENT_CLICKED, self);
    lv_obj_add_event_cb(controller->ctm_btn, open_ctm_panel, LV_EVENT_CLICKED, self);
    lv_obj_add_event_cb(controller->base.obj, hide_overlay, LV_EVENT_CLICKED, self);
    lv_obj_add_event_cb(controller->overlay, overlay_key_cb, LV_EVENT_KEY, controller);
    lv_obj_add_event_cb(controller->base.obj, on_cancel_key, LV_EVENT_CANCEL, controller);

    lv_obj_t *notice = lv_obj_create(lv_layer_sys());
    lv_obj_set_size(notice, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_align(notice, LV_ALIGN_TOP_RIGHT, -LV_DPX(20), LV_DPX(20));
    lv_obj_set_style_radius(notice, LV_DPX(5), 0);
    lv_obj_set_style_pad_hor(notice, LV_DPX(5), 0);
    lv_obj_set_style_pad_ver(notice, LV_DPX(3), 0);
    lv_obj_set_style_border_opa(notice, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_opa(notice, LV_OPA_40, 0);
    lv_obj_set_style_bg_color(notice, lv_color_black(), 0);
    lv_obj_t *notice_label = lv_label_create(notice);
    lv_obj_set_size(notice_label, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_text_font(notice_label, lv_theme_get_font_small(view), 0);
    lv_obj_add_flag(notice, LV_OBJ_FLAG_HIDDEN);

    controller->notice = notice;
    controller->notice_label = notice_label;

    lv_obj_add_event_cb(controller->stats_pin, pin_toggle, LV_EVENT_VALUE_CHANGED, controller->stats);

#if !defined(TARGET_WEBOS)
    const app_settings_t *settings = &controller->global->settings;
    if (settings->syskey_capture) {
        SDL_SetWindowGrab(controller->global->ui.window, SDL_TRUE);
    }
#endif
}

static void on_delete_obj(lv_fragment_t *self, lv_obj_t *view) {
    LV_UNUSED(view);
    streaming_controller_t *controller = (streaming_controller_t *) self;
    if (controller->notice) {
        lv_obj_del(controller->notice);
    }
    if (controller->stats->parent != controller->overlay) {
        lv_obj_del(controller->stats);
    }
    app_input_set_group(&controller->global->ui.input, NULL);
    /* If the CTM panel is still open at teardown, drop its focus groups (the
     * panel obj itself is freed with detached_root). Avoids leaking the groups
     * and a dangling indev group pointer. */
    if (s_ctm_owner == controller) {
        if (s_ctm_nav_group)    { lv_group_del(s_ctm_nav_group);    s_ctm_nav_group = NULL; }
        if (s_ctm_detail_group) { lv_group_del(s_ctm_detail_group); s_ctm_detail_group = NULL; }
        s_ctm_panel = NULL;
        s_ctm_sidebar = NULL;
        s_ctm_detail = NULL;
        s_ctm_status_lbl = NULL;
        s_ctm_owner = NULL;
        s_ctm_detail_open = false;
        s_ctm_nrows = 0;
    }
    /* Cancel any in-flight panel teardown; the dead panel is freed with
     * detached_root below, but its groups must be released here. */
    lv_async_call_cancel(ctm_teardown_async, NULL);
    if (s_ctm_dead_nav)    { lv_group_del(s_ctm_dead_nav);    s_ctm_dead_nav = NULL; }
    if (s_ctm_dead_detail) { lv_group_del(s_ctm_dead_detail); s_ctm_dead_detail = NULL; }
    s_ctm_dead_panel = NULL;
    lv_group_del(controller->group);

#if !defined(TARGET_WEBOS)
    SDL_SetWindowGrab(controller->global->ui.window, SDL_FALSE);
#endif
}

static void on_obj_deleted(lv_fragment_t *self, lv_obj_t *view) {
    LV_UNUSED(view);
    streaming_controller_t *controller = (streaming_controller_t *) self;
    lv_obj_del(controller->detached_root);
}

static void exit_streaming(lv_event_t *event) {
    streaming_controller_t *self = lv_event_get_user_data(event);
    session_interrupt(self->global->session, true, STREAMING_INTERRUPT_USER);
}

static void suspend_streaming(lv_event_t *event) {
    streaming_controller_t *self = lv_event_get_user_data(event);
    session_interrupt(self->global->session, false, STREAMING_INTERRUPT_USER);
}

static void soft_keyboard_close_cb(void *userdata) {
    streaming_controller_t *controller = userdata;
    if (!controller || !controller->soft_kbd) {
        return;
    }
    lv_obj_t *kbd_obj = controller->soft_kbd;
    controller->soft_kbd = NULL;
    app_input_set_group(&controller->global->ui.input, controller->group);
    app_set_mouse_grab(&controller->global->input, true);
    if (controller->global->session) {
        session_screen_keyboard_closed(controller->global->session);
    }
    lv_obj_del(kbd_obj);
}

static void open_keyboard(lv_event_t *event) {
    streaming_controller_t *controller = lv_event_get_user_data(event);
    hide_overlay(event);
    if (controller->soft_kbd) {
        return; /* Already showing */
    }
    session_screen_keyboard_opened(controller->global->session);
    controller->soft_kbd = soft_keyboard_create(
        lv_layer_top(),
        controller->global->session,
        soft_keyboard_close_cb,
        controller);
    lv_group_t *kbd_group = soft_keyboard_get_group(controller->soft_kbd);
    if (kbd_group) {
        app_input_set_group(&controller->global->ui.input, kbd_group);
    }
    /* Mostrar cursor e liberar mouse para Magic Remote / ponteiro funcionarem */
    app_set_mouse_grab(&controller->global->input, false);
}

static void toggle_vmouse(lv_event_t *event) {
    streaming_controller_t *controller = lv_event_get_user_data(event);
    hide_overlay(event);
    app_t *app = controller->global;
    session_toggle_vmouse(app->session);
}

static void stream_fragment_del_timer_cb(lv_timer_t *timer) {
    lv_fragment_t *fragment = timer->user_data;
    lv_timer_del(timer);
    lv_fragment_del(fragment);
}

/* ===================================================================
 * CTM Bridge overlay panel - master/detail, controller-navigable.
 *
 * Overlay-resident (parented to controller->detached_root, NOT a modal
 * lv_msgbox): a modal flips the UI to key/gamepad input mode, which makes
 * root.c hide the webOS Magic-Remote cursor - that was the "pointer
 * disappears" bug. As an act-screen container it behaves like the action
 * bar, so the cursor returns whenever the remote moves.
 *
 * Two focus groups mirror the launcher's nav_group/detail_group:
 *   sidebar  Up/Down = pick controller (right pane live-previews)
 *            Select(A)/Right = enter detail,  Back(B) = close panel
 *   detail   Up/Down = move between settings, Left/Right = change value
 *            Select(A) = toggle plug / cycle audio,  Back(B) = sidebar
 * The Magic-Remote pointer can click or drag anything directly. */

#define CTM_COL_CARD     lv_color_hex(0x12181d)
#define CTM_COL_SIDEBAR  lv_color_hex(0x1a232b)
#define CTM_COL_ROW      lv_color_hex(0x232f39)
#define CTM_COL_FOCUS    lv_color_hex(0x2563eb)
#define CTM_COL_BORDER   lv_color_hex(0x2a3540)
#define CTM_COL_TXT      lv_color_hex(0xf5f8fa)
#define CTM_COL_SUB      lv_color_hex(0x95a3b0)
#define CTM_COL_OK       lv_color_hex(0x35c46a)

static const char *ctm_audio_name(int m) {
    switch (m) {
        case 0:  return "Auto";
        case 1:  return "Off";
        case 2:  return "Speaker";
        case 3:  return "Headset";
        case 4:  return "Both";
        default: return "?";
    }
}

/* DS4 names for the same mode values (route semantics, not endpoints). */
static const char *ctm_audio_name_ds4(int m) {
    switch (m) {
        case 0:  return "Auto";
        case 3:  return "Headphones";
        case 4:  return "Split";
        default: return "?";
    }
}

static const char *ctm_kind_title(const char *kind) {
    if (strcmp(kind, "ds5") == 0 || strcmp(kind, "ds5_usb") == 0)
        return "Sony DualSense (DS5)";
    if (strcmp(kind, "ds5e") == 0 || strcmp(kind, "ds5e_usb") == 0)
        return "Sony DualSense Edge";
    if (strcmp(kind, "ds4") == 0)  return "Sony DualShock 4 (DS4)";
    if (strcmp(kind, "puck") == 0) return "Steam Controller";
    if (strcmp(kind, "xbox") == 0) return "Xbox Controller";
    return "Controller";
}

/* Push one setting field to the selected controller's live bridge settings. */
static void ctm_write_field(int field, int val) {
    if (s_ctm_sel < 0 || s_ctm_sel >= s_ctm_ndev) {
        return;
    }
    int gindex = s_ctm_devs[s_ctm_sel].index;
    ctm_bridge_settings_t s;
    if (!ctm_bridge_get_settings(gindex, &s)) {
        return;
    }
    switch (field) {
        case CTM_F_AUDIO: s.audio_mode = val; break;
        case CTM_F_HVOL:  s.headset_volume_percent = val; break;
        case CTM_F_SVOL:  s.speaker_volume_percent = val; break;
        case CTM_F_LAT:   s.latency_ms = val; break;
        case CTM_F_HAP:   s.haptics_gain_centi = val; break;
        default: return;
    }
    ctm_bridge_set_settings(gindex, &s);
}

/* Render a row's value label: audio as "< Name >", haptics on a 0.0-5.0 scale
 * (stored value is centi-units, 0-500), everything else as a plain integer. */
static void ctm_set_value_label(ctm_row_t *r) {
    if (!r->value_lbl) {
        return;
    }
    if (r->field == CTM_F_AUDIO) {
        lv_label_set_text_fmt(r->value_lbl, "< %s >",
                              r->ds4_audio ? ctm_audio_name_ds4(k_ctm_ds4_modes[r->cur])
                                           : ctm_audio_name(r->cur));
    } else if (r->field == CTM_F_HAP) {
        lv_label_set_text_fmt(r->value_lbl, "%d.%d", r->cur / 100, (r->cur % 100) / 10);
    } else {
        lv_label_set_text_fmt(r->value_lbl, "%d", r->cur);
    }
}

/* Apply a new value to a settings row (clamp/wrap, sync slider + label, write). */
static void ctm_apply_row(ctm_row_t *r, int val) {
    if (r->field == CTM_F_AUDIO) {
        int n = r->ds4_audio ? (int) (sizeof k_ctm_ds4_modes / sizeof k_ctm_ds4_modes[0]) : 5;
        val %= n;
        if (val < 0) val += n;
    } else {
        if (val < r->min) val = r->min;
        if (val > r->max) val = r->max;
    }
    r->cur = val;
    if (r->slider) {
        lv_slider_set_value(r->slider, val, LV_ANIM_OFF);
    }
    ctm_set_value_label(r);
    /* DS4 audio rows store a subset position; the bridge wants the mode value. */
    ctm_write_field(r->field, (r->field == CTM_F_AUDIO && r->ds4_audio)
                                  ? k_ctm_ds4_modes[val] : val);
}

/* Pointer drag on a slider -> write back + refresh its value label. */
static void ctm_slider_cb(lv_event_t *e) {
    ctm_row_t *r = lv_event_get_user_data(e);
    r->cur = (int) lv_slider_get_value(r->slider);
    ctm_set_value_label(r);
    ctm_write_field(r->field, r->cur);
}

/* Plug/unplug toggle row activated (Select or pointer tap). */
static void ctm_plug_click_cb(lv_event_t *e) {
    ctm_row_t *r = lv_event_get_user_data(e);
    if (r->cur) {
        ctm_bridge_unplug_index(r->gindex);
    } else {
        ctm_bridge_plug_index(r->gindex);
    }
    ctm_request_refresh();
}

/* Audio row activated (Select or pointer tap) -> cycle to the next mode. */
static void ctm_audio_click_cb(lv_event_t *e) {
    ctm_row_t *r = lv_event_get_user_data(e);
    ctm_apply_row(r, r->cur + 1);
}

static void ctm_detail_row_key_cb(lv_event_t *e) {
    ctm_row_t *r = lv_event_get_user_data(e);
    switch (lv_event_get_key(e)) {
        case LV_KEY_UP:
            lv_group_focus_prev(s_ctm_detail_group);
            lv_obj_scroll_to_view(lv_group_get_focused(s_ctm_detail_group), LV_ANIM_ON);
            break;
        case LV_KEY_DOWN:
            lv_group_focus_next(s_ctm_detail_group);
            lv_obj_scroll_to_view(lv_group_get_focused(s_ctm_detail_group), LV_ANIM_ON);
            break;
        case LV_KEY_LEFT:
            /* Value-only: plug toggles on Select (A), never on Left/Right. */
            if (r->field != CTM_F_PLUG) ctm_apply_row(r, r->cur - r->step);
            break;
        case LV_KEY_RIGHT:
            if (r->field != CTM_F_PLUG) ctm_apply_row(r, r->cur + r->step);
            break;
        case LV_KEY_ESC:   ctm_leave_detail(); break;
        default: break;
    }
}

static void ctm_detail_cancel_cb(lv_event_t *e) {
    LV_UNUSED(e);
    ctm_leave_detail();
}

/* A focusable card row inside the detail pane. */
static lv_obj_t *ctm_detail_card(void) {
    lv_obj_t *row = lv_obj_create(s_ctm_detail);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(row, LV_DPX(10), 0);
    lv_obj_set_style_pad_gap(row, LV_DPX(6), 0);
    lv_obj_set_style_radius(row, LV_DPX(8), 0);
    lv_obj_set_style_bg_color(row, CTM_COL_ROW, 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_outline_width(row, LV_DPX(2), LV_STATE_FOCUS_KEY);
    lv_obj_set_style_outline_color(row, CTM_COL_FOCUS, LV_STATE_FOCUS_KEY);
    lv_obj_set_style_outline_pad(row, LV_DPX(2), LV_STATE_FOCUS_KEY);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(row, LV_OBJ_FLAG_EVENT_BUBBLE);   /* Back bubbles to the detail pane */
    return row;
}

/* A native-looking button: reuses the streaming action-bar style (rounded,
 * shadow, blue focus outline) so panel buttons match the rest of the app
 * instead of looking improvised. */
static lv_obj_t *ctm_nice_btn(lv_obj_t *parent, const char *text, lv_color_t bg) {
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_EVENT_BUBBLE);   /* Back bubbles to the container */
    lv_obj_add_style(btn, &s_ctm_owner->overlay_button_style, 0);
    lv_obj_add_style(btn, &s_ctm_owner->overlay_button_style_focused, LV_STATE_FOCUS_KEY);
    lv_obj_set_style_bg_color(btn, bg, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_t *l = lv_label_create(btn);
    lv_obj_add_style(l, &s_ctm_owner->overlay_button_label_style, 0);
    lv_label_set_text(l, text);
    lv_obj_center(l);
    return btn;
}

/* Name (left) + value (right) header line for a card row; returns the value label. */
static lv_obj_t *ctm_row_header(lv_obj_t *card, const char *name) {
    lv_obj_t *hdr = lv_obj_create(card);
    lv_obj_remove_style_all(hdr);
    lv_obj_set_size(hdr, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(hdr, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(hdr, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(hdr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *nl = lv_label_create(hdr);
    lv_label_set_text(nl, name);
    lv_obj_set_style_text_color(nl, CTM_COL_SUB, 0);
    lv_obj_t *vl = lv_label_create(hdr);
    lv_obj_set_style_text_color(vl, CTM_COL_TXT, 0);
    return vl;
}

static void ctm_add_slider_row(const char *name, int field, int val, int min, int max, int step) {
    if (s_ctm_nrows >= (int) (sizeof s_ctm_rows / sizeof s_ctm_rows[0])) return;
    ctm_row_t *r = &s_ctm_rows[s_ctm_nrows++];
    r->field = field; r->cur = val; r->min = min; r->max = max; r->step = step;
    r->gindex = 0;
    lv_obj_t *card = ctm_detail_card();
    r->row = card;
    r->value_lbl = ctm_row_header(card, name);
    ctm_set_value_label(r);
    lv_obj_t *sl = lv_slider_create(card);
    r->slider = sl;
    lv_obj_set_width(sl, LV_PCT(100));
    lv_slider_set_range(sl, min, max);
    lv_slider_set_value(sl, val, LV_ANIM_OFF);
    lv_group_remove_obj(sl);            /* pointer-only; nav happens via the card */
    lv_obj_add_event_cb(sl, ctm_slider_cb, LV_EVENT_VALUE_CHANGED, r);
    lv_group_add_obj(s_ctm_detail_group, card);
    lv_obj_add_event_cb(card, ctm_detail_row_key_cb, LV_EVENT_KEY, r);
    lv_obj_add_event_cb(card, ctm_detail_cancel_cb, LV_EVENT_CANCEL, r);
}

static void ctm_add_audio_row(int val, bool ds4) {
    if (s_ctm_nrows >= (int) (sizeof s_ctm_rows / sizeof s_ctm_rows[0])) return;
    ctm_row_t *r = &s_ctm_rows[s_ctm_nrows++];
    r->field = CTM_F_AUDIO; r->min = 0; r->max = 4; r->step = 1;
    r->ds4_audio = ds4;
    if (ds4) {
        /* Translate the stored mode to a subset position (unknown -> Auto). */
        r->cur = 0;
        for (int i = 0; i < (int) (sizeof k_ctm_ds4_modes / sizeof k_ctm_ds4_modes[0]); ++i) {
            if (k_ctm_ds4_modes[i] == val) { r->cur = i; break; }
        }
    } else {
        r->cur = val;
    }
    r->gindex = 0; r->slider = NULL;
    lv_obj_t *card = ctm_detail_card();
    r->row = card;
    r->value_lbl = ctm_row_header(card, "Audio mode");
    ctm_set_value_label(r);
    lv_group_add_obj(s_ctm_detail_group, card);
    lv_obj_add_event_cb(card, ctm_detail_row_key_cb, LV_EVENT_KEY, r);
    lv_obj_add_event_cb(card, ctm_detail_cancel_cb, LV_EVENT_CANCEL, r);
    lv_obj_add_event_cb(card, ctm_audio_click_cb, LV_EVENT_CLICKED, r);
}

/* Bottom action of the detail pane: plug/unplug THIS controller. Activated by
 * Select (A) or pointer tap only. Plain text — no symbol glyph (the webOS font
 * build lacks the LVGL symbol range, so glyphs render as tofu boxes). */
static void ctm_add_plug_row(const ctm_bridge_dev_t *d) {
    if (s_ctm_nrows >= (int) (sizeof s_ctm_rows / sizeof s_ctm_rows[0])) return;
    ctm_row_t *r = &s_ctm_rows[s_ctm_nrows++];
    r->field = CTM_F_PLUG; r->cur = d->plugged ? 1 : 0; r->min = 0; r->max = 1; r->step = 1;
    r->gindex = d->index; r->slider = NULL; r->value_lbl = NULL;
    lv_obj_t *btn = ctm_nice_btn(s_ctm_detail,
                                 d->plugged ? "Unplug this controller" : "Plug this controller",
                                 d->plugged ? lv_palette_darken(LV_PALETTE_RED, 2)
                                            : lv_palette_darken(LV_PALETTE_GREEN, 2));
    lv_obj_set_width(btn, LV_PCT(100));
    r->row = btn;
    lv_group_add_obj(s_ctm_detail_group, btn);
    lv_obj_add_event_cb(btn, ctm_detail_row_key_cb, LV_EVENT_KEY, r);
    lv_obj_add_event_cb(btn, ctm_detail_cancel_cb, LV_EVENT_CANCEL, r);
    lv_obj_add_event_cb(btn, ctm_plug_click_cb, LV_EVENT_CLICKED, r);
}

static void ctm_build_detail(int row) {
    if (!s_ctm_detail) {
        return;
    }
    lv_group_remove_all_objs(s_ctm_detail_group);
    lv_obj_clean(s_ctm_detail);
    s_ctm_nrows = 0;

    if (row < 0 || row >= s_ctm_ndev) {
        lv_obj_t *l = lv_label_create(s_ctm_detail);
        lv_label_set_text(l, "No controller selected.");
        lv_obj_set_style_text_color(l, CTM_COL_SUB, 0);
        return;
    }
    ctm_bridge_dev_t *d = &s_ctm_devs[row];

    lv_obj_t *title = lv_label_create(s_ctm_detail);
    lv_label_set_text(title, ctm_kind_title(d->kind));
    lv_obj_set_style_text_color(title, CTM_COL_TXT, 0);
    lv_obj_set_style_text_font(title, lv_theme_get_font_normal(title), 0);

    /* Identity sub-line: vid:pid - BUS - MAC (ASCII separators; no glyphs). */
    lv_obj_t *idl = lv_label_create(s_ctm_detail);
    if (d->mac[0]) {
        lv_label_set_text_fmt(idl, "%s:%s - %s - %s", d->vid, d->pid,
                              d->bus[0] ? d->bus : "?", d->mac);
    } else {
        lv_label_set_text_fmt(idl, "%s:%s - %s", d->vid, d->pid, d->bus[0] ? d->bus : "?");
    }
    lv_obj_set_style_text_color(idl, CTM_COL_SUB, 0);
    lv_obj_set_style_text_font(idl, lv_theme_get_font_small(idl), 0);
    lv_obj_set_style_pad_bottom(idl, LV_DPX(8), 0);

    ctm_bridge_settings_t s;
    bool have = ctm_bridge_get_settings(d->index, &s);
    if (have && (strcmp(d->kind, "ds5") == 0 || strcmp(d->kind, "ds4") == 0)) {
        ctm_add_audio_row(s.audio_mode, strcmp(d->kind, "ds4") == 0);
        if (strcmp(d->kind, "ds4") == 0) {
            /* DS4 firmware volume ceiling is 0x4f; no host latency/haptics block. */
            ctm_add_slider_row("Headset vol", CTM_F_HVOL, s.headset_volume_percent, 0, 0x4f, 1);
            ctm_add_slider_row("Speaker vol", CTM_F_SVOL, s.speaker_volume_percent, 0, 0x4f, 1);
        } else {
            ctm_add_slider_row("Headset vol", CTM_F_HVOL, s.headset_volume_percent, 0, 100, 1);
            ctm_add_slider_row("Speaker vol", CTM_F_SVOL, s.speaker_volume_percent, 0, 100, 1);
            ctm_add_slider_row("Latency (ms)", CTM_F_LAT, s.latency_ms, 20, 255, 1);
            ctm_add_slider_row("Haptics", CTM_F_HAP, s.haptics_gain_centi, 0, 500, 10);
        }
    } else {
        lv_obj_t *l = lv_label_create(s_ctm_detail);
        lv_label_set_text(l, "No adjustable audio/haptics for this controller.");
        lv_obj_set_style_text_color(l, CTM_COL_SUB, 0);
        lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(l, LV_PCT(100));
    }

    /* Bottom action: plug/unplug THIS controller (A toggles, never Left/Right). */
    ctm_add_plug_row(d);
}

static void ctm_enter_detail(void) {
    if (s_ctm_nrows == 0 || !s_ctm_owner) {
        return;
    }
    s_ctm_detail_open = true;
    app_input_set_group(&s_ctm_owner->global->ui.input, s_ctm_detail_group);
    lv_group_focus_obj(s_ctm_rows[0].row);
    lv_obj_add_state(s_ctm_rows[0].row, LV_STATE_FOCUS_KEY);
}

static void ctm_leave_detail(void) {
    if (!s_ctm_owner) {
        return;
    }
    s_ctm_detail_open = false;
    app_input_set_group(&s_ctm_owner->global->ui.input, s_ctm_nav_group);
    if (s_ctm_sel >= 0 && s_ctm_sel < s_ctm_ndev && s_ctm_dev_rows[s_ctm_sel]) {
        lv_group_focus_obj(s_ctm_dev_rows[s_ctm_sel]);
        lv_obj_add_state(s_ctm_dev_rows[s_ctm_sel], LV_STATE_FOCUS_KEY);
    }
}

/* Sidebar: a controller row was focused -> live-preview its detail. */
static void ctm_dev_focus_cb(lv_event_t *e) {
    int row = (int) (intptr_t) lv_event_get_user_data(e);
    if (row < 0 || row >= s_ctm_ndev) {
        return;
    }
    s_ctm_sel = row;
    for (int i = 0; i < s_ctm_ndev; ++i) {
        if (!s_ctm_dev_rows[i]) continue;
        if (i == row) lv_obj_add_state(s_ctm_dev_rows[i], LV_STATE_CHECKED);
        else          lv_obj_clear_state(s_ctm_dev_rows[i], LV_STATE_CHECKED);
    }
    ctm_build_detail(row);
}

/* Sidebar: a controller row was activated (Select/Right or tap) -> enter detail. */
static void ctm_dev_click_cb(lv_event_t *e) {
    int row = (int) (intptr_t) lv_event_get_user_data(e);
    if (row < 0 || row >= s_ctm_ndev) {
        return;
    }
    s_ctm_sel = row;
    ctm_build_detail(row);
    ctm_enter_detail();
}

static void ctm_nav_key_cb(lv_event_t *e) {
    int row = (int) (intptr_t) lv_event_get_user_data(e);
    switch (lv_event_get_key(e)) {
        case LV_KEY_UP:
            lv_group_focus_prev(s_ctm_nav_group);
            lv_obj_scroll_to_view(lv_group_get_focused(s_ctm_nav_group), LV_ANIM_ON);
            break;
        case LV_KEY_DOWN:
            lv_group_focus_next(s_ctm_nav_group);
            lv_obj_scroll_to_view(lv_group_get_focused(s_ctm_nav_group), LV_ANIM_ON);
            break;
        case LV_KEY_RIGHT:
            if (row >= 0 && s_ctm_nrows > 0) ctm_enter_detail();
            break;
        case LV_KEY_ESC:   ctm_request_close(); break;
        default: break;
    }
}

static void ctm_nav_cancel_cb(lv_event_t *e) {
    LV_UNUSED(e);
    ctm_request_close();
}

/* Short sidebar label: kind badge for known controllers, device name for HID. */
static const char *ctm_dev_label(const ctm_bridge_dev_t *d) {
    if (strcmp(d->kind, "ds5") == 0 || strcmp(d->kind, "ds5_usb") == 0)  return "DS5";
    if (strcmp(d->kind, "ds5e") == 0 || strcmp(d->kind, "ds5e_usb") == 0) return "DS5 Edge";
    if (strcmp(d->kind, "ds4") == 0)  return "DS4";
    if (strcmp(d->kind, "puck") == 0) return "Steam Puck";
    if (strcmp(d->kind, "xbox") == 0) return "Xbox";
    return d->name;
}

static lv_obj_t *ctm_make_dev_row(const ctm_bridge_dev_t *d, int idx) {
    lv_obj_t *row = lv_obj_create(s_ctm_sidebar);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(row, LV_DPX(10), 0);
    lv_obj_set_style_pad_ver(row, LV_DPX(9), 0);
    lv_obj_set_style_radius(row, LV_DPX(6), 0);
    lv_obj_set_style_bg_color(row, CTM_COL_ROW, 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(row, CTM_COL_FOCUS, LV_STATE_FOCUS_KEY);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_STATE_FOCUS_KEY);
    /* Persistent selection: stays lit while focus is in the detail pane. */
    lv_obj_set_style_bg_color(row, lv_color_hex(0x223046), LV_STATE_CHECKED);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_STATE_CHECKED);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(row, LV_OBJ_FLAG_EVENT_BUBBLE);   /* Back bubbles to the sidebar */

    lv_obj_t *name = lv_label_create(row);
    lv_label_set_text(name, ctm_dev_label(d));
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
    lv_obj_set_flex_grow(name, 1);
    lv_obj_set_style_text_color(name, CTM_COL_TXT, 0);

    lv_obj_t *st = lv_label_create(row);
    if (strcmp(d->kind, "hid") == 0) {
        lv_label_set_text(st, "(hid)");
        lv_obj_set_style_text_color(st, CTM_COL_SUB, 0);
    } else if (d->plugged) {
        lv_label_set_text(st, "plugged");
        lv_obj_set_style_text_color(st, CTM_COL_OK, 0);
    } else {
        lv_label_set_text(st, "idle");
        lv_obj_set_style_text_color(st, CTM_COL_SUB, 0);
    }
    lv_obj_set_style_text_font(st, lv_theme_get_font_small(row), 0);

    lv_group_add_obj(s_ctm_nav_group, row);
    lv_obj_add_event_cb(row, ctm_nav_key_cb, LV_EVENT_KEY, (void *) (intptr_t) idx);
    lv_obj_add_event_cb(row, ctm_nav_cancel_cb, LV_EVENT_CANCEL, NULL);
    lv_obj_add_event_cb(row, ctm_dev_focus_cb, LV_EVENT_FOCUSED, (void *) (intptr_t) idx);
    lv_obj_add_event_cb(row, ctm_dev_click_cb, LV_EVENT_CLICKED, (void *) (intptr_t) idx);
    return row;
}

static void ctm_act_plugall_cb(lv_event_t *e)   { LV_UNUSED(e); ctm_bridge_plug_all();   ctm_request_refresh(); }
static void ctm_act_unplugall_cb(lv_event_t *e) { LV_UNUSED(e); ctm_bridge_unplug_all(); ctm_request_refresh(); }
static void ctm_act_close_cb(lv_event_t *e)     { LV_UNUSED(e); ctm_request_close(); }

static void ctm_make_action(const char *label, lv_event_cb_t cb, lv_color_t bg) {
    lv_obj_t *btn = ctm_nice_btn(s_ctm_sidebar, label, bg);
    lv_obj_set_width(btn, LV_PCT(100));
    lv_group_add_obj(s_ctm_nav_group, btn);
    lv_obj_add_event_cb(btn, ctm_nav_key_cb, LV_EVENT_KEY, (void *) (intptr_t) -1);
    lv_obj_add_event_cb(btn, ctm_nav_cancel_cb, LV_EVENT_CANCEL, NULL);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
}

static void ctm_panel_refresh(void) {
    if (!s_ctm_panel) {
        return;
    }
    s_ctm_ndev = ctm_bridge_list(s_ctm_devs, 16);

    lv_group_remove_all_objs(s_ctm_nav_group);
    lv_obj_clean(s_ctm_sidebar);
    for (int i = 0; i < 16; ++i) s_ctm_dev_rows[i] = NULL;

    lv_obj_t *caption = lv_label_create(s_ctm_sidebar);
    lv_label_set_text(caption, "CONTROLLERS");
    lv_obj_set_style_text_color(caption, CTM_COL_SUB, 0);
    lv_obj_set_style_text_font(caption, lv_theme_get_font_small(caption), 0);

    if (s_ctm_status_lbl) {
        int plugged = 0;
        for (int i = 0; i < s_ctm_ndev; ++i) {
            if (s_ctm_devs[i].plugged) plugged++;
        }
        char agent[64];
        ctm_bridge_agent(agent, sizeof agent);
        lv_label_set_text_fmt(s_ctm_status_lbl, "Agent %s  -  %d bridged", agent, plugged);
    }

    if (s_ctm_ndev == 0) {
        lv_obj_t *l = lv_label_create(s_ctm_sidebar);
        lv_label_set_text(l, "No controllers detected.");
        lv_obj_set_style_text_color(l, CTM_COL_SUB, 0);
        lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(l, LV_PCT(100));
    }
    for (int i = 0; i < s_ctm_ndev; ++i) {
        s_ctm_dev_rows[i] = ctm_make_dev_row(&s_ctm_devs[i], i);
    }
    lv_obj_t *divider = lv_obj_create(s_ctm_sidebar);
    lv_obj_remove_style_all(divider);
    lv_obj_set_size(divider, LV_PCT(100), LV_DPX(1));
    lv_obj_set_style_bg_color(divider, CTM_COL_BORDER, 0);
    lv_obj_set_style_bg_opa(divider, LV_OPA_COVER, 0);
    ctm_make_action("Plug all", ctm_act_plugall_cb, lv_palette_darken(LV_PALETTE_GREEN, 2));
    ctm_make_action("Unplug all", ctm_act_unplugall_cb, lv_palette_darken(LV_PALETTE_BLUE_GREY, 2));

    if (s_ctm_sel >= s_ctm_ndev) {
        s_ctm_sel = s_ctm_ndev > 0 ? s_ctm_ndev - 1 : 0;
    }
    ctm_build_detail(s_ctm_ndev > 0 ? s_ctm_sel : -1);

    /* Restore focus to the correct group after the rebuild. */
    if (s_ctm_detail_open && s_ctm_nrows > 0) {
        app_input_set_group(&s_ctm_owner->global->ui.input, s_ctm_detail_group);
        lv_group_focus_obj(s_ctm_rows[0].row);
        lv_obj_add_state(s_ctm_rows[0].row, LV_STATE_FOCUS_KEY);
        if (s_ctm_dev_rows[s_ctm_sel]) {
            lv_obj_add_state(s_ctm_dev_rows[s_ctm_sel], LV_STATE_CHECKED);
        }
    } else {
        s_ctm_detail_open = false;
        app_input_set_group(&s_ctm_owner->global->ui.input, s_ctm_nav_group);
        if (s_ctm_ndev > 0 && s_ctm_dev_rows[s_ctm_sel]) {
            lv_group_focus_obj(s_ctm_dev_rows[s_ctm_sel]);
            lv_obj_add_state(s_ctm_dev_rows[s_ctm_sel], LV_STATE_FOCUS_KEY);
        }
    }
}

static void ctm_teardown_async(void *p) {
    LV_UNUSED(p);
    if (s_ctm_dead_panel)  { lv_obj_del(s_ctm_dead_panel);    s_ctm_dead_panel = NULL; }
    if (s_ctm_dead_nav)    { lv_group_del(s_ctm_dead_nav);    s_ctm_dead_nav = NULL; }
    if (s_ctm_dead_detail) { lv_group_del(s_ctm_dead_detail); s_ctm_dead_detail = NULL; }
}

static void ctm_close_panel(void) {
    if (!s_ctm_panel) {
        return;
    }
    /* Hide NOW (same-frame UI change) and move input back to the overlay; the old
     * group stays alive until the async teardown, so the in-flight keypad event
     * that triggered this close keeps a valid group pointer. */
    lv_obj_add_flag(s_ctm_panel, LV_OBJ_FLAG_HIDDEN);
    if (s_ctm_owner) {
        app_input_set_group(&s_ctm_owner->global->ui.input, s_ctm_owner->group);
    }
    s_ctm_dead_panel = s_ctm_panel;
    s_ctm_dead_nav = s_ctm_nav_group;
    s_ctm_dead_detail = s_ctm_detail_group;
    s_ctm_panel = NULL;
    s_ctm_sidebar = NULL;
    s_ctm_detail = NULL;
    s_ctm_status_lbl = NULL;
    s_ctm_nav_group = NULL;
    s_ctm_detail_group = NULL;
    s_ctm_nrows = 0;
    s_ctm_detail_open = false;
    s_ctm_owner = NULL;
    lv_async_call(ctm_teardown_async, NULL);
}

/* Close is already deferred internally, so call it directly (synchronous hide). */
static void ctm_request_close(void) { ctm_close_panel(); }

static void ctm_refresh_async(void *p) { LV_UNUSED(p); ctm_panel_refresh(); }
static void ctm_request_refresh(void)  { lv_async_call(ctm_refresh_async, NULL); }

static void open_ctm_panel(lv_event_t *event) {
    /* The CTM button has LV_OBJ_FLAG_EVENT_BUBBLE; stop the CLICKED here so it
     * never reaches the overlay root's hide_overlay handler. hide_overlay calls
     * app_set_mouse_grab(true) -> SDL_ShowCursor(FALSE) (that hid the webOS
     * pointer) and hides the action bar. THIS is the real "pointer disappears". */
    lv_event_stop_bubbling(event);
    streaming_controller_t *controller = lv_event_get_user_data(event);
    if (s_ctm_panel || !controller) {
        return;
    }
    s_ctm_owner = controller;
    s_ctm_detail_open = false;
    s_ctm_sel = 0;
    s_ctm_nrows = 0;

    s_ctm_nav_group = lv_group_create();
    s_ctm_detail_group = lv_group_create();
    lv_group_set_wrap(s_ctm_nav_group, false);
    lv_group_set_wrap(s_ctm_detail_group, false);

    /* Full-screen dim backdrop on the act screen (detached_root) so it does NOT
     * flip the UI into key/gamepad mode the way a modal does (that hid the webOS
     * cursor). Clickable so stray clicks don't dismiss the streaming overlay. */
    lv_obj_t *panel = lv_obj_create(controller->detached_root);
    s_ctm_panel = panel;
    lv_obj_remove_style_all(panel);
    lv_obj_set_size(panel, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(panel, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_60, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(panel, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_move_foreground(panel);

    lv_obj_t *card = lv_obj_create(panel);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, LV_PCT(82), LV_PCT(84));
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, CTM_COL_CARD, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, LV_DPX(12), 0);
    lv_obj_set_style_border_width(card, LV_DPX(1), 0);
    lv_obj_set_style_border_color(card, CTM_COL_BORDER, 0);
    lv_obj_set_style_pad_all(card, LV_DPX(16), 0);
    lv_obj_set_style_pad_gap(card, LV_DPX(12), 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *header = lv_obj_create(card);
    lv_obj_remove_style_all(header);
    lv_obj_set_size(header, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(header, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_gap(header, LV_DPX(2), 0);
    lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *titlerow = lv_obj_create(header);
    lv_obj_remove_style_all(titlerow);
    lv_obj_set_size(titlerow, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(titlerow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(titlerow, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(titlerow, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *title = lv_label_create(titlerow);
    lv_label_set_text(title, "CTM Bridge");
    lv_obj_set_style_text_color(title, CTM_COL_TXT, 0);
    lv_obj_set_style_text_font(title, lv_theme_get_font_large(title), 0);

    /* Real close button (pointer); keyboard/gamepad close with Back (B). */
    lv_obj_t *closebtn = ctm_nice_btn(titlerow, "Close", lv_palette_darken(LV_PALETTE_RED, 3));
    lv_obj_add_event_cb(closebtn, ctm_act_close_cb, LV_EVENT_CLICKED, NULL);

    s_ctm_status_lbl = lv_label_create(header);
    lv_obj_set_style_text_color(s_ctm_status_lbl, CTM_COL_SUB, 0);
    lv_obj_set_style_text_font(s_ctm_status_lbl, lv_theme_get_font_small(header), 0);

    lv_obj_t *body = lv_obj_create(card);
    lv_obj_remove_style_all(body);
    lv_obj_set_width(body, LV_PCT(100));
    lv_obj_set_flex_grow(body, 1);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_gap(body, LV_DPX(12), 0);
    lv_obj_clear_flag(body, LV_OBJ_FLAG_SCROLLABLE);

    s_ctm_sidebar = lv_obj_create(body);
    lv_obj_remove_style_all(s_ctm_sidebar);
    lv_obj_set_size(s_ctm_sidebar, LV_PCT(38), LV_PCT(100));
    lv_obj_set_flex_flow(s_ctm_sidebar, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(s_ctm_sidebar, LV_DPX(8), 0);
    lv_obj_set_style_pad_gap(s_ctm_sidebar, LV_DPX(8), 0);
    lv_obj_set_style_bg_color(s_ctm_sidebar, CTM_COL_SIDEBAR, 0);
    lv_obj_set_style_bg_opa(s_ctm_sidebar, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_ctm_sidebar, LV_DPX(8), 0);
    lv_obj_set_style_border_width(s_ctm_sidebar, LV_DPX(1), 0);
    lv_obj_set_style_border_color(s_ctm_sidebar, CTM_COL_BORDER, 0);
    lv_obj_set_scrollbar_mode(s_ctm_sidebar, LV_SCROLLBAR_MODE_AUTO);
    /* Back backstop: any focusable child bubbles CANCEL up here -> close panel. */
    lv_obj_add_event_cb(s_ctm_sidebar, ctm_nav_cancel_cb, LV_EVENT_CANCEL, NULL);

    s_ctm_detail = lv_obj_create(body);
    lv_obj_remove_style_all(s_ctm_detail);
    lv_obj_set_height(s_ctm_detail, LV_PCT(100));
    lv_obj_set_flex_grow(s_ctm_detail, 1);
    lv_obj_set_flex_flow(s_ctm_detail, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(s_ctm_detail, LV_DPX(8), 0);
    lv_obj_set_style_pad_gap(s_ctm_detail, LV_DPX(8), 0);
    lv_obj_set_scrollbar_mode(s_ctm_detail, LV_SCROLLBAR_MODE_AUTO);
    /* Back backstop: a focused detail card bubbles CANCEL up here -> back to sidebar. */
    lv_obj_add_event_cb(s_ctm_detail, ctm_detail_cancel_cb, LV_EVENT_CANCEL, NULL);

    app_input_set_group(&controller->global->ui.input, s_ctm_nav_group);
    ctm_panel_refresh();
}

bool show_overlay(streaming_controller_t *controller) {
    if (overlay_showing) {
        return false;
    }
    overlay_showing = true;
    lv_obj_clear_flag(controller->base.obj, LV_OBJ_FLAG_HIDDEN);

    lv_area_t coords = controller->video->coords;
    streaming_enter_overlay(controller->global->session, coords.x1, coords.y1, lv_area_get_width(&coords),
                            lv_area_get_height(&coords));
    streaming_refresh_stats();

    app_stop_text_input(&controller->global->ui.input);

    update_buttons_layout(controller);

    return true;
}

/* B/Back: close keyboard if shown, else hide overlay */
static void on_cancel_key(lv_event_t *event) {
    streaming_controller_t *controller = lv_event_get_user_data(event);
    if (streaming_soft_keyboard_shown()) {
        soft_keyboard_close_cb(controller);
    } else {
        hide_overlay(event);
    }
}

static void hide_overlay_impl(streaming_controller_t *controller) {
    app_input_set_button_points(&controller->global->ui.input, NULL);
    lv_obj_add_flag(controller->base.obj, LV_OBJ_FLAG_HIDDEN);
    if (!overlay_showing) {
        return;
    }
    overlay_showing = false;
    app_set_mouse_grab(&controller->global->input, true);
    streaming_enter_fullscreen(controller->global->session);
}

static void hide_overlay(lv_event_t *event) {
    hide_overlay_impl((streaming_controller_t *) lv_event_get_user_data(event));
}

static void overlay_key_cb(lv_event_t *e) {
    streaming_controller_t *controller = lv_event_get_user_data(e);
    lv_group_t *group = controller->group;
    switch (lv_event_get_key(e)) {
        case LV_KEY_LEFT:
            lv_group_focus_prev(group);
            break;
        case LV_KEY_RIGHT:
            lv_group_focus_next(group);
            break;
        default:
            break;
    }
}

static void update_buttons_layout(streaming_controller_t *controller) {
    lv_area_t coords;
    lv_obj_get_coords(controller->quit_btn, &coords);
    lv_area_center(&coords, &controller->button_points[1]);
    lv_obj_get_coords(controller->suspend_btn, &coords);
    lv_area_center(&coords, &controller->button_points[3]);
    lv_obj_get_coords(controller->kbd_btn, &coords);
    lv_area_center(&coords, &controller->button_points[4]);
    app_input_set_button_points(&controller->global->ui.input, controller->button_points);
}

void streaming_toggle_stats_pin(void) {
    if (!current_controller) {
        return;
    }
    streaming_set_stats_pinned(current_controller, !overlay_pinned);
}

static void streaming_set_stats_pinned(streaming_controller_t *controller, bool pinned) {
    if (!controller || !controller->stats) {
        return;
    }
    lv_obj_t *stats = controller->stats;
    bool currently_pinned = stats->parent == lv_layer_top();
    if (pinned == currently_pinned) {
        overlay_pinned = pinned;
        return;
    }
    overlay_pinned = pinned;
    if (pinned) {
        lv_obj_clear_flag(stats, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_parent(stats, lv_layer_top());
        if (app_configuration->show_stats_compact) {
            lv_obj_align(stats, LV_ALIGN_TOP_LEFT, 0, 0);
        } else {
            lv_obj_align(stats, LV_ALIGN_TOP_RIGHT, -LV_DPX(20), LV_DPX(20));
        }
        lv_obj_add_state(stats, LV_STATE_USER_1);
        lv_obj_add_state(controller->stats_pin, LV_STATE_CHECKED);
        streaming_refresh_stats();
    } else {
        lv_obj_set_parent(stats, controller->base.obj);
        lv_obj_clear_state(stats, LV_STATE_USER_1);
        lv_obj_clear_state(controller->stats_pin, LV_STATE_CHECKED);
    }
}

static void pin_toggle(lv_event_t *e) {
    lv_obj_t *toggle_view = lv_event_get_user_data(e);
    lv_fragment_t *fragment = lv_obj_get_user_data(toggle_view);
    bool checked = lv_obj_has_state(lv_event_get_current_target(e), LV_STATE_CHECKED);
    streaming_set_stats_pinned((streaming_controller_t *) fragment, checked);
}
