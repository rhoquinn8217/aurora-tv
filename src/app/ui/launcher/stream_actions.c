#include "stream_actions.h"

#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app.h"
#include "logging.h"
#include "client.h"
#include "errors.h"
#include "backend/pcmanager.h"
#include "ui/common/progress_dialog.h"
#include "ui/streaming/streaming.controller.h"
#include "util/bus.h"
#include "util/i18n.h"

#include "lvgl/util/lv_app_utils.h"
#include "lvgl/font/material_icons_regular_symbols.h"
#include "lvgl/theme/lv_theme_moonlight.h"
#include "lvgl/theme/lv_theme_moonlight_colors.h"

/* Vibepollo's controls are apps with ids it keeps fixed (src/remote_session.h
 * of Vibepollo 2.0.1): the plain set, and a second set for a TV that did not
 * start the running app. Both sets are numbered high, out of the way of any
 * id an ordinary app gets. */
typedef enum action_kind_t {
    ACTION_NONE = 0,
    ACTION_RESUME,
    ACTION_TERMINATE,
    ACTION_INPUT,
    ACTION_MONITOR,
    ACTION_DISCONNECT_INPUT,
    ACTION_DISCONNECT_MONITOR,
    /* Left out of the grid and out of the menu: Resume joins the same app. */
    ACTION_RUNNING_COPY,
} action_kind_t;

static action_kind_t kind_of_id(int id) {
    switch (id) {
        case 2147483501:
        case 2147483511:
            return ACTION_RESUME;
        case 2147483502:
            return ACTION_DISCONNECT_MONITOR;
        case 2147483503:
            return ACTION_DISCONNECT_INPUT;
        case 2147483504:
        case 2147483514:
            return ACTION_TERMINATE;
        case 2147483505:
        case 2147483515:
            return ACTION_MONITOR;
        case 2147483506:
        case 2147483516:
            return ACTION_INPUT;
        case 2147483507:
            return ACTION_RUNNING_COPY;
        default:
            return ACTION_NONE;
    }
}

#define MAX_ACTIONS 8
#define TITLE_MAX 96
#define TOPBAR_BTN_SIZE 40

typedef struct stream_action_t {
    action_kind_t kind;
    int id;
    char title[TITLE_MAX];
} stream_action_t;

static app_t *s_app;
static lv_obj_t *s_btn;
static uuidstr_t s_host;
static stream_action_t s_actions[MAX_ACTIONS];
static int s_count;
/* The running app's name, from its copy: what Terminate would close. */
static char s_running[TITLE_MAX];
static lv_obj_t *s_progress;

static void button_clicked(lv_event_t *event);

static void button_deleted(lv_event_t *event);

static void free_user_data(lv_event_t *event);

static void menu_cancel(lv_event_t *event);

static void menu_clicked(lv_event_t *event);

static void confirm_clicked(lv_event_t *event);

static void result_clicked(lv_event_t *event);

static int terminate_worker(void *data);

static void terminate_done(void *data);

/* The name without the spaces Vibepollo puts in front to sort its controls
 * first. */
static const char *visible_name(const char *name) {
    if (name == NULL) {
        return "";
    }
    while (*name == ' ') {
        name++;
    }
    return name;
}

/* The copy of the running app: Vibepollo gives it leading spaces and the
 * running app's id with bit 30 flipped (synthetic_running_game_id), so it is
 * known by both, against the app it copies. */
static bool is_running_copy(const apploader_list_t *apps, size_t index) {
    const APP_LIST *copy = &apps->items[index].base;
    if (copy->name == NULL || copy->name[0] != ' ') {
        return false;
    }
    const char *name = visible_name(copy->name);
    for (size_t i = 0; i < apps->count; i++) {
        const APP_LIST *other = &apps->items[i].base;
        if (i == index || other->name == NULL || strcmp(visible_name(other->name), name) != 0) {
            continue;
        }
        if ((((unsigned) other->id ^ 0x40000000U) & 0x7fffffffU) == (unsigned) copy->id) {
            return true;
        }
    }
    return false;
}

static int kind_rank(action_kind_t kind) {
    /* The menu's order, their picture of 2026-10-17: Resume, Terminate,
     * Remote Input, Remote Monitor; then the two that end a remote role. */
    switch (kind) {
        case ACTION_RESUME:
            return 0;
        case ACTION_TERMINATE:
            return 1;
        case ACTION_INPUT:
            return 2;
        case ACTION_MONITOR:
            return 3;
        case ACTION_DISCONNECT_INPUT:
            return 4;
        default:
            return 5;
    }
}

static int action_compare(const void *a, const void *b) {
    return kind_rank(((const stream_action_t *) a)->kind) - kind_rank(((const stream_action_t *) b)->kind);
}

static void update_button(void) {
    if (s_btn == NULL) {
        return;
    }
    if (s_count > 0) {
        lv_obj_clear_flag(s_btn, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_btn, LV_OBJ_FLAG_HIDDEN);
    }
}

void stream_actions_take(const uuidstr_t *host, apploader_list_t *apps) {
    if (host == NULL || apps == NULL) {
        return;
    }
    s_host = *host;
    s_count = 0;
    s_running[0] = '\0';
    if (apps->count == 0) {
        update_button();
        return;
    }
    /* Decided on the whole list first: the copy is known against its twin. */
    action_kind_t *kinds = calloc(apps->count, sizeof(action_kind_t));
    if (kinds == NULL) {
        return;
    }
    for (size_t i = 0; i < apps->count; i++) {
        kinds[i] = kind_of_id(apps->items[i].base.id);
        if (kinds[i] == ACTION_NONE && is_running_copy(apps, i)) {
            kinds[i] = ACTION_RUNNING_COPY;
        }
    }
    size_t keep = 0;
    for (size_t i = 0; i < apps->count; i++) {
        apploader_item_t *item = &apps->items[i];
        if (kinds[i] == ACTION_NONE) {
            apps->items[keep++] = *item;
            continue;
        }
        if (kinds[i] == ACTION_RUNNING_COPY) {
            snprintf(s_running, sizeof(s_running), "%s", visible_name(item->base.name));
        } else if (s_count < MAX_ACTIONS) {
            stream_action_t *action = &s_actions[s_count++];
            action->kind = kinds[i];
            action->id = item->base.id;
            snprintf(action->title, sizeof(action->title), "%s", visible_name(item->base.name));
        }
        free(item->base.name);
        item->base.name = NULL;
    }
    apps->count = keep;
    free(kinds);
    qsort(s_actions, s_count, sizeof(stream_action_t), action_compare);
    update_button();
}

void stream_actions_button(launcher_fragment_t *launcher, lv_obj_t *topbar) {
    s_app = launcher->global;
    /* Styled as the host button beside it. A child of the top bar joins its
     * focus group, and its keys bubble to the bar, which moves left, right and
     * down to the grid. */
    lv_obj_t *btn = lv_btn_create(topbar);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_set_height(btn, LV_DPX(TOPBAR_BTN_SIZE));
    lv_obj_set_style_pad_hor(btn, LV_DPX(14), 0);
    lv_obj_set_style_radius(btn, LV_DPX(12), 0);
    lv_obj_set_style_bg_color(btn, ml_color_hex(ML_COLOR_SURFACE_ALT), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_70, 0);
    lv_obj_set_style_text_color(btn, ml_color_hex(ML_COLOR_TEXT), 0);
    lv_obj_set_layout(btn, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(btn, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btn, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(btn, LV_DPX(6), 0);

    lv_obj_t *label = lv_label_create(btn);
    lv_obj_set_style_text_font(label, lv_theme_get_font_small(btn), 0);
    lv_label_set_text_static(label, locstr("Stream actions"));
    lv_obj_clear_flag(label, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *chevron = lv_label_create(btn);
    lv_obj_set_style_text_font(chevron, lv_theme_moonlight_get_iconfont_small(btn), 0);
    lv_label_set_text_static(chevron, MAT_SYMBOL_ARROW_DROP_DOWN);
    lv_obj_clear_flag(chevron, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_add_event_cb(btn, button_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(btn, button_deleted, LV_EVENT_DELETE, NULL);
    s_btn = btn;
    update_button();
}

static void button_deleted(lv_event_t *event) {
    if (lv_event_get_target(event) == s_btn) {
        s_btn = NULL;
    }
}

/* A menu entry and the question Terminate asks each hold their own copy of
 * the action: the list reloads every few seconds, and a choice must not
 * become a different action under the person's finger. */
static stream_action_t *action_copy(const stream_action_t *action) {
    stream_action_t *copy = malloc(sizeof(stream_action_t));
    if (copy != NULL) {
        *copy = *action;
    }
    return copy;
}

static void free_user_data(lv_event_t *event) {
    lv_obj_t *obj = lv_event_get_target(event);
    free(lv_obj_get_user_data(obj));
    lv_obj_set_user_data(obj, NULL);
}

/* The menu: a box of buttons just under the button, built as the tiles'
 * own long-press menu is, so the pad and the remote move through it the same
 * way and Back closes it. */
static void button_clicked(lv_event_t *event) {
    if (s_count == 0) {
        return;
    }
    lv_obj_t *btn = lv_event_get_target(event);
    lv_obj_t *msgbox = lv_msgbox_create(NULL, NULL, NULL, NULL, false);
    lv_obj_t *backdrop = lv_obj_get_parent(msgbox);
    if (backdrop != NULL) {
        lv_obj_set_style_bg_opa(backdrop, LV_OPA_30, 0);
    }
    lv_obj_set_style_min_width(msgbox, 0, 0);
    lv_obj_set_width(msgbox, LV_DPX(320));
    lv_obj_t *content = lv_msgbox_get_content(msgbox);
    lv_obj_add_flag(content, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_event_cb(content, menu_cancel, LV_EVENT_CANCEL, NULL);
    lv_obj_add_event_cb(content, menu_clicked, LV_EVENT_SHORT_CLICKED, NULL);
    for (int i = 0; i < s_count; i++) {
        lv_obj_t *item = lv_list_add_btn(content, NULL, s_actions[i].title);
        lv_obj_add_flag(item, LV_OBJ_FLAG_EVENT_BUBBLE);
        lv_obj_set_user_data(item, action_copy(&s_actions[i]));
        lv_obj_add_event_cb(item, free_user_data, LV_EVENT_DELETE, NULL);
    }
    lv_obj_update_layout(msgbox);
    lv_obj_align_to(msgbox, btn, LV_ALIGN_OUT_BOTTOM_RIGHT, 0, LV_DPX(6));
}

static void menu_cancel(lv_event_t *event) {
    lv_obj_t *target = lv_event_get_target(event);
    if (target->parent != lv_event_get_current_target(event)) {
        return;
    }
    lv_msgbox_close(lv_event_get_current_target(event)->parent);
}

static void launch(const stream_action_t *action) {
    if (s_app == NULL) {
        return;
    }
    commons_log_info("StreamActions", "%s (app %d)", action->title, action->id);
    /* What pressing its tile did: the streaming screen, which launches it.
     * The session copies the name. */
    streaming_scene_arg_t scene = {
            .global = s_app,
            .uuid = s_host,
            .app = {.name = (char *) action->title, .id = action->id},
    };
    lv_fragment_t *fragment = lv_fragment_create(&streaming_controller_class, &scene);
    if (fragment == NULL) {
        return;
    }
    lv_obj_t *const *container = lv_fragment_get_container(lv_fragment_manager_get_top(s_app->ui.fm));
    lv_fragment_manager_push(s_app->ui.fm, fragment, container);
}

static void confirm_terminate(const stream_action_t *action);

static void ask_host(const stream_action_t *action);

static void menu_clicked(lv_event_t *event) {
    lv_obj_t *target = lv_event_get_target(event);
    if (target->parent != lv_event_get_current_target(event)) {
        return;
    }
    lv_obj_t *msgbox = lv_event_get_current_target(event)->parent;
    const stream_action_t *action = lv_obj_get_user_data(target);
    lv_msgbox_close_async(msgbox);
    if (action == NULL) {
        return;
    }
    switch (action->kind) {
        case ACTION_TERMINATE:
            confirm_terminate(action);
            break;
        case ACTION_DISCONNECT_INPUT:
        case ACTION_DISCONNECT_MONITOR:
            /* Nothing to stream: the host's answer is the whole of it, shown as
             * a message rather than as a stream that failed to start. */
            ask_host(action);
            break;
        default:
            launch(action);
            break;
    }
}

/* ------------------------------------------------------------- Terminate -- */

/* rhoquinn8217, 2026-10-17: no warning in the menu, but "a pop up that comes
 * up to confirm that also ... warns you that everyone will [be] kicked off".
 * Cancel comes first, so a stray press does nothing. */
static void confirm_terminate(const stream_action_t *action) {
    static const char *btn_txts[] = {translatable("Cancel"), translatable("Terminate"), ""};
    lv_obj_t *msgbox = lv_msgbox_create_i18n(NULL, locstr("Terminate?"), "text", btn_txts, false);
    lv_obj_t *text = lv_msgbox_get_text(msgbox);
    const pclist_t *node = pcmanager_node(pcmanager, &s_host);
    const char *host = node != NULL && node->server != NULL && node->server->hostname != NULL
                       ? node->server->hostname : locstr("the host");
    if (s_running[0] != '\0') {
        lv_label_set_text_fmt(text, locstr("This closes %s on %s. Everyone streaming it is kicked off, "
                                           "on every TV."), s_running, host);
    } else {
        lv_label_set_text_fmt(text, locstr("This closes the running app on %s. Everyone streaming it is "
                                           "kicked off, on every TV."), host);
    }
    lv_obj_set_user_data(msgbox, action_copy(action));
    lv_obj_add_event_cb(msgbox, free_user_data, LV_EVENT_DELETE, NULL);
    lv_obj_add_event_cb(msgbox, confirm_clicked, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_center(msgbox);
}

static void confirm_clicked(lv_event_t *event) {
    lv_obj_t *msgbox = lv_event_get_current_target(event);
    const stream_action_t *action = lv_obj_get_user_data(msgbox);
    uint16_t pressed = lv_msgbox_get_active_btn(msgbox);
    if (pressed == 1 && action != NULL) {
        ask_host(action);
    }
    lv_msgbox_close_async(msgbox);
}

typedef struct host_task_t {
    app_t *app;
    uuidstr_t host;
    stream_action_t action;
    /* A copy of the host's data, so the launch call's bookkeeping (it records
     * the id it launched as the running app) never reaches the real one. */
    SERVER_DATA server;
    char *address;
    int ret;
    char message[512];
} host_task_t;

/* A control that streams nothing (Terminate, Disconnect), asked of the host
 * off the screen thread, with "working" on the screen until it answers. */
static void ask_host(const stream_action_t *action) {
    if (s_app == NULL) {
        return;
    }
    const pclist_t *node = pcmanager_node(pcmanager, &s_host);
    if (node == NULL || node->server == NULL || node->server->serverInfo.address == NULL) {
        return;
    }
    host_task_t *task = calloc(1, sizeof(host_task_t));
    if (task == NULL) {
        return;
    }
    task->app = s_app;
    task->host = s_host;
    task->action = *action;
    task->server = *node->server;
    task->address = strdup(node->server->serverInfo.address);
    task->server.serverInfo.address = task->address;
    task->server.modes = NULL;
    task->server.unsupported = true;      /* no display mode to match: nothing streams */
    task->server.currentGame = 0;
    commons_log_info("StreamActions", "%s chosen on the TV: asking the host (app %d)",
                     task->action.title, task->action.id);
    if (s_progress == NULL) {
        s_progress = progress_dialog_create(action->kind == ACTION_TERMINATE ? locstr("Terminating...")
                                                                             : locstr("Please wait..."));
    }
    SDL_Thread *thread = SDL_CreateThread(terminate_worker, "stream-action", task);
    if (thread == NULL) {
        free(task->address);
        free(task);
        if (s_progress != NULL) {
            lv_msgbox_close(s_progress);
            s_progress = NULL;
        }
        return;
    }
    SDL_DetachThread(thread);
}

/* Off the screen thread: the launch Vibepollo answers, at most twice. To a TV
 * that did not start the app, Terminate's first answer is a request to confirm
 * ("Launch Terminate again within 60 seconds"), which the person already did
 * on this TV; the second request is that confirmation. Each answer is a refusal
 * by design (status 410), with the host's words in it. */
static int terminate_worker(void *data) {
    host_task_t *task = data;
    GS_CLIENT client = app_gs_client_new(task->app);
    for (int attempt = 0; attempt < 2; attempt++) {
        STREAM_CONFIGURATION config;
        LiInitializeStreamConfiguration(&config);
        config.width = 1280;
        config.height = 720;
        config.fps = 60;
        config.bitrate = 10000;
        config.packetSize = 1024;
        config.audioConfiguration = AUDIO_CONFIGURATION_STEREO;
        config.supportedVideoFormats = VIDEO_FORMAT_H264;
        SERVER_DATA server = task->server;
        task->ret = gs_start_app(client, &server, &config, task->action.id, server.isGfe, false, false, 0, NULL);
        const char *message = NULL;
        gs_get_error(&message);
        snprintf(task->message, sizeof(task->message), "%s", task->ret == GS_OK ? "" : (message ? message : ""));
        commons_log_info("StreamActions", "%s, request %d: %d %s", task->action.title, attempt + 1, task->ret,
                         task->message);
        if (task->ret == GS_OK || task->action.kind != ACTION_TERMINATE || strstr(task->message, " again ") == NULL) {
            break;
        }
    }
    gs_destroy(client);
    app_bus_post(task->app, (bus_actionfunc) terminate_done, task);
    return 0;
}

static void terminate_done(void *data) {
    host_task_t *task = data;
    if (s_progress != NULL) {
        lv_msgbox_close(s_progress);
        s_progress = NULL;
    }
    /* The host's own words, without libgamestream's "Bad status 410: ". */
    const char *said = task->message;
    const char *colon = strstr(said, ": ");
    if (strncmp(said, "Bad status ", 11) == 0 && colon != NULL) {
        said = colon + 2;
    }
    static const char *btn_txts[] = {translatable("OK"), ""};
    lv_obj_t *msgbox = lv_msgbox_create_i18n(NULL, task->action.title, "text", btn_txts, false);
    lv_label_set_text(lv_msgbox_get_text(msgbox), said[0] != '\0' ? said : locstr("The host did not answer."));
    lv_obj_add_event_cb(msgbox, result_clicked, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_center(msgbox);
    /* The list changes with it: Resume and Terminate go once nothing runs. */
    pcmanager_request_update(pcmanager, &task->host, NULL, NULL);
    free(task->address);
    free(task);
}

static void result_clicked(lv_event_t *event) {
    lv_msgbox_close_async(lv_event_get_current_target(event));
}
