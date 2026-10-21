#include "stream_actions.h"

#include <SDL.h>
#include <ctype.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "app.h"
#include "logging.h"
#include "client.h"
#include "errors.h"
#include "apps.controller.h"
#include "backend/pcmanager.h"
#include "coverloader.h"
#include "util/path.h"
#include "ui/common/click_away.h"
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
/* This TV's Remote Monitor or Remote Input tile, put in the grid while it is
 * one (see THE ORDER), or 0. */
static int s_role_tile;

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
typedef enum shown_state_t {
    SHOWN_NONE,
    SHOWN_APP,
    SHOWN_INPUT,
    SHOWN_MONITOR,
} shown_state_t;

static shown_state_t streaming_now(int *picture, char *name, size_t len) {
    const int reported = pcmanager_server_current_app(pcmanager, &s_host);
    const int running = reported != 0 ? reported : s_running_id;
    *picture = 0;
    if (offered(ACTION_DISCONNECT_MONITOR)) {
        *picture = PICTURE_REMOTE_MONITOR;
        snprintf(name, len, "%s", locstr("Remote Monitor"));
        return SHOWN_MONITOR;
    }
    if (s_count > 0 && offered(ACTION_MONITOR) && !offered(ACTION_INPUT)) {
        *picture = PICTURE_REMOTE_INPUT;
        snprintf(name, len, "%s", locstr("Remote Input"));
        return SHOWN_INPUT;
    }
    if (running != 0) {
        *picture = running;
        snprintf(name, len, "%s", s_running[0] != '\0' ? s_running : locstr("An app"));
        return SHOWN_APP;
    }
    snprintf(name, len, "%s", locstr("None"));
    return SHOWN_NONE;
}

static void update_button(void) {
    if (s_btn == NULL || s_btn_name == NULL) {
        return;
    }
    int picture = 0;
    char name[TITLE_MAX];
    streaming_now(&picture, name, sizeof(name));
    if (picture == s_shown_picture_id && strcmp(name, s_shown_name) == 0) {
        return;
    }
    s_shown_picture_id = picture;
    snprintf(s_shown_name, sizeof(s_shown_name), "%s", name);
    lv_label_set_text(s_btn_name, name);
    /* rhoquinn8217, 2026-10-20: "change it to just "Streaming: <tile>"". The
     * tile's picture alone, its name being in the menu's status line; the word
     * only when there is no tile ("Streaming: None"). */
    if (picture == 0 || s_app == NULL) {
        lv_obj_add_flag(s_btn_picture, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_btn_name, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_add_flag(s_btn_name, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_btn_picture, LV_OBJ_FLAG_HIDDEN);
    if (s_btn_covers == NULL) {
        s_btn_covers = coverloader_new(s_app);
    }
    coverloader_display(s_btn_covers, &s_host, picture, s_btn_picture, lv_obj_get_style_width(s_btn_picture, 0),
                        lv_obj_get_style_height(s_btn_picture, 0));
}

/* -------------------------------------------------------------- THE ORDER -- */

/* rhoquinn8217, 2026-10-20, of the grid: "Desktop tile is always first except
 * when something is streaming", "When a game app is streaming, it get's move
 * to the front", "When Remote Input or Remote Monitor is active, that tile
 * shows up with the purple play icon and is at placed at the front. It gets
 * removed/hidden when they are stopped"; and of the launchers, "Since
 * favorites are pushed to the front, lets add logic that automatically sets
 * these to starred ... Desktop should always be in front of starred". So, in
 * order: this TV's Remote Monitor or Remote Input tile while it is one, the
 * running app, Desktop, the starred, the rest, the hidden; each group in name
 * order, as upstream's list is.
 * ⓘ Vibepollo forgets a Remote Input as its stream ends, so that tile is
 * seldom seen; it keeps a Remote Monitor for Resume until it is disconnected
 * (its "Remove Remote Monitor when its stream ends" is off by default). It
 * lists no apps at all to a Remote Monitor, so there the tile is alone. */

/* The launchers starred by themselves, once each: a person who unstars one
 * keeps it unstarred. "uuid id" lines, as server-cards.txt. ⓘ By id: Vibepollo
 * gives an app a new id when its picture changes, and a star is kept by id. */
#define STARRED_FILE "auto-starred.txt"
#define STARRED_MAX 64

typedef struct auto_starred_t {
    uuidstr_t host;
    int app_id;
} auto_starred_t;

static auto_starred_t s_starred[STARRED_MAX];
static int s_starred_count = -1;        /* -1: the file is not read yet */

static void starred_read(void) {
    s_starred_count = 0;
    char *path = path_join(app_configuration->conf_dir, STARRED_FILE);
    FILE *file = fopen(path, "r");
    free(path);
    if (file == NULL) {
        return;
    }
    char line[128];
    while (s_starred_count < STARRED_MAX && fgets(line, sizeof(line), file) != NULL) {
        char uuid[UUIDSTR_CAPACITY];
        int id;
        if (sscanf(line, "%36s %d", uuid, &id) == 2 && strlen(uuid) == UUIDSTR_LENGTH && id != 0) {
            uuidstr_fromstr(&s_starred[s_starred_count].host, uuid);
            s_starred[s_starred_count].app_id = id;
            s_starred_count++;
        }
    }
    fclose(file);
}

static bool starred_before(const uuidstr_t *host, int id) {
    if (s_starred_count < 0) {
        starred_read();
    }
    for (int i = 0; i < s_starred_count; i++) {
        if (s_starred[i].app_id == id && uuidstr_t_equals_t(&s_starred[i].host, host)) {
            return true;
        }
    }
    return false;
}

static void starred_note(const uuidstr_t *host, int id) {
    if (s_starred_count < STARRED_MAX) {
        s_starred[s_starred_count].host = *host;
        s_starred[s_starred_count].app_id = id;
        s_starred_count++;
    }
    char *path = path_join(app_configuration->conf_dir, STARRED_FILE);
    FILE *file = fopen(path, "a");
    if (file == NULL) {
        commons_log_warn("StreamActions", "Cannot write %s", path);
    } else {
        fprintf(file, "%.36s %d\n", host->data, id);
        fclose(file);
    }
    free(path);
}

/* A name that is the launcher's, whole or as its first words ("Steam Big
 * Picture", "Playnite Fullscreen"). The short ones only whole, so a game named
 * "EA Sports ..." is not starred. */
static bool name_starts_with(const char *name, const char *start) {
    const size_t n = strlen(start);
    return strncasecmp(name, start, n) == 0 && (name[n] == '\0' || !isalnum((unsigned char) name[n]));
}

static bool is_launcher(const char *name) {
    static const char *const by_start[] = {
            "Steam", "Epic Games", "Playnite", "GOG Galaxy", "Ubisoft Connect", "Battle.net", "Amazon Games",
            "Heroic", "Rockstar Games Launcher", "LaunchBox", "RetroArch", "EmulationStation", "Pegasus",
    };
    static const char *const whole[] = {
            "GOG", "EA", "EA app", "EA Desktop", "Origin", "Uplay", "Xbox", "Xbox app", "itch", "itch.io",
            "Big Box", "ES-DE",
    };
    if (name == NULL) {
        return false;
    }
    for (size_t i = 0; i < sizeof(by_start) / sizeof(by_start[0]); i++) {
        if (name_starts_with(name, by_start[i])) {
            return true;
        }
    }
    for (size_t i = 0; i < sizeof(whole) / sizeof(whole[0]); i++) {
        if (strcasecmp(name, whole[i]) == 0) {
            return true;
        }
    }
    return false;
}

static void star_launchers(const uuidstr_t *host, apploader_list_t *apps) {
    for (size_t i = 0; i < apps->count; i++) {
        apploader_item_t *item = &apps->items[i];
        if (item->fav || item->hidden || !is_launcher(item->base.name) || starred_before(host, item->base.id)) {
            continue;
        }
        pcmanager_favorite_app(pcmanager, host, item->base.id, true);
        item->fav = true;
        starred_note(host, item->base.id);
        commons_log_info("StreamActions", "%s is a launcher: starred, once", item->base.name);
    }
}

/* This TV's role, as a tile of the grid: the host lists its controls but not
 * the role itself. Made in the room the controls left. */
static void add_role_tile(apploader_list_t *apps, size_t room) {
    s_role_tile = 0;
    int id = 0;
    const char *name = NULL;
    if (offered(ACTION_DISCONNECT_MONITOR)) {
        id = PICTURE_REMOTE_MONITOR;
        name = locstr("Remote Monitor");
    } else if (offered(ACTION_MONITOR) && !offered(ACTION_INPUT)) {
        id = PICTURE_REMOTE_INPUT;
        name = locstr("Remote Input");
    }
    if (id == 0 || apps->count >= room) {
        return;
    }
    apploader_item_t *item = &apps->items[apps->count];
    memset(item, 0, sizeof(*item));
    item->base.id = id;
    item->base.name = strdup(name);
    if (item->base.name == NULL) {
        return;
    }
    apps->count++;
    s_role_tile = id;
}

static int s_order_running;

static int order_rank(const apploader_item_t *item) {
    if (item->hidden) {
        return 5;
    }
    if (s_role_tile != 0 && item->base.id == s_role_tile) {
        return 0;
    }
    if (s_order_running != 0 && item->base.id == s_order_running) {
        return 1;
    }
    if (item->base.name != NULL && strcasecmp(item->base.name, "Desktop") == 0) {
        return 2;
    }
    return item->fav ? 3 : 4;
}

static int order_compare(const void *a, const void *b) {
    const apploader_item_t *x = a, *y = b;
    const int rx = order_rank(x), ry = order_rank(y);
    if (rx != ry) {
        return rx - ry;
    }
    const int by_name = strcoll(x->base.name != NULL ? x->base.name : "", y->base.name != NULL ? y->base.name : "");
    if (by_name != 0) {
        return by_name;
    }
    return x->base.id < y->base.id ? -1 : x->base.id > y->base.id;
}

void stream_actions_take(const uuidstr_t *host, apploader_list_t *apps) {
    if (host == NULL || apps == NULL) {
        return;
    }
    s_host = *host;
    s_count = 0;
    s_running[0] = '\0';
    s_running_id = 0;
    s_role_tile = 0;
    if (apps->count == 0) {
        update_button();
        return;
    }
    const size_t room = apps->count;
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
    add_role_tile(apps, room);
    star_launchers(host, apps);
    s_order_running = reported != 0 ? reported : s_running_id;
    qsort(apps->items, apps->count, sizeof(apploader_item_t), order_compare);
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
    /* rhoquinn8217, 2026-10-20: "change the AURORA label in the dashboard
     * with AURORA-RHQN". Upstream's title, made before this button in the
     * same bar, renamed here rather than in upstream's own line. */
    for (uint32_t i = 0; i < lv_obj_get_child_cnt(topbar); i++) {
        lv_obj_t *child = lv_obj_get_child(topbar, (int32_t) i);
        if (lv_obj_check_type(child, &lv_label_class) && strcmp(lv_label_get_text(child), "AURORA") == 0) {
            lv_label_set_text_static(child, "AURORA-RHQN");
            break;
        }
    }
    /* rhoquinn8217, 2026-10-20: "the Profile halo appears to clip on the top
     * edge of the screen". Upstream's profile box takes its height from its
     * text, 48 DPX against the bar's buttons' 40, and LVGL's focus ring is
     * drawn 6 DPX outside it, which reaches the screen's top edge. Made the
     * buttons' height here, its text still in the middle, so the ring stays on
     * the screen. */
    for (uint32_t i = 0; i < lv_obj_get_child_cnt(topbar); i++) {
        lv_obj_t *child = lv_obj_get_child(topbar, (int32_t) i);
        if (!lv_obj_check_type(child, &lv_dropdown_class)) {
            continue;
        }
        const lv_coord_t height = LV_DPX(TOPBAR_BTN_SIZE);
        const lv_font_t *font = lv_obj_get_style_text_font(child, LV_PART_MAIN);
        const lv_coord_t border = lv_obj_get_style_border_width(child, LV_PART_MAIN);
        const lv_coord_t pad = (height - lv_font_get_line_height(font) - 2 * border) / 2;
        lv_obj_set_style_pad_ver(child, pad > 0 ? pad : 0, 0);
        lv_obj_set_height(child, height);
        break;
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

/* A tile's picture, as the host sends it for that tile, as a game's cover is.
 * ⚠️ No user data: the cover loader reads an object with some as a game tile. */
static lv_obj_t *add_picture(lv_obj_t *parent, int picture_id, lv_coord_t width, lv_coord_t height) {
    lv_obj_t *picture = lv_img_create(parent);
    lv_obj_clear_flag(picture, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(picture, width, height);
    lv_obj_set_style_radius(picture, LV_DPX(4), 0);
    lv_obj_set_style_clip_corner(picture, true, 0);
    lv_img_set_antialias(picture, true);
    if (s_app != NULL) {
        if (s_menu_covers == NULL) {
            s_menu_covers = coverloader_new(s_app);
        }
        coverloader_display(s_menu_covers, &s_host, picture_id, picture, width, height);
    }
    return picture;
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

/* rhoquinn8217, 2026-10-20: "i want little icons next to each menu item at
 * their left. Use a shrunk version of the tile they replaced." The host sends
 * each control's tile picture as it sends a game's cover, so the entry shows
 * that, at about the row's height and the tile's shape. Greyed when it cannot
 * be used, and the selection steps over it (LVGL skips a disabled object,
 * lv_group.c). */
static void add_entry(lv_obj_t *content, const menu_entry_t *entry) {
    stream_action_t action;
    const bool usable = entry_action(entry, &action);
    /* The host's own title when it offers the control; the entry's when
     * Aurora does it or nothing can. */
    const char *title = usable && action.kind != ACTION_OWN_RESUME && action.kind != ACTION_OWN_QUIT
                        ? action.title : locstr(entry->title);
    lv_obj_t *item = lv_list_add_btn(content, NULL, title);
    lv_obj_add_flag(item, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_set_style_pad_ver(item, LV_DPX(6), 0);
    lv_obj_set_style_pad_column(item, LV_DPX(10), 0);
    lv_obj_t *picture = add_picture(item, entry->picture_id, LV_DPX(30) * 3 / 4, LV_DPX(30));
    lv_obj_move_to_index(picture, 0);
    if (usable) {
        lv_obj_set_user_data(item, action_copy(&action));
        lv_obj_add_event_cb(item, free_user_data, LV_EVENT_DELETE, NULL);
    } else {
        lv_obj_add_state(item, LV_STATE_DISABLED);
        lv_obj_set_style_text_opa(item, LV_OPA_40, LV_STATE_DISABLED);
        lv_obj_set_style_img_opa(picture, LV_OPA_30, 0);
    }
}

/* rhoquinn8217, 2026-10-20: "above the first menu item, write out the full
 * status": "Currently streaming <tile> <game/app name>", "Currently acting as
 * remote input", "Currently streaming <remote monitor> (View only)", "Nothing
 * streaming"; and of the menu under it, "I still want small drop down menu".
 * The same reading as the button's (streaming_now).
 * ⓘ Then "drop "(View only)"" (2026-10-20): a Remote Monitor is not view
 * only. Vibepollo sends it "picture and input" (its "Mute audio on Remote
 * Monitor" setting's own words) as a private monitor of the PC's, so the line
 * names it as it names an app. */
static lv_obj_t *add_status(lv_obj_t *content) {
    int picture = 0;
    char name[TITLE_MAX];
    const shown_state_t state = streaming_now(&picture, name, sizeof(name));
    lv_obj_t *line = lv_obj_create(content);
    lv_obj_remove_style_all(line);
    lv_obj_set_size(line, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(line, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(line, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(line, LV_DPX(8), 0);
    lv_obj_set_style_pad_bottom(line, LV_DPX(6), 0);
    lv_obj_clear_flag(line, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *text = lv_label_create(line);
    switch (state) {
        case SHOWN_APP:
        case SHOWN_MONITOR:
            lv_label_set_text_static(text, locstr("Currently streaming"));
            break;
        case SHOWN_INPUT:
            lv_label_set_text_static(text, locstr("Currently acting as remote input"));
            break;
        default:
            lv_label_set_text_static(text, locstr("Nothing streaming"));
            break;
    }
    if (state == SHOWN_APP || state == SHOWN_MONITOR) {
        add_picture(line, picture, LV_DPX(18), LV_DPX(24));
        lv_obj_t *after = lv_label_create(line);
        lv_label_set_long_mode(after, LV_LABEL_LONG_DOT);
        lv_obj_set_style_max_width(after, LV_DPX(320), 0);
        lv_label_set_text(after, name);
    }
    return line;
}

/* The menu: the status, then the entries, in a box just under the button,
 * built as the tiles' own long-press menu is, so the pad and the remote move
 * through it the same way and Back closes it. */
static void button_clicked(lv_event_t *event) {
    lv_obj_t *btn = lv_event_get_target(event);
    lv_obj_t *msgbox = lv_msgbox_create(NULL, NULL, NULL, NULL, false);
    click_away_closes(msgbox);
    lv_obj_t *backdrop = lv_obj_get_parent(msgbox);
    if (backdrop != NULL) {
        lv_obj_set_style_bg_opa(backdrop, LV_OPA_30, 0);
    }
    lv_obj_set_style_min_width(msgbox, 0, 0);
    lv_obj_set_width(msgbox, LV_DPX(320));
    lv_obj_t *content = lv_msgbox_get_content(msgbox);
    lv_obj_add_flag(content, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_event_cb(msgbox, menu_cancel, LV_EVENT_CANCEL, NULL);
    lv_obj_add_event_cb(content, menu_clicked, LV_EVENT_SHORT_CLICKED, NULL);
    lv_obj_add_event_cb(msgbox, menu_deleted, LV_EVENT_DELETE, NULL);
    lv_obj_t *status = add_status(content);
    for (size_t i = 0; i < sizeof(k_entries) / sizeof(k_entries[0]); i++) {
        add_entry(content, &k_entries[i]);
    }
    lv_obj_update_layout(msgbox);
    /* Wider when the status needs it, as far as the theme's widest box. */
    const lv_coord_t around = lv_obj_get_width(msgbox) - lv_obj_get_content_width(content);
    const lv_coord_t wanted = lv_obj_get_width(status) + around;
    if (wanted > lv_obj_get_width(msgbox)) {
        lv_obj_set_width(msgbox, wanted);
        lv_obj_update_layout(msgbox);
    }
    /* Under the button, from its left edge, now that it is on the left. */
    lv_obj_align_to(msgbox, btn, LV_ALIGN_OUT_BOTTOM_LEFT, 0, LV_DPX(6));
}

/* Back, from any entry: the box goes. */
static void menu_cancel(lv_event_t *event) {
    lv_msgbox_close_async(lv_event_get_current_target(event));
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
    lv_obj_t *content = lv_event_get_current_target(event);
    if (lv_obj_get_parent(target) != content) {
        return;
    }
    lv_obj_t *msgbox = lv_obj_get_parent(content);
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

bool stream_actions_tile_badged(const uuidstr_t *host, int running_id, int app_id) {
    if (app_id != 0 && app_id == running_id) {
        return true;
    }
    return s_role_tile != 0 && app_id == s_role_tile && host != NULL && uuidstr_t_equals_t(host, &s_host);
}

/* rhoquinn8217, 2026-10-20, of that badge: "I don't like the large purple play
 * button. Can you make it smaller and closer to a corner", then that it should
 * "stand out enough that it's obvious that it's a label for the tile", and the
 * third of four pictures: 88/240 of the tile's width across, a quarter of that
 * in from the top and left edges, with a white ring and a shadow. ⓘ The size
 * and the distance from the corner stayed; the corner moved to the bottom
 * (PLAY AND STOP, below). */
/* ⭐ AND SEE-THROUGH (rhoquinn8217, 2026-10-20): "the white triangle see
 * through", then "draw white outline of the inner edges of the triangle. Make
 * the purple transparent", picking "purple a quarter" of three pictures. So
 * the badge is a picture drawn here at the tile's size, once per size: a white
 * ring, the purple at a quarter (a half since, BADGE_PURPLE_A), and the
 * triangle cut out of it with a white line round it, so the tile shows
 * through the triangle. Each pixel is the
 * average of 4 x 4 samples, for smooth edges. ⓘ Drawn, not a file: there is
 * no picture to keep in step with anything. */
#define BADGE_SAMPLES 4
#define BADGE_PURPLE_R 124
#define BADGE_PURPLE_G 77
#define BADGE_PURPLE_B 255
/* A half since rhoquinn8217, 2026-10-20, of the quarter: "The purple play and
 * stop button need to be less transparent". */
#define BADGE_PURPLE_A 128
/* Under the pointer: the purple stronger and the badge a little larger. */
#define BADGE_HOT_A 224
#define BADGE_HOT_ZOOM (LV_IMG_ZOOM_NONE * 115 / 100)
#define BADGE_CACHE 16

/* rhoquinn8217, 2026-10-20: "a (stop) icon mirroring the purple play button
 * ... with same shape and color except its has a square cut out instead of a
 * triangle", then "vertically mirror the stop button instead. Keep them the
 * same size". */
typedef enum badge_symbol_t {
    BADGE_PLAY = 1,
    BADGE_STOP,
} badge_symbol_t;

static float badge_side(float ax, float ay, float bx, float by, float px, float py) {
    return (bx - ax) * (py - ay) - (by - ay) * (px - ax);
}

/* How far a point is from the edge of the square of half-side `half`. */
static float badge_square_distance(float px, float py, float cx, float cy, float half) {
    const float dx = fabsf(px - cx) - half, dy = fabsf(py - cy) - half;
    const float ox = dx > 0 ? dx : 0, oy = dy > 0 ? dy : 0;
    if (ox > 0 || oy > 0) {
        return sqrtf(ox * ox + oy * oy);
    }
    return -(dx > dy ? dx : dy);
}

static float badge_segment_distance(float px, float py, float ax, float ay, float bx, float by) {
    const float vx = bx - ax, vy = by - ay;
    float t = ((px - ax) * vx + (py - ay) * vy) / (vx * vx + vy * vy);
    t = t < 0 ? 0 : (t > 1 ? 1 : t);
    const float dx = px - (ax + t * vx), dy = py - (ay + t * vy);
    return sqrtf(dx * dx + dy * dy);
}

typedef struct badge_cached_t {
    lv_coord_t size;
    badge_symbol_t symbol;
    bool hot;
    lv_img_dsc_t *dsc;
} badge_cached_t;

static const lv_img_dsc_t *badge_picture(lv_coord_t size, badge_symbol_t symbol, bool hot) {
    static badge_cached_t s_badges[BADGE_CACHE];
    static int s_badge_count;
    for (int i = 0; i < s_badge_count; i++) {
        if (s_badges[i].size == size && s_badges[i].symbol == symbol && s_badges[i].hot == hot) {
            return s_badges[i].dsc;
        }
    }
    if (size < 8 || s_badge_count >= BADGE_CACHE) {
        return NULL;
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
    const float purple_a = hot ? BADGE_HOT_A : BADGE_PURPLE_A;
    /* The triangle, pointing right, a little right of the middle; the square,
     * in the middle. */
    const float ax = cx - r * 0.30f, ay = cy - r * 0.42f;
    const float bx = cx + r * 0.46f, by = cy;
    const float qx = cx - r * 0.30f, qy = cy + r * 0.42f;
    const float half = r * 0.34f;
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
                        float edge;
                        bool inside;
                        if (symbol == BADGE_STOP) {
                            edge = badge_square_distance(px, py, cx, cy, half);
                            inside = fabsf(px - cx) <= half && fabsf(py - cy) <= half;
                        } else {
                            edge = fminf(badge_segment_distance(px, py, ax, ay, bx, by),
                                         fminf(badge_segment_distance(px, py, bx, by, qx, qy),
                                               badge_segment_distance(px, py, qx, qy, ax, ay)));
                            const float s1 = badge_side(ax, ay, bx, by, px, py);
                            const float s2 = badge_side(bx, by, qx, qy, px, py);
                            const float s3 = badge_side(qx, qy, ax, ay, px, py);
                            inside = (s1 >= 0 && s2 >= 0 && s3 >= 0) || (s1 <= 0 && s2 <= 0 && s3 <= 0);
                        }
                        if (edge <= line / 2) {
                            a = 255;                            /* the symbol's line */
                        } else if (!inside) {
                            a = purple_a;                       /* the purple, part see-through */
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
    s_badges[s_badge_count++] = (badge_cached_t) {.size = size, .symbol = symbol, .hot = hot, .dsc = dsc};
    return dsc;
}

/* ⭐ PLAY AND STOP, ACROSS THE TILE'S MIDDLE (rhoquinn8217, 2026-10-20): "Both
 * the purple play and stop icon are clickable, triangle resumes and square
 * stops", "When hovering over the purple play or stop icon cause the button
 * to highlight", and of where: "Lets move the play and stop button both to
 * the bottom", then "vertically center the play and stop buttons": Play on
 * the left and Stop on the right, each a quarter of its size in from its
 * side, halfway down the tile. So upstream's label becomes a row across the
 * tile holding the two, and the tile's own showing and hiding of the label
 * shows and hides both. A press on the row between them is the tile's: only
 * the two are clickable. */
#define BADGE_FLAG LV_OBJ_FLAG_USER_3
#define HOVER_IDLE_MS 4000
#define HOVER_POINTERS 4

static void badge_clicked(lv_event_t *event);

static void badge_deleted(lv_event_t *event);

static void hover_tick(lv_timer_t *timer);

static void add_badge(lv_obj_t *row, lv_coord_t size, badge_symbol_t symbol, lv_align_t align) {
    lv_obj_t *img = lv_img_create(row);
    lv_obj_clear_flag(img, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(img, LV_OBJ_FLAG_CLICKABLE | BADGE_FLAG);
    lv_obj_set_user_data(img, (void *) (intptr_t) symbol);
    lv_img_set_src(img, badge_picture(size, symbol, false));
    lv_img_set_antialias(img, true);
    lv_obj_align(img, align, 0, 0);
    lv_obj_add_event_cb(img, badge_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(img, badge_deleted, LV_EVENT_DELETE, NULL);
}

void stream_actions_badge_style(lv_obj_t *badge, lv_coord_t tile_width) {
    const lv_coord_t size = tile_width * 88 / 240;
    const lv_coord_t gap = size / 4;
    if (badge_picture(size, BADGE_PLAY, false) == NULL || badge_picture(size, BADGE_STOP, false) == NULL) {
        lv_obj_set_size(badge, size, size);
        lv_obj_align(badge, LV_ALIGN_TOP_LEFT, gap, gap);
        return;     /* upstream's purple circle and its arrow stay */
    }
    lv_obj_set_size(badge, tile_width - 2 * gap, size);
    lv_obj_align(badge, LV_ALIGN_LEFT_MID, gap, 0);
    /* Upstream's label, emptied: its arrow, its purple and its glow go. */
    lv_label_set_text(badge, "");
    lv_obj_set_style_bg_opa(badge, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(badge, 0, 0);
    lv_obj_set_style_shadow_width(badge, 0, 0);
    lv_obj_set_style_shadow_opa(badge, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(badge, 0, 0);
    /* A badge under the pointer grows past the row. */
    lv_obj_add_flag(badge, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    add_badge(badge, size, BADGE_PLAY, LV_ALIGN_LEFT_MID);
    add_badge(badge, size, BADGE_STOP, LV_ALIGN_RIGHT_MID);
    static lv_timer_t *s_hover;
    if (s_hover == NULL) {
        s_hover = lv_timer_create(hover_tick, 50, NULL);
    }
}

/* What Play does: back into what runs, as the menu's Resume does (Aurora's own
 * resume on the TV that started it, the host's Resume on another). On this
 * TV's Remote Monitor tile, the host's Resume, which returns to the monitor;
 * on its Remote Input tile, Remote Input again. */
static const menu_entry_t *entry_of(action_kind_t kind) {
    for (size_t i = 0; i < sizeof(k_entries) / sizeof(k_entries[0]); i++) {
        if (k_entries[i].kind == kind) {
            return &k_entries[i];
        }
    }
    return NULL;
}

static void tile_resume(int app_id) {
    stream_action_t action = {0};
    if (s_role_tile != 0 && app_id == s_role_tile && app_id == PICTURE_REMOTE_INPUT) {
        action.kind = ACTION_INPUT;
        action.id = app_id;
        snprintf(action.title, sizeof(action.title), "%s", locstr("Remote Input"));
        launch(&action);
        return;
    }
    const menu_entry_t *resume = entry_of(ACTION_RESUME);
    if (resume != NULL && entry_action(resume, &action)) {
        launch(&action);
    } else {
        commons_log_info("StreamActions", "Play on app %d: nothing to resume", app_id);
    }
}

/* What Stop does: on the running app, the menu's Terminate, asking first; on
 * this TV's Remote Monitor or Remote Input tile, its disconnect, at once, since
 * it ends only this TV's part. ⓘ Vibepollo never lists Disconnect Input but
 * takes it from a TV that is a Remote Input (remote_session.cpp, dispatch). */
static void tile_stop(int app_id) {
    stream_action_t action = {0};
    if (s_role_tile != 0 && app_id == s_role_tile) {
        if (app_id == PICTURE_REMOTE_MONITOR) {
            const menu_entry_t *disconnect = entry_of(ACTION_DISCONNECT_MONITOR);
            if (disconnect == NULL || !entry_action(disconnect, &action)) {
                return;
            }
        } else {
            action.kind = ACTION_DISCONNECT_INPUT;
            action.id = 2147483503;
            snprintf(action.title, sizeof(action.title), "%s", locstr("Disconnect Input"));
        }
        ask_host(&action);
        return;
    }
    const menu_entry_t *terminate = entry_of(ACTION_TERMINATE);
    if (terminate != NULL && entry_action(terminate, &action)) {
        confirm_terminate(&action);
    } else {
        commons_log_info("StreamActions", "Stop on app %d: nothing to stop", app_id);
    }
}

static void badge_clicked(lv_event_t *event) {
    lv_obj_t *img = lv_event_get_target(event);
    lv_obj_t *row = lv_obj_get_parent(img);
    lv_obj_t *tile = row != NULL ? lv_obj_get_parent(row) : NULL;
    const appitem_viewholder_t *holder = tile != NULL ? lv_obj_get_user_data(tile) : NULL;
    if (holder == NULL || holder->app_id == 0) {
        return;
    }
    if ((badge_symbol_t) (intptr_t) lv_obj_get_user_data(img) == BADGE_PLAY) {
        tile_resume(holder->app_id);
    } else {
        tile_stop(holder->app_id);
    }
}

/* ⭐ THE HIGHLIGHT. LVGL 8 does not mark what a pointer is over (its hovered
 * state is "not supported"), so every 50 ms each pointer that moved in the
 * last 4 seconds (the remote's, a mouse, a pad's touchpad: input/
 * touchpad_pointer.h) is asked what is under it; a badge there is drawn
 * stronger and larger until the pointer leaves it. Not through a message box:
 * the box's backdrop is what is under the pointer then. */
typedef struct pointer_seen_t {
    lv_indev_t *indev;
    lv_point_t point;
    uint32_t moved;
} pointer_seen_t;

static lv_obj_t *s_hovered;

static void badge_set_hot(lv_obj_t *img, bool hot) {
    const lv_img_dsc_t *now = lv_img_get_src(img);
    if (now == NULL) {
        return;
    }
    const badge_symbol_t symbol = (badge_symbol_t) (intptr_t) lv_obj_get_user_data(img);
    const lv_img_dsc_t *picture = badge_picture((lv_coord_t) now->header.w, symbol, hot);
    if (picture != NULL) {
        lv_img_set_src(img, picture);
    }
    lv_img_set_zoom(img, hot ? BADGE_HOT_ZOOM : LV_IMG_ZOOM_NONE);
}

static void badge_deleted(lv_event_t *event) {
    if (lv_event_get_target(event) == s_hovered) {
        s_hovered = NULL;
    }
}

static void hover_tick(lv_timer_t *timer) {
    (void) timer;
    static pointer_seen_t s_seen[HOVER_POINTERS];
    lv_obj_t *found = NULL;
    lv_indev_t *indev = NULL;
    int slot = 0;
    while ((indev = lv_indev_get_next(indev)) != NULL && slot < HOVER_POINTERS) {
        if (lv_indev_get_type(indev) != LV_INDEV_TYPE_POINTER) {
            continue;
        }
        pointer_seen_t *seen = &s_seen[slot++];
        lv_point_t point;
        lv_indev_get_point(indev, &point);
        if (seen->indev != indev || seen->point.x != point.x || seen->point.y != point.y) {
            seen->indev = indev;
            seen->point = point;
            seen->moved = lv_tick_get();
        }
        if (found != NULL || lv_tick_elaps(seen->moved) > HOVER_IDLE_MS) {
            continue;
        }
        lv_obj_t *under = lv_indev_search_obj(lv_layer_top(), &point);
        if (under == NULL) {
            under = lv_indev_search_obj(lv_scr_act(), &point);
        }
        if (under != NULL && lv_obj_has_flag(under, BADGE_FLAG)) {
            found = under;
        }
    }
    if (found == s_hovered) {
        return;
    }
    if (s_hovered != NULL) {
        badge_set_hot(s_hovered, false);
    }
    s_hovered = found;
    if (found != NULL) {
        badge_set_hot(found, true);
    }
}

static const char *app_name(const apploader_list_t *apps, int id) {
    const apploader_item_t *item = apps != NULL ? apploader_list_item_by_id(apps, id) : NULL;
    return item != NULL && item->base.name != NULL ? visible_name(item->base.name) : NULL;
}

static void switch_clicked(lv_event_t *event);

bool stream_actions_tile_pressed(const uuidstr_t *host, const apploader_list_t *apps, int app_id) {
    /* This TV's Remote Monitor or Remote Input tile: back into it, as its Play
     * does. ⓘ Not as another app: it closes nothing. */
    if (s_role_tile != 0 && app_id == s_role_tile && host != NULL && uuidstr_t_equals_t(host, &s_host)) {
        tile_resume(app_id);
        return true;
    }
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
