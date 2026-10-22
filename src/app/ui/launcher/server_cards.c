#include "server_cards.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app.h"
#include "logging.h"
#include "coverloader.h"
#include "stream_actions.h"
#include "util/path.h"

#include "lvgl/font/material_icons_regular_symbols.h"
#include "lvgl/theme/lv_theme_moonlight.h"

#define CARDS_FILE "server-cards.txt"
#define CARDS_MAX 32

/* Our two children on a server tile, told apart from the window's own icon and
 * name, which stay underneath: the picture covers them once it has loaded. */
#define CARD_PICTURE LV_OBJ_FLAG_USER_1
#define CARD_BADGE LV_OBJ_FLAG_USER_2

typedef struct known_card_t {
    uuidstr_t host;
    int app_id;
} known_card_t;

/* A tile of the open window and what it shows. */
typedef struct shown_tile_t {
    lv_obj_t *tile;
    uuidstr_t host;
    const char *status_icon;
    SERVER_STATE_ENUM state;
    int picture_id;             /* the cover on it now, 0 for none */
    bool running_elsewhere;     /* its list shows an app another TV started */
} shown_tile_t;

/* A host's list, asked for while the window is open. */
typedef struct list_check_t {
    uuidstr_t host;
    apploader_t *loader;
    struct list_check_t *next;
} list_check_t;

static known_card_t s_cards[CARDS_MAX];
static int s_count = -1;            /* -1: the file is not read yet */
static coverloader_t *s_loader;
static app_t *s_app;
static shown_tile_t s_tiles[CARDS_MAX];
static int s_tile_count;
static list_check_t *s_checks;

static char *cards_path(void) {
    return path_join(app_configuration->conf_dir, CARDS_FILE);
}

static void cards_read(void) {
    s_count = 0;
    char *path = cards_path();
    FILE *file = fopen(path, "r");
    free(path);
    if (file == NULL) {
        return;
    }
    char line[128];
    while (s_count < CARDS_MAX && fgets(line, sizeof(line), file) != NULL) {
        char uuid[UUIDSTR_CAPACITY];
        int id;
        if (sscanf(line, "%36s %d", uuid, &id) == 2 && strlen(uuid) == UUIDSTR_LENGTH && id != 0) {
            uuidstr_fromstr(&s_cards[s_count].host, uuid);
            s_cards[s_count].app_id = id;
            s_count++;
        }
    }
    fclose(file);
}

static void cards_write(void) {
    char *path = cards_path();
    FILE *file = fopen(path, "w");
    if (file == NULL) {
        commons_log_warn("ServerCards", "Cannot write %s", path);
        free(path);
        return;
    }
    for (int i = 0; i < s_count; i++) {
        fprintf(file, "%.36s %d\n", s_cards[i].host.data, s_cards[i].app_id);
    }
    fclose(file);
    free(path);
}

static known_card_t *card_of(const uuidstr_t *host) {
    if (s_count < 0) {
        cards_read();
    }
    for (int i = 0; i < s_count; i++) {
        if (uuidstr_t_equals_t(&s_cards[i].host, host)) {
            return &s_cards[i];
        }
    }
    return NULL;
}

void server_cards_learn(const uuidstr_t *host, const apploader_list_t *apps) {
    if (host == NULL || apps == NULL) {
        return;
    }
    int id = 0;
    for (size_t i = 0; i < apps->count; i++) {
        const char *name = apps->items[i].base.name;
        if (name != NULL && strcmp(name, "Desktop") == 0) {
            id = apps->items[i].base.id;
            break;
        }
    }
    if (id == 0) {
        return;         /* no Desktop in this list: what was known stays */
    }
    known_card_t *card = card_of(host);
    if (card != NULL && card->app_id == id) {
        return;         /* the list loads on every poll: write only a change */
    }
    if (card == NULL) {
        if (s_count >= CARDS_MAX) {
            return;
        }
        card = &s_cards[s_count++];
        card->host = *host;
    }
    /* Vibepollo gives an app a new id when its picture changes, so this
     * follows a new card too. */
    card->app_id = id;
    commons_log_info("ServerCards", "%.36s: its Desktop is app %d", host->data, id);
    cards_write();
}

static lv_obj_t *child_with(lv_obj_t *tile, lv_obj_flag_t flag) {
    uint32_t count = lv_obj_get_child_cnt(tile);
    for (uint32_t i = 0; i < count; i++) {
        lv_obj_t *child = lv_obj_get_child(tile, (int32_t) i);
        if (lv_obj_has_flag(child, flag)) {
            return child;
        }
    }
    return NULL;
}

static void remove_children(lv_obj_t *tile, lv_obj_flag_t flag) {
    lv_obj_t *old;
    while ((old = child_with(tile, flag)) != NULL) {
        lv_obj_del(old);
    }
}

static void add_badge(lv_obj_t *tile, lv_coord_t width, const char *icon, SERVER_STATE_ENUM state) {
    const lv_font_t *font = lv_theme_moonlight_get_iconfont_small(tile);
    lv_coord_t size = LV_MAX(font->line_height + LV_DPX(6), width * 2 / 11);
    lv_obj_t *badge = lv_label_create(tile);
    lv_obj_add_flag(badge, CARD_BADGE);
    lv_obj_clear_flag(badge, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(badge, size, size);
    lv_obj_set_style_radius(badge, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(badge, lv_color_hex(0x3C3C42), 0);
    lv_obj_set_style_bg_opa(badge, LV_OPA_COVER, 0);
    lv_obj_set_style_text_font(badge, font, 0);
    lv_obj_set_style_text_color(badge, state == SERVER_STATE_OFFLINE || state == SERVER_STATE_ERROR
                                       ? lv_color_hex(0xFFC400) : lv_color_white(), 0);
    lv_obj_set_style_text_align(badge, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_top(badge, LV_MAX(0, (size - font->line_height) / 2), 0);
    lv_label_set_text_static(badge, icon);
    lv_obj_align(badge, LV_ALIGN_TOP_RIGHT, -size / 4, size / 4);
}

/* What a tile shows: the host's card, if one is known, and the corner badge
 * for a host that is not simply ready. */
static void draw(shown_tile_t *shown) {
    lv_obj_t *tile = shown->tile;
    lv_coord_t width = lv_obj_get_style_width(tile, LV_PART_MAIN);
    lv_coord_t height = lv_obj_get_style_height(tile, LV_PART_MAIN);
    const known_card_t *card = card_of(&shown->host);
    int id = card != NULL ? card->app_id : 0;
    if (id != shown->picture_id) {
        remove_children(tile, CARD_PICTURE);
        shown->picture_id = 0;
    }
    if (id != 0 && shown->picture_id == 0 && s_app != NULL) {
        /* ⚠️ The picture is a child with no user data: the cover loader takes an
         * object with user data for a game tile and reads it as one, and a
         * server tile's user data is something else. */
        lv_obj_t *picture = lv_img_create(tile);
        lv_obj_add_flag(picture, CARD_PICTURE);
        lv_obj_clear_flag(picture, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_size(picture, width, height);
        lv_obj_align(picture, LV_ALIGN_TOP_LEFT, 0, 0);
        lv_img_set_antialias(picture, true);
        if (shown->state == SERVER_STATE_OFFLINE || shown->state == SERVER_STATE_ERROR ||
            shown->state == SERVER_STATE_NOT_PAIRED) {
            lv_obj_set_style_img_recolor(picture, lv_color_black(), 0);
            lv_obj_set_style_img_recolor_opa(picture, LV_OPA_50, 0);
        }
        if (s_loader == NULL) {
            s_loader = coverloader_new(s_app);
        }
        coverloader_display(s_loader, &shown->host, id, picture, width, height);
        shown->picture_id = id;
    }
    remove_children(tile, CARD_BADGE);
    bool ready = shown->status_icon == NULL || strcmp(shown->status_icon, MAT_SYMBOL_TV) == 0;
    if (ready && shown->running_elsewhere) {
        /* On a card or on the plain icon, which says only "ready". */
        add_badge(tile, width, MAT_SYMBOL_ONDEMAND_VIDEO, shown->state);
    } else if (!ready && id != 0) {
        /* The card covers the window's own icon: its status goes in the corner. */
        add_badge(tile, width, shown->status_icon, shown->state);
    }
}

/* rhoquinn8217, 2026-10-20, "2. yes": a computer streaming to another TV shows
 * it on its card. Vibepollo tells only the TV that started an app that it
 * runs; every other TV sees it in that host's list instead (the running app's
 * copy, Resume, Terminate). So the window asks each ready host for its list
 * once, as it opens, and that also teaches it the card of a host whose games
 * this TV never showed. */
static void check_free(list_check_t *check) {
    list_check_t **link = &s_checks;
    while (*link != NULL && *link != check) {
        link = &(*link)->next;
    }
    if (*link == check) {
        *link = check->next;
    }
    apploader_destroy(check->loader);
    free(check);
}

static void check_loaded(apploader_list_t *apps, void *userdata) {
    list_check_t *check = userdata;
    server_cards_learn(&check->host, apps);
    bool running = stream_actions_list_shows_running(apps);
    apploader_list_free(apps);
    for (int i = 0; i < s_tile_count; i++) {
        if (uuidstr_t_equals_t(&s_tiles[i].host, &check->host)) {
            s_tiles[i].running_elsewhere = running;
            draw(&s_tiles[i]);
        }
    }
    check_free(check);
}

static void check_failed(int code, const char *error, void *userdata) {
    (void) code;
    (void) error;
    check_free(userdata);
}

static void check_start(const uuidstr_t *host) {
    for (list_check_t *cur = s_checks; cur != NULL; cur = cur->next) {
        if (uuidstr_t_equals_t(&cur->host, host)) {
            return;
        }
    }
    static const apploader_cb_t callbacks = {.data = check_loaded, .error = check_failed};
    list_check_t *check = calloc(1, sizeof(list_check_t));
    if (check == NULL) {
        return;
    }
    check->host = *host;
    check->loader = apploader_create(s_app, host, &callbacks, check);
    check->next = s_checks;
    s_checks = check;
    apploader_load(check->loader);
}

void server_cards_bind(lv_obj_t *tile, app_t *app, const uuidstr_t *host, const char *status_icon,
                       SERVER_STATE_ENUM state) {
    if (tile == NULL || host == NULL) {
        return;
    }
    s_app = app;
    shown_tile_t *shown = NULL;
    for (int i = 0; i < s_tile_count; i++) {
        if (s_tiles[i].tile == tile) {
            shown = &s_tiles[i];
        }
    }
    if (shown == NULL) {
        if (s_tile_count >= CARDS_MAX) {
            return;
        }
        shown = &s_tiles[s_tile_count++];
        memset(shown, 0, sizeof(*shown));      /* a slot may hold an earlier window's tile */
        shown->tile = tile;
    }
    /* A tile is bound again when the grid reuses it for another host. */
    if (!uuidstr_t_equals_t(&shown->host, host)) {
        remove_children(tile, CARD_PICTURE);
        shown->picture_id = 0;
        shown->running_elsewhere = false;
    }
    shown->host = *host;
    shown->status_icon = status_icon;
    shown->state = state;
    draw(shown);
    if (state == SERVER_STATE_AVAILABLE && app != NULL) {
        check_start(host);
    }
}

/* ⛔ The loader goes only after the popup's pictures have, on LVGL's next pass.
 * This runs in the popup box's DELETE, which LVGL sends before it deletes the
 * tiles inside, and a tile's picture lets go of its cover in its own DELETE:
 * freed here at once, the loader left the pictures letting go of covers
 * already freed (the crash found in stream_actions.c, 2026-10-21). */
static void loader_unref_later(void *loader) {
    coverloader_unref(loader);
}

void server_cards_closed(void) {
    while (s_checks != NULL) {
        apploader_cancel(s_checks->loader);
        check_free(s_checks);
    }
    s_tile_count = 0;
    if (s_loader != NULL) {
        lv_async_call(loader_unref_later, s_loader);
        s_loader = NULL;
    }
}
