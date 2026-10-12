/* The overlay's Mouse button's menu, and the bar that aligns the TV cursor.
 * The why is in the header. */

#include "mouse_menu.h"

#include <stdio.h>

#include <SDL.h>

#include "app.h"
#include "ui/ui_input.h"
#include "util/i18n.h"
#include "stream/session.h"
#include "stream/bridge_session.h"
#include "stream/input/session_input.h"
#include "input/tv_cursor.h"
#include "lvgl/font/material_icons_regular_symbols.h"
#include "lvgl/theme/lv_theme_moonlight.h"

/* The USB Bridge panel's colours (ctm_panel.c), so the two read as one family. */
#define MENU_COL_CARD    lv_color_hex(0x12181d)
#define MENU_COL_ROW     lv_color_hex(0x232f39)
#define MENU_COL_FOCUS   lv_color_hex(0x2563eb)
#define MENU_COL_BORDER  lv_color_hex(0x2a3540)
#define MENU_COL_TXT     lv_color_hex(0xf5f8fa)
#define MENU_COL_SUB     lv_color_hex(0x95a3b0)
#define MENU_COL_DIM     lv_color_hex(0x5f6b77)
#define MENU_COL_ON      lv_color_hex(0x35c46a)
#define MENU_COL_ON_TXT  lv_color_hex(0x0b2a17)
#define MENU_COL_OFF     lv_color_hex(0x4a5866)

static streaming_controller_t *s_owner = NULL;   /* whose overlay the button is in */
static lv_obj_t *s_button = NULL;                /* the Mouse button */

/* The menu: a clear backdrop over the whole screen, which catches a click
 * outside the card, with the card on it. */
static lv_obj_t *s_menu = NULL;
static lv_group_t *s_menu_group = NULL;
static lv_obj_t *s_cursor_badge = NULL, *s_cursor_sub = NULL;
static lv_obj_t *s_align_title = NULL, *s_align_sub = NULL, *s_align_icon = NULL;
static lv_obj_t *s_vmouse_badge = NULL;

/* The bar: a clear layer over the whole screen, which takes the keys and every
 * click, with the bar at its top. */
static lv_obj_t *s_bar = NULL;
static lv_group_t *s_bar_group = NULL;
static lv_obj_t *s_bar_title = NULL;
static bool s_aligning = false;
static int s_was_x = 0, s_was_y = 0;   /* the shift before aligning, which Esc puts back */

static void menu_close(void);

static void bar_open(void);

/* ⓘ A close hides at once and deletes on the next pass, as the USB Bridge
 * panel's does (ctm_panel.c): the key event that closed it is still being
 * handled, with its group, when the close returns. */
typedef struct {
    lv_obj_t *obj;
    lv_group_t *group;
} buried_t;

static void bury_now(void *p) {
    buried_t *b = p;
    if (b->obj != NULL) {
        lv_obj_del(b->obj);
    }
    if (b->group != NULL) {
        lv_group_del(b->group);
    }
    lv_mem_free(b);
}

static void bury(lv_obj_t *obj, lv_group_t *group) {
    if (obj != NULL) {
        lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    }
    buried_t *b = lv_mem_alloc(sizeof(*b));
    if (b == NULL) {
        return;   /* ⓘ left hidden: two objects lost beat deleting what is in use */
    }
    b->obj = obj;
    b->group = group;
    if (lv_async_call(bury_now, b) != LV_RES_OK) {
        lv_mem_free(b);   /* the same */
    }
}

/* The keys go back to the overlay's buttons, where they were. */
static void keys_to_overlay(void) {
    if (s_owner != NULL) {
        app_input_set_group(&global->ui.input, s_owner->group);
    }
}

/* ---- the menu --------------------------------------------------------- */

static void set_badge(lv_obj_t *badge, bool on) {
    if (badge == NULL) {
        return;
    }
    lv_label_set_text(badge, on ? locstr("On") : locstr("Off"));
    lv_obj_set_style_bg_color(badge, on ? MENU_COL_ON : MENU_COL_OFF, 0);
    lv_obj_set_style_text_color(badge, on ? MENU_COL_ON_TXT : MENU_COL_TXT, 0);
}

/* Each row's line under its title, its badge, and whether Align can be used,
 * as things stand now. */
static void menu_refresh(void) {
    if (s_menu == NULL) {
        return;
    }
    const bool on = tv_cursor_mode_on();
    set_badge(s_cursor_badge, on);
    lv_label_set_text(s_cursor_sub, tv_cursor_mode_switchable()
                                    ? locstr("Off for games that turn with the mouse")
                                    : locstr("Not while \"Use mouse hardware\" is on"));
    const bool can_align = on && tv_cursor_drawn();
    if (can_align) {
        int x = 0, y = 0;
        tv_cursor_get_shift(&x, &y);
        lv_label_set_text_fmt(s_align_sub, locstr("This TV: right %d, down %d"), x, y);
    } else {
        lv_label_set_text(s_align_sub, on ? locstr("The TV's own pointer is in use: nothing to align")
                                          : locstr("Turn the TV cursor on to align it"));
    }
    lv_obj_set_style_text_color(s_align_title, can_align ? MENU_COL_TXT : MENU_COL_DIM, 0);
    lv_obj_set_style_text_color(s_align_icon, can_align ? MENU_COL_SUB : MENU_COL_DIM, 0);
    set_badge(s_vmouse_badge, global != NULL && bridge_session_vmouse_active(global->session));
}

static void row_key_cb(lv_event_t *event) {
    if (s_menu_group == NULL) {
        return;   /* a key still on its way to a menu that has closed */
    }
    switch (lv_event_get_key(event)) {
        case LV_KEY_UP:
            lv_group_focus_prev(s_menu_group);
            break;
        case LV_KEY_DOWN:
            lv_group_focus_next(s_menu_group);
            break;
        case LV_KEY_ESC:
            menu_close();
            break;
        default:
            break;
    }
}

/* ⓘ Every row acts on its click, which comes as Select or the mouse button
 * comes UP: nothing of the press is left to land on the overlay once the menu
 * hands it the keys again (ctm_panel.c, at its close corner, says why that
 * matters). */
static void cursor_row_clicked(lv_event_t *event) {
    LV_UNUSED(event);
    tv_cursor_toggle_mode();
    menu_refresh();
}

static void align_row_clicked(lv_event_t *event) {
    LV_UNUSED(event);
    if (!tv_cursor_mode_on() || !tv_cursor_drawn() || s_owner == NULL) {
        return;   /* greyed out: the line under it says why */
    }
    streaming_controller_t *owner = s_owner;
    menu_close();
    /* ⓘ The overlay closes as it does for its own buttons: their click reaches
     * its root, and the root closes it (upstream's hide_overlay, on
     * LV_EVENT_CLICKED; tests/merge-guard.sh checks it is still there). That
     * also gives the stream back the whole screen and the mouse, which the bar
     * needs: the host's cursor follows the TV's only in a full-screen stream. */
    lv_event_send(owner->base.obj, LV_EVENT_CLICKED, NULL);
    bar_open();
}

static void vmouse_row_clicked(lv_event_t *event) {
    LV_UNUSED(event);
    if (global != NULL && global->session != NULL) {
        /* ⓘ Upstream's switch, as the overlay's own button uses it, Bridge
         * Override and all (stream/bridge_session.h). */
        session_toggle_vmouse(global->session);
    }
    menu_refresh();
}

/* A click on the backdrop itself, outside the card, closes the menu, as one
 * outside the USB Bridge panel closes it. */
static void backdrop_clicked(lv_event_t *event) {
    if (lv_event_get_target(event) != lv_event_get_current_target(event)) {
        return;
    }
    menu_close();
}

/* One row: a title with a line under it, and whatever goes on the right. */
static lv_obj_t *menu_row(lv_obj_t *card, const char *title, lv_obj_t **title_out, lv_obj_t **sub_out,
                          lv_event_cb_t clicked) {
    lv_obj_t *row = lv_obj_create(card);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(row, LV_DPX(12), 0);
    lv_obj_set_style_pad_ver(row, LV_DPX(8), 0);
    lv_obj_set_style_pad_column(row, LV_DPX(12), 0);
    lv_obj_set_style_radius(row, LV_DPX(6), 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(row, MENU_COL_ROW, 0);
    lv_obj_set_style_bg_color(row, MENU_COL_FOCUS, LV_STATE_FOCUSED);
    lv_obj_set_style_bg_color(row, MENU_COL_FOCUS, LV_STATE_FOCUS_KEY);
    /* ⛔ Not scrollable: Enter on a scrollable object starts editing it, and
     * never clicks. */
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(row, row_key_cb, LV_EVENT_KEY, NULL);
    lv_obj_add_event_cb(row, clicked, LV_EVENT_CLICKED, NULL);
    lv_group_add_obj(s_menu_group, row);

    /* The words, as wide as the row leaves them. ⓘ Not clickable, so a click
     * on them is the row's. */
    lv_obj_t *text = lv_obj_create(row);
    lv_obj_remove_style_all(text);
    lv_obj_set_height(text, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(text, 1);
    lv_obj_set_flex_flow(text, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(text, LV_DPX(2), 0);
    lv_obj_clear_flag(text, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *name = lv_label_create(text);
    lv_obj_set_style_text_color(name, MENU_COL_TXT, 0);
    lv_label_set_text(name, title);
    if (title_out != NULL) {
        *title_out = name;
    }
    lv_obj_t *sub = lv_label_create(text);
    lv_obj_set_width(sub, LV_PCT(100));
    lv_label_set_long_mode(sub, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(sub, lv_theme_get_font_small(sub), 0);
    lv_obj_set_style_text_color(sub, MENU_COL_SUB, 0);
    lv_label_set_text(sub, "");
    if (sub_out != NULL) {
        *sub_out = sub;
    }
    return row;
}

static lv_obj_t *row_badge(lv_obj_t *row) {
    lv_obj_t *badge = lv_label_create(row);
    lv_obj_set_style_bg_opa(badge, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(badge, LV_DPX(4), 0);
    lv_obj_set_style_pad_hor(badge, LV_DPX(10), 0);
    lv_obj_set_style_pad_ver(badge, LV_DPX(3), 0);
    lv_obj_set_style_text_font(badge, lv_theme_get_font_small(badge), 0);
    return badge;
}

static void menu_open(void) {
    s_menu_group = lv_group_create();
    lv_group_set_wrap(s_menu_group, false);

    /* ⓘ On the top layer, as the USB Bridge panel is, so the pinned stats
     * cannot cover it; and clear, so the game stays in view. */
    lv_obj_t *backdrop = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(backdrop);
    lv_obj_set_size(backdrop, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_opa(backdrop, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(backdrop, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(backdrop, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(backdrop, backdrop_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_move_foreground(backdrop);
    s_menu = backdrop;

    /* ⓘ Clickable, so a click on its own edge or title is its own, and does
     * not close the menu as one on the backdrop would. */
    lv_obj_t *card = lv_obj_create(backdrop);
    lv_obj_remove_style_all(card);
    lv_obj_set_width(card, LV_PCT(30));
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_bg_color(card, MENU_COL_CARD, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, LV_DPX(12), 0);
    lv_obj_set_style_border_width(card, LV_DPX(1), 0);
    lv_obj_set_style_border_color(card, MENU_COL_BORDER, 0);
    lv_obj_set_style_pad_all(card, LV_DPX(12), 0);
    lv_obj_set_style_pad_row(card, LV_DPX(8), 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *heading = lv_label_create(card);
    lv_obj_set_style_text_font(heading, lv_theme_get_font_small(heading), 0);
    lv_obj_set_style_text_color(heading, MENU_COL_SUB, 0);
    lv_obj_set_style_pad_left(heading, LV_DPX(6), 0);
    lv_label_set_text(heading, locstr("Mouse"));

    lv_obj_t *cursor_row = menu_row(card, locstr("TV cursor"), NULL, &s_cursor_sub, cursor_row_clicked);
    s_cursor_badge = row_badge(cursor_row);

    lv_obj_t *align_row = menu_row(card, locstr("Align TV cursor"), &s_align_title, &s_align_sub,
                                   align_row_clicked);
    s_align_icon = lv_label_create(align_row);
    lv_obj_set_style_text_font(s_align_icon, lv_theme_moonlight_get_iconfont_small(s_align_icon), 0);
    lv_label_set_text_static(s_align_icon, MAT_SYMBOL_TUNE);

    lv_obj_t *vmouse_row = menu_row(card, locstr("Virtual Mouse"), NULL, NULL, vmouse_row_clicked);
    s_vmouse_badge = row_badge(vmouse_row);

    menu_refresh();

    /* Above the Mouse button, its left edge on the button's. */
    lv_obj_update_layout(card);
    lv_obj_align_to(card, s_button, LV_ALIGN_OUT_TOP_LEFT, 0, -LV_DPX(12));

    app_input_set_group(&global->ui.input, s_menu_group);
    lv_group_focus_obj(cursor_row);
}

static void forget_menu(void) {
    s_menu = NULL;
    s_menu_group = NULL;
    s_cursor_badge = s_cursor_sub = NULL;
    s_align_title = s_align_sub = s_align_icon = NULL;
    s_vmouse_badge = NULL;
}

static void menu_close(void) {
    if (s_menu == NULL) {
        return;
    }
    lv_obj_t *menu = s_menu;
    lv_group_t *group = s_menu_group;
    forget_menu();
    keys_to_overlay();
    bury(menu, group);
}

/* ---- the bar that aligns the TV cursor ---------------------------------- */

/* ⭐ THE MOUSE STILL MOVES BOTH CURSORS WHILE THE BAR IS UP, so the tips can be
 * compared anywhere on the screen. The bar takes the input as the overlay
 * does, and with it the stream's mouse; so each move is handed to the stream
 * here, as upstream's own handler hands it over (stream_input_handle_mmotion),
 * and nothing else is: a click, the wheel and every key stay on the TV.
 * ⓘ SDL calls this as it takes the event in, on the app's loop. */
static int SDLCALL align_motion_watch(void *userdata, SDL_Event *event) {
    LV_UNUSED(userdata);
    if (event->type == SDL_MOUSEMOTION && s_aligning && global != NULL && global->session != NULL) {
        stream_input_handle_mmotion(session_get_input(global->session), &event->motion, false);
    }
    return 1;
}

/* The host's cursor to the TV cursor's tip, so the two start together: the
 * overlay had the mouse, and the host's cursor stayed where it was when the
 * overlay opened. */
static void host_cursor_to_tip(void) {
    if (global == NULL || global->session == NULL) {
        return;
    }
    SDL_Event event;
    SDL_zero(event);
    event.motion.type = SDL_MOUSEMOTION;
    event.motion.timestamp = SDL_GetTicks();
    SDL_GetMouseState(&event.motion.x, &event.motion.y);
    stream_input_handle_mmotion(session_get_input(global->session), &event.motion, false);
}

static void bar_refresh(void) {
    if (s_bar_title == NULL) {
        return;
    }
    int x = 0, y = 0;
    tv_cursor_get_shift(&x, &y);
    lv_label_set_text_fmt(s_bar_title, locstr("Align the TV cursor with the PC's   |   right %d, down %d"), x, y);
}

static void forget_bar(void) {
    s_bar = NULL;
    s_bar_group = NULL;
    s_bar_title = NULL;
}

/* Enter or A keeps it, Esc or B puts back what was there. */
static void bar_close(bool keep) {
    if (!s_aligning) {
        return;
    }
    s_aligning = false;
    SDL_DelEventWatch(align_motion_watch, NULL);
    if (keep) {
        tv_cursor_keep();
    } else {
        tv_cursor_adjust(s_was_x, s_was_y, -1);
    }
    lv_obj_t *bar = s_bar;
    lv_group_t *group = s_bar_group;
    forget_bar();
    keys_to_overlay();
    bury(bar, group);
}

static void bar_key_cb(lv_event_t *event) {
    if (!s_aligning) {
        return;
    }
    int x = 0, y = 0;
    tv_cursor_get_shift(&x, &y);
    switch (lv_event_get_key(event)) {
        case LV_KEY_LEFT:
            x--;
            break;
        case LV_KEY_RIGHT:
            x++;
            break;
        case LV_KEY_UP:
            y--;
            break;
        case LV_KEY_DOWN:
            y++;
            break;
        case LV_KEY_ESC:
            bar_close(false);
            return;
        default:
            return;
    }
    tv_cursor_adjust(x, y, -1);
    bar_refresh();
}

/* ⓘ Enter and A come as a click when they come UP, as everywhere in this
 * interface. A click of the mouse arrives here too, as the layer takes every
 * click, and does nothing: the mouse is for comparing the tips. */
static void bar_clicked_cb(lv_event_t *event) {
    LV_UNUSED(event);
    lv_indev_t *indev = lv_indev_get_act();
    if (indev == NULL || lv_indev_get_type(indev) != LV_INDEV_TYPE_KEYPAD) {
        return;
    }
    bar_close(true);
}

static void bar_open(void) {
    tv_cursor_get_shift(&s_was_x, &s_was_y);
    s_bar_group = lv_group_create();

    lv_obj_t *layer = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(layer);
    lv_obj_set_size(layer, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_opa(layer, LV_OPA_TRANSP, 0);
    /* ⛔ Not scrollable, or Enter would start editing it and never click. */
    lv_obj_clear_flag(layer, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(layer, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(layer, bar_key_cb, LV_EVENT_KEY, NULL);
    lv_obj_add_event_cb(layer, bar_clicked_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_move_foreground(layer);
    lv_group_add_obj(s_bar_group, layer);
    s_bar = layer;

    lv_obj_t *card = lv_obj_create(layer);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_bg_color(card, MENU_COL_CARD, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_90, 0);
    lv_obj_set_style_radius(card, LV_DPX(12), 0);
    lv_obj_set_style_border_width(card, LV_DPX(1), 0);
    lv_obj_set_style_border_color(card, MENU_COL_BORDER, 0);
    lv_obj_set_style_pad_hor(card, LV_DPX(20), 0);
    lv_obj_set_style_pad_ver(card, LV_DPX(10), 0);
    lv_obj_set_style_pad_row(card, LV_DPX(4), 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(card, LV_ALIGN_TOP_MID, 0, LV_DPX(24));

    s_bar_title = lv_label_create(card);
    lv_obj_set_style_text_color(s_bar_title, MENU_COL_TXT, 0);
    lv_obj_t *hint = lv_label_create(card);
    lv_obj_set_style_text_font(hint, lv_theme_get_font_small(hint), 0);
    lv_obj_set_style_text_color(hint, MENU_COL_SUB, 0);
    lv_label_set_text(hint, locstr("Arrows or d-pad: move it 1 TV pixel   |   Enter or A: save for this TV"
                                   "   |   Esc or B: cancel"));
    bar_refresh();

    s_aligning = true;
    app_input_set_group(&global->ui.input, s_bar_group);
    lv_group_focus_obj(layer);
    SDL_AddEventWatch(align_motion_watch, NULL);
    host_cursor_to_tip();
}

/* ---- the button --------------------------------------------------------- */

static void button_clicked(lv_event_t *event) {
    /* ⛔ Stopped here: the button passes its click up to the overlay's root,
     * which closes the overlay on any click that reaches it. The menu opens
     * over the overlay instead (ctm_panel.c's open_ctm_panel does the same). */
    lv_event_stop_bubbling(event);
    streaming_controller_t *controller = lv_event_get_user_data(event);
    if (controller == NULL || s_menu != NULL || s_aligning) {
        return;
    }
    s_owner = controller;
    menu_open();
}

/* Everything of the menu's and the bar's goes now, with nothing kept. */
static void drop_everything(void) {
    if (s_aligning) {
        s_aligning = false;
        SDL_DelEventWatch(align_motion_watch, NULL);
        tv_cursor_adjust(s_was_x, s_was_y, -1);   /* never kept, so not kept now */
    }
    /* ⓘ The streaming screen takes its keys away before its objects go
     * (upstream's on_delete_obj); asked again here in case. */
    lv_group_t *active = global != NULL ? app_input_get_group(&global->ui.input) : NULL;
    if (active != NULL && (active == s_menu_group || active == s_bar_group)) {
        app_input_set_group(&global->ui.input, NULL);
    }
    if (s_bar != NULL) {
        lv_obj_del(s_bar);
    }
    if (s_bar_group != NULL) {
        lv_group_del(s_bar_group);
    }
    if (s_menu != NULL) {
        lv_obj_del(s_menu);
    }
    if (s_menu_group != NULL) {
        lv_group_del(s_menu_group);
    }
    forget_bar();
    forget_menu();
}

/* The menu and the bar go with the button, which goes with the stream's
 * screen: both sit on the top layer, which outlives it. */
static void button_deleted(lv_event_t *event) {
    if (lv_event_get_target(event) != s_button) {
        return;
    }
    drop_everything();
    s_button = NULL;
    s_owner = NULL;
}

void mouse_menu_attach(lv_obj_t *button, streaming_controller_t *controller) {
    /* ⓘ Anything left by a screen that went without its delete event is
     * dropped, not trusted (bridge_overlay.c does the same with its timer). */
    drop_everything();
    s_button = button;
    s_owner = controller;
    lv_obj_add_event_cb(button, button_clicked, LV_EVENT_CLICKED, controller);
    lv_obj_add_event_cb(button, button_deleted, LV_EVENT_DELETE, NULL);
}

bool mouse_menu_aligning(void) {
    return s_aligning;
}
