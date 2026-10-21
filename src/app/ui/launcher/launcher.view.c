#include "launcher.controller.h"
#include "apps.controller.h"
#include "stream_actions.h"

#include "ui/root.h"

#include "lvgl.h"

#include "lvgl/ext/lv_child_group.h"
#include "lvgl/util/lv_app_utils.h"
#include "lvgl/font/material_icons_regular_symbols.h"

#include "util/font.h"
#include "util/i18n.h"
#include "profile/profile_manager.h"

#include <string.h>

#include "app.h"
#include "lvgl/theme/lv_theme_moonlight.h"
#include "lvgl/theme/lv_theme_moonlight_colors.h"

#define TOPBAR_HEIGHT LAUNCHER_TOPBAR_DPX
#define TOPBAR_BTN_SIZE 40

static void launcher_layout_settings_layer(launcher_fragment_t *controller, lv_obj_t *shell);

static void launcher_shell_resized(lv_event_t *event);

static void detail_group_add(lv_event_t *event);

static lv_obj_t *create_topbar_icon_btn(launcher_fragment_t *controller, lv_obj_t *parent, const char *icon);

static void launcher_profile_changed(lv_event_t *event);

static void launcher_refresh_profile_dropdown(launcher_fragment_t *controller);

static void launcher_layout_settings_layer(launcher_fragment_t *controller, lv_obj_t *shell) {
    if (!controller || !controller->settings_layer || !shell) {
        return;
    }
    lv_coord_t w = lv_obj_get_width(shell);
    lv_coord_t h = lv_obj_get_height(shell);
    lv_coord_t bar_h = LV_DPX(TOPBAR_HEIGHT);
    if (w <= 0 || h <= bar_h) {
        return;
    }
    lv_obj_set_size(controller->settings_layer, w, h - bar_h);
    lv_obj_align(controller->settings_layer, LV_ALIGN_TOP_LEFT, 0, bar_h);
    lv_obj_move_foreground(controller->settings_layer);
}

static void launcher_shell_resized(lv_event_t *event) {
    launcher_fragment_t *controller = lv_event_get_user_data(event);
    launcher_layout_settings_layer(controller, lv_event_get_target(event));
}

lv_obj_t *launcher_win_create(lv_fragment_t *self, lv_obj_t *parent) {
    launcher_fragment_t *controller = (launcher_fragment_t *) self;
    /* Root window: header is hidden (we provide our own top bar inside the content area
     * so the bar's height is exactly what we control via TOPBAR_HEIGHT). */
    lv_obj_t *win = lv_win_create(parent, 0);

    lv_obj_t *header = lv_win_get_header(win);
    lv_obj_add_flag(header, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *content = lv_win_get_content(win);
    lv_obj_clear_flag(content, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_layout(content, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(content, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_all(content, 0, 0);
    lv_obj_set_style_pad_gap(content, 0, 0);

    /* Shell: top bar + game grid (column layout, no overlay). */
    lv_obj_t *shell = lv_obj_create(content);
    lv_obj_remove_style_all(shell);
    lv_obj_set_width(shell, LV_PCT(100));
    lv_obj_set_flex_grow(shell, 1);
    lv_obj_clear_flag(shell, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_layout(shell, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(shell, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(shell, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_all(shell, 0, 0);
    lv_obj_set_style_pad_gap(shell, 0, 0);

    /* Two focus groups: nav_group for top-bar buttons, detail_group for game grid items. */
    controller->nav_group = lv_group_create();
    controller->detail_group = lv_group_create();

    /* Shared style for the round icon-only top-bar action buttons. */
    lv_style_init(&controller->topbar_btn_style);
    lv_style_set_radius(&controller->topbar_btn_style, LV_RADIUS_CIRCLE);
    lv_style_set_pad_all(&controller->topbar_btn_style, 0);
    lv_style_set_border_width(&controller->topbar_btn_style, 0);
    lv_style_set_bg_color(&controller->topbar_btn_style, ml_color_hex(ML_COLOR_SURFACE_ALT));
    lv_style_set_bg_opa(&controller->topbar_btn_style, LV_OPA_COVER);
    lv_style_set_text_color(&controller->topbar_btn_style, ml_color_hex(ML_COLOR_TEXT));

    /* ---------------- Top bar ---------------- */
    lv_obj_t *topbar = lv_obj_create(shell);
    lv_obj_remove_style_all(topbar);
    lv_obj_set_width(topbar, LV_PCT(100));
    lv_obj_set_height(topbar, LV_DPX(TOPBAR_HEIGHT));
    lv_obj_set_layout(topbar, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(topbar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(topbar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_bg_color(topbar, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(topbar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(topbar, 0, 0);
    lv_obj_set_style_pad_hor(topbar, LV_DPX(20), 0);
    lv_obj_set_style_pad_ver(topbar, LV_DPX(8), 0);
    lv_obj_set_style_pad_gap(topbar, LV_DPX(10), 0);
    lv_obj_clear_flag(topbar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(topbar, cb_child_group_add, LV_EVENT_CHILD_CREATED, controller->nav_group);

    /* App icon (same asset as OS launcher icons). */
    lv_obj_t *title_logo = lv_img_create(topbar);
    lv_img_set_src(title_logo, ui_logo_src());
    lv_obj_set_size(title_logo, LV_DPX(NAV_LOGO_SIZE), LV_DPX(NAV_LOGO_SIZE));
    lv_img_set_size_mode(title_logo, LV_IMG_SIZE_MODE_VIRTUAL);
    lv_obj_clear_flag(title_logo, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title_label = lv_label_create(topbar);
    lv_obj_set_style_text_font(title_label, lv_theme_get_font_large(topbar), 0);
    lv_obj_set_style_text_color(title_label, ml_color_hex(ML_COLOR_TEXT), 0);
    lv_label_set_text_static(title_label, "AURORA");

    controller->profile_dropdown = lv_dropdown_create(topbar);
    lv_obj_set_style_max_width(controller->profile_dropdown, LV_DPX(160), 0);
    lv_obj_add_event_cb(controller->profile_dropdown, launcher_profile_changed, LV_EVENT_VALUE_CHANGED, controller);
    launcher_refresh_profile_dropdown(controller);
    launcher_attach_profile_dropdown_nav(controller);

    /* Spacer that grows to push the action buttons to the right edge. */
    lv_obj_t *spacer = lv_obj_create(topbar);
    lv_obj_remove_style_all(spacer);
    lv_obj_set_height(spacer, LV_DPX(1));
    lv_obj_set_flex_grow(spacer, 1);

    /* Server selector button. Renders the currently selected PC name; click opens
     * the server popup with the full pclist (status icons + long-press menu kept). */
    lv_obj_t *server_btn = lv_btn_create(topbar);
    lv_obj_add_flag(server_btn, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_set_height(server_btn, LV_DPX(TOPBAR_BTN_SIZE));
    lv_obj_set_style_pad_hor(server_btn, LV_DPX(14), 0);
    lv_obj_set_style_radius(server_btn, LV_DPX(12), 0);
    lv_obj_set_style_bg_color(server_btn, ml_color_hex(ML_COLOR_SURFACE_ALT), 0);
    lv_obj_set_style_bg_opa(server_btn, LV_OPA_70, 0);
    lv_obj_set_style_text_color(server_btn, ml_color_hex(ML_COLOR_TEXT), 0);
    lv_obj_set_layout(server_btn, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(server_btn, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(server_btn, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(server_btn, LV_DPX(6), 0);

    lv_obj_t *srv_icon = lv_label_create(server_btn);
    lv_obj_set_style_text_font(srv_icon, lv_theme_moonlight_get_iconfont_small(server_btn), 0);
    lv_label_set_text_static(srv_icon, MAT_SYMBOL_TV);
    lv_obj_clear_flag(srv_icon, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *server_label = lv_label_create(server_btn);
    lv_obj_set_style_text_font(server_label, lv_theme_get_font_small(server_btn), 0);
    lv_label_set_long_mode(server_label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_max_width(server_label, LV_DPX(180), 0);
    lv_label_set_text_static(server_label, locstr("Select server"));
    lv_obj_clear_flag(server_label, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *srv_chevron = lv_label_create(server_btn);
    lv_obj_set_style_text_font(srv_chevron, lv_theme_moonlight_get_iconfont_small(server_btn), 0);
    lv_label_set_text_static(srv_chevron, MAT_SYMBOL_ARROW_DROP_DOWN);
    lv_obj_clear_flag(srv_chevron, LV_OBJ_FLAG_CLICKABLE);

    stream_actions_button(controller, topbar);

    /* Right-side action buttons (icon only). */
    lv_obj_t *add_btn = create_topbar_icon_btn(controller, topbar, MAT_SYMBOL_ADD_TO_QUEUE);
    lv_obj_t *help_btn = create_topbar_icon_btn(controller, topbar, MAT_SYMBOL_HELP);
    lv_obj_t *pref_btn = create_topbar_icon_btn(controller, topbar, MAT_SYMBOL_SETTINGS);
    lv_obj_t *quit_btn = create_topbar_icon_btn(controller, topbar, MAT_SYMBOL_CLOSE);

    /* ---------------- Game grid (fills remaining space below top bar) ---------------- */
    lv_obj_t *detail_stack = lv_obj_create(shell);
    lv_obj_remove_style_all(detail_stack);
    lv_obj_set_width(detail_stack, LV_PCT(100));
    lv_obj_set_flex_grow(detail_stack, 1);
    lv_obj_clear_flag(detail_stack, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *detail = lv_obj_create(detail_stack);
    lv_obj_remove_style_all(detail);
    lv_obj_set_width(detail, LV_PCT(100));
    lv_obj_set_height(detail, LV_PCT(100));
    lv_obj_clear_flag(detail, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(detail, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(detail, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(detail, 0, 0);
    lv_obj_add_event_cb(detail, detail_group_add, LV_EVENT_CHILD_CREATED, controller);

    /* Settings overlay: floats over the game grid, below the top bar. */
    lv_obj_t *settings_layer = lv_obj_create(shell);
    lv_obj_remove_style_all(settings_layer);
    lv_obj_add_flag(settings_layer, LV_OBJ_FLAG_HIDDEN | LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_clear_flag(settings_layer, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(settings_layer, LV_OPA_TRANSP, 0);
    lv_obj_add_event_cb(shell, launcher_shell_resized, LV_EVENT_SIZE_CHANGED, controller);

    controller->nav = topbar;
    controller->detail = detail;
    controller->settings_layer = settings_layer;
    controller->server_btn = server_btn;
    controller->server_label = server_label;
    controller->add_btn = add_btn;
    controller->help_btn = help_btn;
    controller->pref_btn = pref_btn;
    controller->quit_btn = quit_btn;
    launcher_layout_settings_layer(controller, shell);
    return win;
}

static lv_obj_t *create_topbar_icon_btn(launcher_fragment_t *controller, lv_obj_t *parent, const char *icon) {
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_set_size(btn, LV_DPX(TOPBAR_BTN_SIZE), LV_DPX(TOPBAR_BTN_SIZE));
    lv_obj_add_style(btn, &controller->topbar_btn_style, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_70, 0);
    lv_obj_t *lbl = lv_label_create(btn);
    lv_obj_set_style_text_font(lbl, lv_theme_moonlight_get_iconfont_small(parent), 0);
    lv_obj_set_style_text_color(lbl, ml_color_hex(ML_COLOR_TEXT), 0);
    lv_label_set_text_static(lbl, icon);
    lv_obj_center(lbl);
    lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
    return btn;
}

static void detail_group_add(lv_event_t *event) {
    lv_obj_t *child = lv_event_get_param(event);
    launcher_fragment_t *fragment = lv_event_get_user_data(event);
    apps_fragment_t *apps_fragment = (apps_fragment_t *) lv_fragment_manager_find_by_container(
            fragment->base.child_manager, fragment->detail);
    lv_obj_t *view = lv_obj_get_child(fragment->detail, 0);
    if (!apps_fragment || !view) return;
    if (!child || !lv_obj_is_group_def(child)) return;
    if (lv_obj_get_group(child)) {
        lv_group_remove_obj(child);
    }
    /* Accept children of the apps view or the apperror panel. */
    if (child->parent != view && child->parent != apps_fragment->apperror) {
        return;
    }
    lv_group_add_obj(fragment->detail_group, child);
}

static void launcher_refresh_profile_dropdown(launcher_fragment_t *controller) {
    if (!controller->profile_dropdown) {
        return;
    }
    char options[512] = {0};
    int active_index = 0;
    const char *active_id = profile_manager_active_id();
    for (int i = 0; i < profile_manager_count(); i++) {
        const streaming_profile_t *profile = profile_manager_get(i);
        if (!profile) {
            continue;
        }
        if (i > 0) {
            strcat(options, "\n");
        }
        strcat(options, profile->name);
        if (active_id && strcmp(profile->id, active_id) == 0) {
            active_index = i;
        }
    }
    lv_dropdown_set_options(controller->profile_dropdown, options[0] ? options : "Default");
    lv_dropdown_set_selected(controller->profile_dropdown, active_index);
}

static void launcher_profile_changed(lv_event_t *event) {
    launcher_fragment_t *controller = lv_event_get_user_data(event);
    uint16_t index = lv_dropdown_get_selected(controller->profile_dropdown);
    const streaming_profile_t *profile = profile_manager_get(index);
    if (!profile) {
        return;
    }
    profile_manager_set_active(profile->id);
    profile_manager_apply_to_settings(app_configuration);
}
