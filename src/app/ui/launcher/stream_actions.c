#include "stream_actions.h"

#include <SDL.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app.h"
#include "logging.h"
#include "client.h"
#include "errors.h"
#include "backend/pcmanager.h"
#include "coverloader.h"
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
    /* Not a control: a tile pressed while another app runs (see SWITCHING). */
    ACTION_SWITCH,
    /* Resume and Terminate on the TV that started the app, which the host
     * offers neither: done as Aurora's own resume and quit (see THE MENU). */
    ACTION_OWN_RESUME,
    ACTION_OWN_QUIT,
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
/* And its own tile's id, which the copy names. Vibepollo tells a TV that did
 * not start the app that nothing runs, so this is the only way it knows. */
static int s_running_id;
static lv_obj_t *s_progress;
/* The menu's pictures, kept while it is open. */
static coverloader_t *s_menu_covers;
/* The button's own: what streams, its small picture, and that picture's
 * loader, kept with the button. What it shows now, so it is redrawn only
 * when that changes (the list reloads every few seconds). */
static lv_obj_t *s_btn_name;
static lv_obj_t *s_btn_picture;
static coverloader_t *s_btn_covers;
static int s_shown_picture_id = -1;
static char s_shown_name[TITLE_MAX];

static void button_clicked(lv_event_t *event);

static const char *app_name(const apploader_list_t *apps, int id);

static void button_deleted(lv_event_t *event);

static void free_user_data(lv_event_t *event);

static void menu_cancel(lv_event_t *event);

static void menu_clicked(lv_event_t *event);

static void menu_deleted(lv_event_t *event);

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
 * known by both, against the app it copies. Answers the copied app's own id,
 * or 0 when this is not a copy. */
static int running_copy_of(const apploader_list_t *apps, size_t index) {
    const APP_LIST *copy = &apps->items[index].base;
    if (copy->name == NULL || copy->name[0] != ' ') {
        return 0;
    }
    const char *name = visible_name(copy->name);
    for (size_t i = 0; i < apps->count; i++) {
        const APP_LIST *other = &apps->items[i].base;
        if (i == index || other->name == NULL || strcmp(visible_name(other->name), name) != 0) {
            continue;
        }
        if ((((unsigned) other->id ^ 0x40000000U) & 0x7fffffffU) == (unsigned) copy->id) {
            return other->id;
        }
    }
    return 0;
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

/* Vibepollo's own pictures for its two remote roles: the ids of their tiles
 * (remote_session.h's monitor_id and input_id), whose covers it sends. */
#define PICTURE_REMOTE_MONITOR 2147483505
#define PICTURE_REMOTE_INPUT 2147483506

static bool offered(action_kind_t kind) {
    for (int i = 0; i < s_count; i++) {
        if (s_actions[i].kind == kind) {
            return true;
        }
    }
    return false;
}

/* rhoquinn8217, 2026-10-20: "Lets replace the "Stream actions" with
 * "Streaming: <tile>". Tile could be ... Desktop tile, a Game/App tile ...
 * Remote input tile or remote monitor tile ... None". What this TV streams
 * from the host the launcher shows, as far as the host's list says:
 *   - Remote Monitor: this TV is one, and the list is only Resume and
 *     Disconnect Monitor;
 *   - Remote Input: this TV is one, and the list offers Remote Monitor but not
 *     Remote Input;
 *   - the running app: the host's word to the TV that started it, or the copy
 *     of it the host lists for every other TV;
 *   - None. ⓘ Including when another TV is a Remote Input or Monitor with no
 *     app running: the host tells no other TV of it. */
static void update_button(void) {
    if (s_btn == NULL || s_btn_name == NULL) {
        return;
    }
    const int reported = pcmanager_server_current_app(pcmanager, &s_host);
    const int running = reported != 0 ? reported : s_running_id;
    int picture = 0;
    char name[TITLE_MAX];
    if (offered(ACTION_DISCONNECT_MONITOR)) {
        picture = PICTURE_REMOTE_MONITOR;
        snprintf(name, sizeof(name), "%s", locstr("Remote Monitor"));
    } else if (s_count > 0 && offered(ACTION_MONITOR) && !offered(ACTION_INPUT)) {
        picture = PICTURE_REMOTE_INPUT;
        snprintf(name, sizeof(name), "%s", locstr("Remote Input"));
    } else if (running != 0) {
        picture = running;
        snprintf(name, sizeof(name), "%s", s_running[0] != '\0' ? s_running : locstr("An app"));
    } else {
        snprintf(name, sizeof(name), "%s", locstr("None"));
    }
    if (picture == s_shown_picture_id && strcmp(name, s_shown_name) == 0) {
        return;
    }
    s_shown_picture_id = picture;
    snprintf(s_shown_name, sizeof(s_shown_name), "%s", name);
    lv_label_set_text(s_btn_name, name);
    if (picture == 0 || s_app == NULL) {
        lv_obj_add_flag(s_btn_picture, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_clear_flag(s_btn_picture, LV_OBJ_FLAG_HIDDEN);
    if (s_btn_covers == NULL) {
        s_btn_covers = coverloader_new(s_app);
    }
    coverloader_display(s_btn_covers, &s_host, picture, s_btn_picture, lv_obj_get_style_width(s_btn_picture, 0),
                        lv_obj_get_style_height(s_btn_picture, 0));
}

void stream_actions_take(const uuidstr_t *host, apploader_list_t *apps) {
    if (host == NULL || apps == NULL) {
        return;
    }
    s_host = *host;
    s_count = 0;
    s_running[0] = '\0';
    s_running_id = 0;
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
        int twin = kinds[i] == ACTION_NONE ? running_copy_of(apps, i) : 0;
        if (twin != 0) {
            kinds[i] = ACTION_RUNNING_COPY;
            s_running_id = twin;
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
    /* On the TV that started the app there is no copy: the host names the
     * running app, and the list its name. */
    const int reported = pcmanager_server_current_app(pcmanager, host);
    if (reported != 0 && s_running[0] == '\0') {
        const char *name = app_name(apps, reported);
        if (name != NULL) {
            snprintf(s_running, sizeof(s_running), "%s", name);
        }
    }
    update_button();
}

bool stream_actions_list_shows_running(const apploader_list_t *apps) {
    if (apps == NULL) {
        return false;
    }
    for (size_t i = 0; i < apps->count; i++) {
        action_kind_t kind = kind_of_id(apps->items[i].base.id);
        if (kind == ACTION_RESUME || kind == ACTION_TERMINATE || running_copy_of(apps, i) != 0) {
            return true;
        }
    }
    return false;
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
    lv_label_set_text_static(label, locstr("Streaming:"));
    lv_obj_clear_flag(label, LV_OBJ_FLAG_CLICKABLE);

    /* The tile's own picture, small, in the tile's shape. ⚠️ No user data:
     * the cover loader reads an object with some as a game tile. */
    lv_obj_t *picture = lv_img_create(btn);
    lv_obj_clear_flag(picture, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(picture, LV_DPX(22), LV_DPX(30));
    lv_obj_set_style_radius(picture, LV_DPX(3), 0);
    lv_obj_set_style_clip_corner(picture, true, 0);
    lv_img_set_antialias(picture, true);
    lv_obj_add_flag(picture, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *name = lv_label_create(btn);
    lv_obj_set_style_text_font(name, lv_theme_get_font_small(btn), 0);
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
    lv_obj_set_style_max_width(name, LV_DPX(260), 0);
    lv_label_set_text_static(name, locstr("None"));
    lv_obj_clear_flag(name, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *chevron = lv_label_create(btn);
    lv_obj_set_style_text_font(chevron, lv_theme_moonlight_get_iconfont_small(btn), 0);
    lv_label_set_text_static(chevron, MAT_SYMBOL_ARROW_DROP_DOWN);
    lv_obj_clear_flag(chevron, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_add_event_cb(btn, button_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(btn, button_deleted, LV_EVENT_DELETE, NULL);
    s_btn = btn;
    s_btn_name = name;
    s_btn_picture = picture;
    s_shown_picture_id = -1;
    s_shown_name[0] = '\0';

    /* rhoquinn8217, 2026-10-20: "Left align the Server and Streaming: <tile>
     * buttons". The bar packs its children from the left and one spacer that
     * grows takes the free width, pushing all after it to the right; it was
     * made before the host button. Moved to the end now, after this button,
     * the icon buttons still made after it stay on the right. ⓘ A move, not a
     * swap: a swap announces the child as new and the bar's focus group would
     * put it last, out of the order seen on the screen. */
    const uint32_t children = lv_obj_get_child_cnt(topbar);
    for (uint32_t i = 0; i < children; i++) {
        lv_obj_t *child = lv_obj_get_child(topbar, (int32_t) i);
        if (lv_obj_get_style_flex_grow(child, LV_PART_MAIN) > 0) {
            lv_obj_move_to_index(child, -1);
            break;
        }
    }
    update_button();
}

static void button_deleted(lv_event_t *event) {
    if (lv_event_get_target(event) == s_btn) {
        s_btn = NULL;
        s_btn_name = NULL;
        s_btn_picture = NULL;
        if (s_btn_covers != NULL) {
            coverloader_unref(s_btn_covers);
            s_btn_covers = NULL;
        }
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

/* rhoquinn8217, 2026-10-20: "i want little icons next to each menu item at
 * their left. Use a shrunk version of the tile they replaced." The host sends
 * each control's tile picture as it sends a game's cover, so the entry shows
 * that, at about the row's height and the tile's shape. */
static void add_tile_picture(lv_obj_t *item, int picture_id, bool usable) {
    if (s_app == NULL) {
        return;
    }
    lv_coord_t height = LV_DPX(30);
    lv_coord_t width = height * 3 / 4;
    /* No user data: the cover loader reads an object with some as a game tile. */
    lv_obj_t *picture = lv_img_create(item);
    lv_obj_move_to_index(picture, 0);
    lv_obj_clear_flag(picture, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(picture, width, height);
    lv_obj_set_style_radius(picture, LV_DPX(3), 0);
    lv_obj_set_style_clip_corner(picture, true, 0);
    lv_img_set_antialias(picture, true);
    if (!usable) {
        lv_obj_set_style_img_opa(picture, LV_OPA_30, 0);
    }
    lv_obj_set_style_pad_ver(item, LV_DPX(6), 0);
    lv_obj_set_style_pad_column(item, LV_DPX(10), 0);
    if (s_menu_covers == NULL) {
        s_menu_covers = coverloader_new(s_app);
    }
    coverloader_display(s_menu_covers, &s_host, picture_id, picture, width, height);
}

/* rhoquinn8217, 2026-10-20, of the menu: "show all the buttons but gray them
 * out if they are not usable", and of Resume and Terminate on the TV that
 * started the game, which the host offers neither: "They should work like
 * aurora-tv's own resume and quit". Every entry the host can offer, in this
 * order, with its own tile's picture (the plain set's ids, whose covers the
 * host sends whatever it offers now). ⓘ Not Disconnect Input: the host never
 * offers it (remote_session.cpp's project). */
typedef struct menu_entry_t {
    action_kind_t kind;
    const char *title;
    int picture_id;
} menu_entry_t;

static const menu_entry_t k_entries[] = {
        {ACTION_RESUME, translatable("Resume"), 2147483501},
        {ACTION_TERMINATE, translatable("Terminate"), 2147483504},
        {ACTION_INPUT, translatable("Remote Input"), 2147483506},
        {ACTION_MONITOR, translatable("Remote Monitor"), 2147483505},
        {ACTION_DISCONNECT_MONITOR, translatable("Disconnect Monitor"), 2147483502},
};

/* What an entry does now: the host's own control when it offers one, Aurora's
 * own resume or quit on the TV that started the app, or nothing (greyed). */
static bool entry_action(const menu_entry_t *entry, stream_action_t *out) {
    for (int i = 0; i < s_count; i++) {
        if (s_actions[i].kind == entry->kind) {
            *out = s_actions[i];
            return true;
        }
    }
    const int reported = pcmanager_server_current_app(pcmanager, &s_host);
    if (reported == 0) {
        return false;
    }
    if (entry->kind == ACTION_RESUME) {
        out->kind = ACTION_OWN_RESUME;
        out->id = reported;
        snprintf(out->title, sizeof(out->title), "%s", s_running[0] != '\0' ? s_running : locstr("Resume"));
        return true;
    }
    if (entry->kind == ACTION_TERMINATE) {
        out->kind = ACTION_OWN_QUIT;
        out->id = reported;
        snprintf(out->title, sizeof(out->title), "%s", locstr("Terminate"));
        return true;
    }
    return false;
}

static void menu_deleted(lv_event_t *event) {
    (void) event;
    if (s_menu_covers != NULL) {
        coverloader_unref(s_menu_covers);
        s_menu_covers = NULL;
    }
}

/* The menu: a box of buttons just under the button, built as the tiles'
 * own long-press menu is, so the pad and the remote move through it the same
 * way and Back closes it. */
static void button_clicked(lv_event_t *event) {
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
    lv_obj_add_event_cb(msgbox, menu_deleted, LV_EVENT_DELETE, NULL);
    for (size_t i = 0; i < sizeof(k_entries) / sizeof(k_entries[0]); i++) {
        const menu_entry_t *entry = &k_entries[i];
        stream_action_t action;
        const bool usable = entry_action(entry, &action);
        /* The host's own title when it offers the control; the entry's when
         * Aurora does it or nothing can. */
        const char *title = usable && action.kind != ACTION_OWN_RESUME && action.kind != ACTION_OWN_QUIT
                            ? action.title : locstr(entry->title);
        lv_obj_t *item = lv_list_add_btn(content, NULL, title);
        lv_obj_add_flag(item, LV_OBJ_FLAG_EVENT_BUBBLE);
        if (usable) {
            lv_obj_set_user_data(item, action_copy(&action));
            lv_obj_add_event_cb(item, free_user_data, LV_EVENT_DELETE, NULL);
        } else {
            /* Greyed, and the selection steps over it (LVGL skips a disabled
             * object, lv_group.c). */
            lv_obj_add_state(item, LV_STATE_DISABLED);
            lv_obj_set_style_text_opa(item, LV_OPA_40, LV_STATE_DISABLED);
        }
        add_tile_picture(item, entry->picture_id, usable);
    }
    lv_obj_update_layout(msgbox);
    /* Under the button, from its left edge, now that it is on the left. */
    lv_obj_align_to(msgbox, btn, LV_ALIGN_OUT_BOTTOM_LEFT, 0, LV_DPX(6));
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
        case ACTION_OWN_QUIT:
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

/* ------------------------------------------------------------- SWITCHING -- */

/* rhoquinn8217, 2026-10-19: "There use to be a badge or a play button on app
 * that was in use. ... Sometime I see it sometimes I don't", and picking an app
 * while one ran did nothing they could see. Both had the same root: the host
 * tells only the TV that started an app which app runs. That TV's launcher
 * marks it and silently ignores a press on any other tile; every other TV is
 * told nothing runs, and the press goes to the host, which refuses it ("An app
 * is already running. Launch this app again within 60 seconds ..."). So the
 * badge now marks the running app on every TV (its id from the copy, above),
 * and a press on another tile asks before it closes what runs. */

int stream_actions_running_id(const uuidstr_t *host, int reported) {
    if (reported != 0 || host == NULL || !uuidstr_t_equals_t(host, &s_host)) {
        return reported;
    }
    return s_running_id;
}

/* rhoquinn8217, 2026-10-20, of that badge: "I don't like the large purple play
 * button. Can you make it smaller and closer to a corner", then that it should
 * "stand out enough that it's obvious that it's a label for the tile", and the
 * third of four pictures: 88/240 of the tile's width across, a quarter of that
 * in from the top and left edges, with a white ring and a shadow. */
/* ⭐ AND SEE-THROUGH (rhoquinn8217, 2026-10-20): "the white triangle see
 * through", then "draw white outline of the inner edges of the triangle. Make
 * the purple transparent", picking "purple a quarter" of three pictures. So
 * the badge is a picture drawn here at the tile's size, once per size: a white
 * ring, the purple at a quarter, and the triangle cut out of it with a white
 * line round it, so the tile shows through the triangle. Each pixel is the
 * average of 4 x 4 samples, for smooth edges. ⓘ Drawn, not a file: there is
 * no picture to keep in step with anything. */
#define BADGE_SAMPLES 4
#define BADGE_PURPLE_R 124
#define BADGE_PURPLE_G 77
#define BADGE_PURPLE_B 255
#define BADGE_PURPLE_A 64

static float badge_side(float ax, float ay, float bx, float by, float px, float py) {
    return (bx - ax) * (py - ay) - (by - ay) * (px - ax);
}

static float badge_segment_distance(float px, float py, float ax, float ay, float bx, float by) {
    const float vx = bx - ax, vy = by - ay;
    float t = ((px - ax) * vx + (py - ay) * vy) / (vx * vx + vy * vy);
    t = t < 0 ? 0 : (t > 1 ? 1 : t);
    const float dx = px - (ax + t * vx), dy = py - (ay + t * vy);
    return sqrtf(dx * dx + dy * dy);
}

static const lv_img_dsc_t *badge_picture(lv_coord_t size) {
    static lv_img_dsc_t *s_badges[4];
    static int s_badge_count;
    for (int i = 0; i < s_badge_count; i++) {
        if (s_badges[i]->header.w == size) {
            return s_badges[i];
        }
    }
    if (size < 8 || s_badge_count >= 4) {
        return s_badge_count > 0 ? s_badges[s_badge_count - 1] : NULL;
    }
    uint8_t *data = calloc((size_t) size * (size_t) size, 4);
    lv_img_dsc_t *dsc = calloc(1, sizeof(lv_img_dsc_t));
    if (data == NULL || dsc == NULL) {
        free(data);
        free(dsc);
        return NULL;
    }
    const float r = size / 2.0f, cx = r, cy = r;
    const float ring = size * 0.06f < 3 ? 3 : size * 0.06f;
    const float line = ring * 0.8f;
    /* The triangle, pointing right, a little right of the middle. */
    const float ax = cx - r * 0.30f, ay = cy - r * 0.42f;
    const float bx = cx + r * 0.46f, by = cy;
    const float qx = cx - r * 0.30f, qy = cy + r * 0.42f;
    const int n = BADGE_SAMPLES * BADGE_SAMPLES;
    for (int y = 0; y < size; y++) {
        for (int x = 0; x < size; x++) {
            float sum_r = 0, sum_g = 0, sum_b = 0, sum_a = 0;
            for (int j = 0; j < BADGE_SAMPLES; j++) {
                for (int i = 0; i < BADGE_SAMPLES; i++) {
                    const float px = x + (i + 0.5f) / BADGE_SAMPLES, py = y + (j + 0.5f) / BADGE_SAMPLES;
                    const float d = sqrtf((px - cx) * (px - cx) + (py - cy) * (py - cy));
                    float a = 0, cr = 255, cg = 255, cb = 255;
                    if (d > r) {
                        continue;
                    } else if (d > r - ring) {
                        a = 255;                                /* the ring */
                    } else {
                        const float edge = fminf(badge_segment_distance(px, py, ax, ay, bx, by),
                                                 fminf(badge_segment_distance(px, py, bx, by, qx, qy),
                                                       badge_segment_distance(px, py, qx, qy, ax, ay)));
                        const float s1 = badge_side(ax, ay, bx, by, px, py);
                        const float s2 = badge_side(bx, by, qx, qy, px, py);
                        const float s3 = badge_side(qx, qy, ax, ay, px, py);
                        const bool inside = (s1 >= 0 && s2 >= 0 && s3 >= 0) || (s1 <= 0 && s2 <= 0 && s3 <= 0);
                        if (edge <= line / 2) {
                            a = 255;                            /* the triangle's line */
                        } else if (!inside) {
                            a = BADGE_PURPLE_A;                 /* the purple, a quarter */
                            cr = BADGE_PURPLE_R;
                            cg = BADGE_PURPLE_G;
                            cb = BADGE_PURPLE_B;
                        }                                       /* inside: cut out */
                    }
                    sum_r += cr * a;
                    sum_g += cg * a;
                    sum_b += cb * a;
                    sum_a += a;
                }
            }
            uint8_t *p = data + ((size_t) y * (size_t) size + (size_t) x) * 4;
            if (sum_a > 0) {
                /* LVGL at 32 bits: blue, green, red, alpha. */
                p[0] = (uint8_t) (sum_b / sum_a + 0.5f);
                p[1] = (uint8_t) (sum_g / sum_a + 0.5f);
                p[2] = (uint8_t) (sum_r / sum_a + 0.5f);
                p[3] = (uint8_t) (sum_a / n + 0.5f);
            }
        }
    }
    dsc->header.cf = LV_IMG_CF_TRUE_COLOR_ALPHA;
    dsc->header.w = size;
    dsc->header.h = size;
    dsc->data_size = (uint32_t) size * (uint32_t) size * 4;
    dsc->data = data;
    s_badges[s_badge_count++] = dsc;
    return dsc;
}

void stream_actions_badge_style(lv_obj_t *badge, lv_coord_t tile_width) {
    lv_coord_t size = tile_width * 88 / 240;
    lv_obj_set_size(badge, size, size);
    lv_obj_align(badge, LV_ALIGN_TOP_LEFT, size / 4, size / 4);
    const lv_img_dsc_t *picture = badge_picture(size);
    if (picture == NULL) {
        return;     /* upstream's purple circle and its arrow stay */
    }
    /* Upstream's label, emptied: its arrow, its purple and its glow go, and
     * the picture sits in it, so the tile's own showing and hiding of the
     * label still shows and hides the badge. */
    lv_label_set_text(badge, "");
    lv_obj_set_style_bg_opa(badge, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(badge, 0, 0);
    lv_obj_set_style_shadow_width(badge, 0, 0);
    lv_obj_set_style_shadow_opa(badge, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(badge, 0, 0);
    lv_obj_t *img = lv_img_create(badge);
    lv_obj_clear_flag(img, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_img_set_src(img, picture);
    lv_obj_align(img, LV_ALIGN_TOP_LEFT, 0, 0);
}

static const char *app_name(const apploader_list_t *apps, int id) {
    const apploader_item_t *item = apps != NULL ? apploader_list_item_by_id(apps, id) : NULL;
    return item != NULL && item->base.name != NULL ? visible_name(item->base.name) : NULL;
}

static void switch_clicked(lv_event_t *event);

bool stream_actions_tile_pressed(const uuidstr_t *host, const apploader_list_t *apps, int app_id) {
    int running = stream_actions_running_id(host, pcmanager_server_current_app(pcmanager, host));
    if (running == 0 || app_id == running || s_app == NULL) {
        return false;       /* nothing runs, or the running app's own tile: as before */
    }
    const char *want = app_name(apps, app_id);
    const char *now = s_running[0] != '\0' ? s_running : app_name(apps, running);
    if (want == NULL) {
        return false;
    }
    s_host = *host;
    stream_action_t action = {.kind = ACTION_SWITCH, .id = app_id};
    snprintf(action.title, sizeof(action.title), "%s", want);
    static const char *btn_txts[] = {translatable("Cancel"), translatable("Start it"), ""};
    lv_obj_t *msgbox = lv_msgbox_create_i18n(NULL, locstr("Another app is running"), "text", btn_txts, false);
    const pclist_t *node = pcmanager_node(pcmanager, host);
    const char *hostname = node != NULL && node->server != NULL && node->server->hostname != NULL
                           ? node->server->hostname : locstr("the host");
    lv_label_set_text_fmt(lv_msgbox_get_text(msgbox),
                          locstr("%s is running on %s. Starting %s closes it, and everyone streaming it is "
                                 "kicked off, on every TV."), now != NULL ? now : locstr("An app"), hostname, want);
    lv_obj_set_user_data(msgbox, action_copy(&action));
    lv_obj_add_event_cb(msgbox, free_user_data, LV_EVENT_DELETE, NULL);
    lv_obj_add_event_cb(msgbox, switch_clicked, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_center(msgbox);
    return true;
}

/* Yes: the host's own confirmation, as it asks for it. The first launch, off
 * the screen thread, arms it (it answers "Launch this app again within 60
 * seconds"); the second is the streaming screen's own launch, which then
 * closes the running app and starts this one. */
static void switch_clicked(lv_event_t *event) {
    lv_obj_t *msgbox = lv_event_get_current_target(event);
    const stream_action_t *action = lv_obj_get_user_data(msgbox);
    if (lv_msgbox_get_active_btn(msgbox) == 1 && action != NULL) {
        ask_host(action);
    }
    lv_msgbox_close_async(msgbox);
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

static void own_quit(void);

static void confirm_clicked(lv_event_t *event) {
    lv_obj_t *msgbox = lv_event_get_current_target(event);
    const stream_action_t *action = lv_obj_get_user_data(msgbox);
    uint16_t pressed = lv_msgbox_get_active_btn(msgbox);
    if (pressed == 1 && action != NULL) {
        if (action->kind == ACTION_OWN_QUIT) {
            own_quit();
        } else {
            ask_host(action);
        }
    }
    lv_msgbox_close_async(msgbox);
}

/* Terminate on the TV that started the app: Aurora's own quit, as the running
 * tile's "Stop streaming" does it (apps.controller.c, launcher_quit_game): the
 * host is asked to end what runs, with "working" on the screen until it
 * answers. Only an answer that says it failed is shown. */
static void own_quit_done(int result, const char *error, const uuidstr_t *uuid, void *userdata) {
    (void) uuid;
    (void) userdata;
    if (s_progress != NULL) {
        lv_msgbox_close(s_progress);
        s_progress = NULL;
    }
    commons_log_info("StreamActions", "Terminate (Aurora's own quit): %d %s", result, error != NULL ? error : "");
    if (result == GS_OK) {
        return;
    }
    static const char *btn_txts[] = {translatable("OK"), ""};
    lv_obj_t *msgbox = lv_msgbox_create_i18n(NULL, locstr("Terminate"), "text", btn_txts, false);
    lv_label_set_text(lv_msgbox_get_text(msgbox), error != NULL && error[0] != '\0'
                                                  ? error : locstr("The host did not end the app."));
    lv_obj_add_event_cb(msgbox, result_clicked, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_center(msgbox);
}

static void own_quit(void) {
    if (s_progress == NULL) {
        s_progress = progress_dialog_create(locstr("Terminating..."));
    }
    if (!pcmanager_quitapp(pcmanager, &s_host, own_quit_done, NULL)) {
        /* Nothing runs now, as the host last said: there is nothing to end. */
        own_quit_done(GS_FAILED, locstr("Nothing is running on the host now."), &s_host, NULL);
    }
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
    if (task->action.kind == ACTION_SWITCH &&
        (task->ret == GS_OK || strstr(task->message, " again ") != NULL)) {
        /* Armed (or a host that does not ask): the real launch confirms it. */
        launch(&task->action);
        free(task->address);
        free(task);
        return;
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
