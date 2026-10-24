/* The add-a-computer box's start and its clear button. The why is in the header. */

#include "add_server_box.h"

#include "lvgl/font/material_icons_regular_symbols.h"
#include "util/font.h"   /* app_fonts_t, which the theme's header names without declaring */
#include "lvgl/theme/lv_theme_moonlight.h"

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    lv_obj_t *clear;
    lv_obj_t *buttons;
    uint16_t ok;
} box_parts_t;

/* The TV's own IPv4 address without its last number ("192.168.1."): a cable's
 * first, then Wi-Fi's, then any other. Not loopback, and not Wi-Fi Direct
 * (p2p0), which holds an address of its own for screen sharing. */
static bool own_network_start(char *out, size_t len) {
    struct ifaddrs *list = NULL;
    if (getifaddrs(&list) != 0) {
        return false;
    }
    static const char *const kinds[] = {"eth", "wlan", ""};
    bool found = false;
    for (size_t k = 0; k < sizeof(kinds) / sizeof(kinds[0]) && !found; k++) {
        for (const struct ifaddrs *it = list; it != NULL; it = it->ifa_next) {
            if (it->ifa_addr == NULL || it->ifa_addr->sa_family != AF_INET || it->ifa_name == NULL ||
                (it->ifa_flags & IFF_UP) == 0 || (it->ifa_flags & IFF_LOOPBACK) != 0 ||
                strncmp(it->ifa_name, "p2p", 3) == 0 || strncmp(it->ifa_name, kinds[k], strlen(kinds[k])) != 0) {
                continue;
            }
            /* In network order, so its bytes read as written. */
            const uint8_t *b = (const uint8_t *) &((const struct sockaddr_in *) it->ifa_addr)->sin_addr.s_addr;
            snprintf(out, len, "%u.%u.%u.", b[0], b[1], b[2]);
            found = true;
            break;
        }
    }
    freeifaddrs(list);
    return found;
}

/* After upstream's own check (add.dialog.c, input_changed_cb), which ran first. */
static void box_changed(lv_event_t *event) {
    lv_obj_t *box = lv_event_get_current_target(event);
    const box_parts_t *parts = lv_event_get_user_data(event);
    const char *text = lv_textarea_get_text(box);
    const size_t len = text != NULL ? strlen(text) : 0;
    if (len > 0) {
        lv_obj_clear_flag(parts->clear, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(parts->clear, LV_OBJ_FLAG_HIDDEN);
    }
    if (len == 0 || text[len - 1] == '.') {
        lv_btnmatrix_set_btn_ctrl(parts->buttons, parts->ok, LV_BTNMATRIX_CTRL_DISABLED);
    }
}

static void clear_clicked(lv_event_t *event) {
    /* Its box sends value-changed, which hides the button and turns OK off. */
    lv_textarea_set_text(lv_obj_get_parent(lv_event_get_current_target(event)), "");
}

static void box_deleted(lv_event_t *event) {
    lv_mem_free(lv_event_get_user_data(event));
}

void add_server_box(lv_obj_t *box, lv_obj_t *buttons, uint16_t ok) {
    if (box == NULL || buttons == NULL) {
        return;
    }
    box_parts_t *parts = lv_mem_alloc(sizeof(box_parts_t));
    if (parts == NULL) {
        return;
    }
    /* A plain object, not a button: the window's focus stays on its buttons,
     * and the keys walk them as before; this one is for a pointer. Floating,
     * so it keeps its place while the box's one line scrolls. */
    const lv_coord_t size = LV_DPX(22);
    lv_obj_t *clear = lv_obj_create(box);
    lv_obj_remove_style_all(clear);
    lv_obj_add_flag(clear, LV_OBJ_FLAG_FLOATING | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(clear, LV_OBJ_FLAG_CLICK_FOCUSABLE | LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    lv_obj_set_size(clear, size, size);
    lv_obj_set_style_radius(clear, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(clear, lv_color_hex(0x58585C), 0);
    lv_obj_set_style_bg_color(clear, lv_color_hex(0x707076), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(clear, LV_OPA_COVER, 0);
    lv_obj_t *mark = lv_label_create(clear);
    lv_obj_set_style_text_font(mark, lv_theme_moonlight_get_iconfont_small(box), 0);
    lv_obj_set_style_text_color(mark, lv_color_hex(0xEBEBEB), 0);
    lv_label_set_text_static(mark, MAT_SYMBOL_CLOSE);
    lv_obj_center(mark);
    /* The text stops short of it: the box's right padding grows by the button
     * and a gap, and the button sits in that growth, as far from the box's edge
     * as the text was. Aligned to the content's right end, so moved out by as
     * much as the padding grew. */
    const lv_coord_t gap = LV_DPX(8);
    lv_obj_set_style_pad_right(box, lv_obj_get_style_pad_right(box, LV_PART_MAIN) + size + gap, 0);
    lv_obj_align(clear, LV_ALIGN_RIGHT_MID, size + gap, 0);
    lv_obj_add_event_cb(clear, clear_clicked, LV_EVENT_CLICKED, NULL);

    parts->clear = clear;
    parts->buttons = buttons;
    parts->ok = ok;
    lv_obj_add_event_cb(box, box_changed, LV_EVENT_VALUE_CHANGED, parts);
    lv_obj_add_event_cb(box, box_deleted, LV_EVENT_DELETE, parts);

    /* The cursor ends after it. Setting it sends value-changed, so OK and the
     * button are right from the start. */
    char start[20];
    if (own_network_start(start, sizeof(start))) {
        lv_textarea_set_text(box, start);
    } else {
        lv_obj_add_flag(clear, LV_OBJ_FLAG_HIDDEN);
    }
}
