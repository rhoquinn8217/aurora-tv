#include "server_cards.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app.h"
#include "logging.h"
#include "coverloader.h"
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

static known_card_t s_cards[CARDS_MAX];
static int s_count = -1;            /* -1: the file is not read yet */
static coverloader_t *s_loader;

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

void server_cards_bind(lv_obj_t *tile, app_t *app, const uuidstr_t *host, const char *status_icon,
                       SERVER_STATE_ENUM state) {
    if (tile == NULL) {
        return;
    }
    /* A tile is bound again when the grid reuses it for another host. */
    lv_obj_t *old;
    while ((old = child_with(tile, CARD_PICTURE)) != NULL) {
        lv_obj_del(old);
    }
    while ((old = child_with(tile, CARD_BADGE)) != NULL) {
        lv_obj_del(old);
    }
    const known_card_t *card = host != NULL ? card_of(host) : NULL;
    if (card == NULL || app == NULL) {
        return;
    }
    lv_coord_t width = lv_obj_get_style_width(tile, LV_PART_MAIN);
    lv_coord_t height = lv_obj_get_style_height(tile, LV_PART_MAIN);

    /* ⚠️ The picture is a child with no user data: the cover loader takes an
     * object with user data for a game tile and reads it as one, and a server
     * tile's user data is something else. */
    lv_obj_t *picture = lv_img_create(tile);
    lv_obj_add_flag(picture, CARD_PICTURE);
    lv_obj_clear_flag(picture, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(picture, width, height);
    lv_obj_align(picture, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_img_set_antialias(picture, true);
    if (state == SERVER_STATE_OFFLINE || state == SERVER_STATE_ERROR || state == SERVER_STATE_NOT_PAIRED) {
        lv_obj_set_style_img_recolor(picture, lv_color_black(), 0);
        lv_obj_set_style_img_recolor_opa(picture, LV_OPA_50, 0);
    }
    if (s_loader == NULL) {
        s_loader = coverloader_new(app);
    }
    coverloader_display(s_loader, host, card->app_id, picture, width, height);

    if (status_icon != NULL && strcmp(status_icon, MAT_SYMBOL_TV) != 0) {
        add_badge(tile, width, status_icon, state);
    }
}

void server_cards_closed(void) {
    if (s_loader != NULL) {
        coverloader_unref(s_loader);
        s_loader = NULL;
    }
}
