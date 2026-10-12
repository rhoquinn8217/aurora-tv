/* The bar that aligns the TV cursor with the PC's. The why is in the header. */

#include "cursor_align.h"

#include <SDL.h>

#include "app.h"
#include "ui/ui_input.h"
#include "util/i18n.h"
#include "stream/session.h"
#include "stream/input/session_input.h"
#include "streaming.controller.h"
#include "bridge_prompt.h"
#include "input/tv_cursor.h"

/* The USB Bridge panel's colours (ctm_panel.c), so the two read as one family. */
#define ALIGN_COL_CARD    lv_color_hex(0x12181d)
#define ALIGN_COL_BORDER  lv_color_hex(0x2a3540)
#define ALIGN_COL_TXT     lv_color_hex(0xf5f8fa)
#define ALIGN_COL_SUB     lv_color_hex(0x95a3b0)

/* How often the pointer is put where it is while the bar is up: well inside the
 * 5 seconds webOS gives it (app.c). */
#define ALIGN_WAKE_MS 1000

/* The bar: a clear layer over the whole screen, which takes the keys and every
 * click, with the bar at its top. */
static lv_obj_t *s_bar = NULL;
static lv_group_t *s_group = NULL;
static lv_group_t *s_prev_group = NULL;   /* where the keys went before, and go back to */
static lv_obj_t *s_title = NULL;
static lv_timer_t *s_wake = NULL;
static bool s_aligning = false;
static bool s_open_queued = false;
static int s_was_x = 0, s_was_y = 0;      /* the shift before aligning, which Esc puts back */
static int s_wakes = 0;

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

/* The pointer put where it already is, which webOS takes as the mouse moving:
 * it stays up, and nothing moves. The header says why not the direct call. */
static void wake_pointer(void) {
    if (global == NULL || global->ui.window == NULL) {
        return;
    }
    int x = 0, y = 0;
    SDL_GetMouseState(&x, &y);
    SDL_WarpMouseInWindow(global->ui.window, x, y);
    if (s_wakes++ == 0) {
        tv_cursor_log("align bar: the pointer kept up, put where it is (%d,%d) once a second", x, y);
    }
}

static void wake_cb(lv_timer_t *timer) {
    LV_UNUSED(timer);
    if (s_aligning) {
        wake_pointer();
    }
}

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

/* The host's cursor to the TV cursor's tip, so the two start together. */
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
    if (s_title == NULL) {
        return;
    }
    int x = 0, y = 0;
    tv_cursor_get_shift(&x, &y);
    lv_label_set_text_fmt(s_title, locstr("Align the TV cursor with the PC's   |   right %d, down %d"), x, y);
}

/* Enter or A keeps it, Esc or B puts back what was there. */
static void bar_close(bool keep) {
    if (!s_aligning) {
        return;
    }
    s_aligning = false;
    SDL_DelEventWatch(align_motion_watch, NULL);
    if (s_wake != NULL) {
        lv_timer_del(s_wake);
        s_wake = NULL;
    }
    if (keep) {
        tv_cursor_keep();
    } else {
        tv_cursor_adjust(s_was_x, s_was_y, -1);
        tv_cursor_log("align bar: closed, nothing kept");
    }
    lv_obj_t *bar = s_bar;
    lv_group_t *group = s_group;
    s_bar = NULL;
    s_group = NULL;
    s_title = NULL;
    if (global != NULL) {
        app_input_set_group(&global->ui.input, s_prev_group);
    }
    s_prev_group = NULL;
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
    wake_pointer();
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
    s_group = lv_group_create();

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
    lv_group_add_obj(s_group, layer);
    s_bar = layer;

    lv_obj_t *card = lv_obj_create(layer);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_bg_color(card, ALIGN_COL_CARD, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_90, 0);
    lv_obj_set_style_radius(card, LV_DPX(12), 0);
    lv_obj_set_style_border_width(card, LV_DPX(1), 0);
    lv_obj_set_style_border_color(card, ALIGN_COL_BORDER, 0);
    lv_obj_set_style_pad_hor(card, LV_DPX(20), 0);
    lv_obj_set_style_pad_ver(card, LV_DPX(10), 0);
    lv_obj_set_style_pad_row(card, LV_DPX(4), 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(card, LV_ALIGN_TOP_MID, 0, LV_DPX(24));

    s_title = lv_label_create(card);
    lv_obj_set_style_text_color(s_title, ALIGN_COL_TXT, 0);
    lv_obj_t *hint = lv_label_create(card);
    lv_obj_set_style_text_font(hint, lv_theme_get_font_small(hint), 0);
    lv_obj_set_style_text_color(hint, ALIGN_COL_SUB, 0);
    lv_label_set_text(hint, locstr("Arrows or d-pad: move it 1 TV pixel   |   Enter or A: save for this TV"
                                   "   |   Esc or B: cancel"));
    bar_refresh();

    s_aligning = true;
    s_prev_group = app_input_get_group(&global->ui.input);
    app_input_set_group(&global->ui.input, s_group);
    lv_group_focus_obj(layer);
    SDL_AddEventWatch(align_motion_watch, NULL);
    host_cursor_to_tip();
    s_wakes = 0;
    wake_pointer();
    s_wake = lv_timer_create(wake_cb, ALIGN_WAKE_MS, NULL);
    tv_cursor_log("align bar: open at right %d, down %d", s_was_x, s_was_y);
}

/* ⓘ On the interface's next pass, never inside its reading of a key: the chord
 * from a keyboard webOS hands over is found there, and the bar takes the keys
 * from the group that is reading. */
static void open_async(void *p) {
    LV_UNUSED(p);
    s_open_queued = false;
    if (s_aligning || global == NULL || global->session == NULL) {
        return;
    }
    if (streaming_overlay_shown() || streaming_soft_keyboard_shown() || bridge_prompt_shown()) {
        tv_cursor_log("align bar: not opened, the interface has the input already");
        return;
    }
    if (!tv_cursor_mode_on()) {
        bridge_prompt_note(locstr("TV cursor is off. Press Ctrl+Alt+Shift+M to turn it on."));
        tv_cursor_log("align bar: not opened, the TV cursor is off");
        return;
    }
    if (!tv_cursor_drawn()) {
        bridge_prompt_note(locstr("The TV's own pointer is in use: there is nothing to align."));
        tv_cursor_log("align bar: not opened, the cursor is webOS's own");
        return;
    }
    bar_open();
}

void cursor_align_request(void) {
    if (s_open_queued) {
        return;
    }
    s_open_queued = true;
    if (lv_async_call(open_async, NULL) != LV_RES_OK) {
        s_open_queued = false;
    }
}

bool cursor_align_shown(void) {
    return s_aligning;
}

void cursor_align_stream_ended(void) {
    if (s_open_queued) {
        lv_async_call_cancel(open_async, NULL);
        s_open_queued = false;
    }
    if (s_aligning) {
        s_aligning = false;
        SDL_DelEventWatch(align_motion_watch, NULL);
        tv_cursor_adjust(s_was_x, s_was_y, -1);   /* never kept, so not kept now */
        tv_cursor_log("align bar: closed with the stream, nothing kept");
    }
    if (s_wake != NULL) {
        lv_timer_del(s_wake);
        s_wake = NULL;
    }
    /* ⓘ The streaming screen takes its keys away before its objects go
     * (upstream's on_delete_obj); asked again here in case, and the group the
     * keys came from is the screen's, so it is not handed back. */
    lv_group_t *active = global != NULL ? app_input_get_group(&global->ui.input) : NULL;
    if (active != NULL && active == s_group) {
        app_input_set_group(&global->ui.input, NULL);
    }
    if (s_bar != NULL) {
        lv_obj_del(s_bar);
    }
    if (s_group != NULL) {
        lv_group_del(s_group);
    }
    s_bar = NULL;
    s_group = NULL;
    s_title = NULL;
    s_prev_group = NULL;
}
