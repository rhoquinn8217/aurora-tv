/* The bar that aligns the TV cursor with the PC's. The why is in the header. */

#include "cursor_align.h"

#include <math.h>

#include <SDL.h>

#include "app.h"
#include "ui/ui_input.h"
#include "util/i18n.h"
#include "stream/session.h"
#include "stream/input/session_input.h"
#include "streaming.controller.h"
#include "bridge_prompt.h"
#include "input/bridge_keyboard.h"
#include "input/tv_cursor.h"
#include "input/mouse_seen.h"     /* mouse_seen_wake */
#include "input/loop_watch.h"

/* The USB Bridge panel's colours (ctm_panel.c), so the two read as one family. */
#define ALIGN_COL_CARD    lv_color_hex(0x12181d)
#define ALIGN_COL_BORDER  lv_color_hex(0x2a3540)
#define ALIGN_COL_TXT     lv_color_hex(0xf5f8fa)
#define ALIGN_COL_SUB     lv_color_hex(0x95a3b0)
#define ALIGN_COL_PC_MARK lv_color_hex(0xffffff)   /* step 1's + */
#define ALIGN_COL_TV_MARK lv_color_hex(0x45d0ff)   /* step 2's + */
#define ALIGN_COL_HALO    lv_color_hex(0x0b0f13)

/* ⭐ TWO STEPS, AND webOS's PICTURE CHANGED ONCE (rhoquinn8217, 2026-11-14: "what if you
 * place a marker on screen over the stream. make the cursor invisible and have the user
 * move the windows cursor to align with that marker and click", then "go" to a second
 * step for where webOS paints its own arrow, and "yes" to the layout). On webOS 23 and
 * 24 a change of the TV cursor's picture while its pointer showed made the app wait 4 to
 * 6 s for webOS (2026-11-13, the C3 and the B4; never on the C1's webOS 6), and every
 * press of the bar before this was one. Now nothing changes the picture until the end:
 * 1. The TV arrow hidden, a white + at the screen's middle: the PC's arrow is moved
 *    onto it and clicked (or Enter or A). Where the PC's tip shows, against the pointer.
 * 2. The TV arrow shown, the PC's arrow sent to a corner, a blue + in the same place:
 *    the TV arrow is moved onto it and clicked (or Enter or A). Where webOS paints its
 *    tip, which the app has no other way to know: it never sees the screen. As step 1
 *    is, with the mouse and a click (rhoquinn8217, 2026-11-14, at the B4, of a step 2
 *    that moved the + with the d-pad and saved on Enter: "the crosshair and the webOS
 *    cursor are moving together and clicking doesn't end the alignment").
 * In both, the arrows or the d-pad move the + a TV pixel, for the last bit: the mouse
 * moves a pointer two TV pixels at a time. The second click works out the shift that
 * puts the one tip on the other, webOS's one new picture (tv_cursor_adjust), kept as
 * this TV's. ⓘ webOS's pointer moves a whole point of the
 * app's at a time, two TV pixels (2026-11-14, the B4), and Windows' at twice the app's
 * point: both measured, so this needs no guess about either. */
typedef enum {
    STEP_PC = 1,
    STEP_TV = 2,
} align_step_t;

/* The bar: a clear layer over the whole screen, which takes the keys and every
 * click, with the bar at its top. */
static lv_obj_t *s_bar = NULL;
static lv_group_t *s_group = NULL;
static lv_group_t *s_prev_group = NULL;   /* where the keys went before, and go back to */
static lv_obj_t *s_title = NULL;
static lv_obj_t *s_hint = NULL;
static lv_timer_t *s_awake = NULL;        /* keeps webOS's pointer awake */
static int s_wake_said = -1;              /* what the log last said of it: -1 nothing, 0 none, 1 some */
static bool s_aligning = false;
static bool s_open_queued = false;
static align_step_t s_step = STEP_PC;
static int s_shown_was = SDL_ENABLE;      /* SDL's cursor shown or not before the bar */

/* The +, drawn by the app: its middle in the TV's own pixels. */
static lv_obj_t *s_mark = NULL;
static int s_mark_x = 0, s_mark_y = 0;
#define ALIGN_MARK_BOX 15    /* the +'s object reaches this many points each way from its middle */
#define ALIGN_MARK_ARM 10    /* each arm's far end, in points from the middle */
#define ALIGN_MARK_GAP 3     /* and its near end: the middle stays clear */

/* Step 1's answer: the PC's tip against the pointer, in the TV's pixels. */
static int s_pc_dx = 0, s_pc_dy = 0;
static int s_shift_x = 0, s_shift_y = 0;  /* the shift the TV arrow is drawn with */

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

/* Once a second while the bar is up: webOS hides a still mouse's pointer after
 * 5 seconds, and it is woken well before (input/mouse_seen.h). */
#define ALIGN_AWAKE_MS 1000

static void keep_awake(void) {
    const int woken = mouse_seen_wake();
    const int some = woken > 0 ? 1 : 0;
    if (some == s_wake_said) {
        return;
    }
    s_wake_said = some;
    if (some) {
        tv_cursor_log("align bar: the pointer kept awake through %d mouse(s) the TV saw move", woken);
    } else {
        tv_cursor_log("align bar: no mouse seen moving yet, so nothing keeps the pointer awake until one moves");
    }
}

/* ⓘ Step 2's alone: in step 1 the TV arrow is hidden, and each wake would move the
 * PC's arrow a point and back as its tip is being put on the +. */
static void awake_cb(lv_timer_t *timer) {
    LV_UNUSED(timer);
    if (s_aligning && s_step == STEP_TV) {
        const unsigned long long watch_at = loop_watch_now();   /* input/loop_watch.h */
        keep_awake();
        loop_watch_add("the bar's keep-awake", loop_watch_now() - watch_at);
    }
}

static void stop_awake(void) {
    if (s_awake != NULL) {
        lv_timer_del(s_awake);
        s_awake = NULL;
    }
}

/* ⭐ THE MOUSE MOVES THE PC'S CURSOR IN STEP 1, so its tip can be put on the +. The
 * bar takes the input as the overlay does, and with it the stream's mouse; so each
 * move is handed to the stream here, as upstream's own handler hands it over
 * (stream_input_handle_mmotion), and nothing else is: a click, the wheel and every key
 * stay on the TV. In step 2 nothing goes: the PC's cursor waits in its corner.
 * ⓘ SDL calls this as it takes the event in, on the app's loop. */
static int SDLCALL align_motion_watch(void *userdata, SDL_Event *event) {
    LV_UNUSED(userdata);
    if (event->type == SDL_MOUSEMOTION && s_aligning && s_step == STEP_PC && global != NULL &&
        global->session != NULL) {
        stream_input_handle_mmotion(session_get_input(global->session), &event->motion, false);
    }
    return 1;
}

/* The host's cursor to a point of the app's screen, as the stream hands it a move. */
static void host_cursor_to(int x, int y) {
    if (global == NULL || global->session == NULL) {
        return;
    }
    SDL_Event event;
    SDL_zero(event);
    event.motion.type = SDL_MOUSEMOTION;
    event.motion.timestamp = SDL_GetTicks();
    event.motion.x = x;
    event.motion.y = y;
    stream_input_handle_mmotion(session_get_input(global->session), &event.motion, false);
}

/* The host's cursor to the TV pointer, so the two go on together. */
static void host_cursor_to_pointer(void) {
    int x = 0, y = 0;
    SDL_GetMouseState(&x, &y);
    host_cursor_to(x, y);
}

static double tv_per_point(void) {
    const double scale = tv_cursor_scale();
    return scale > 1.0 ? scale : 1.0;
}

/* A line one point thick from a1 to a2 (whole points), its near edge at c (in points,
 * maybe half of one: the TV's own pixel), split over two points by how far it is in. */
static void mark_line(lv_draw_ctx_t *ctx, bool vertical, double c, lv_coord_t a1, lv_coord_t a2, lv_color_t color,
                      lv_opa_t opa) {
    const double low = floor(c);
    const double in = c - low;
    for (int k = 0; k < 2; ++k) {
        const double share = k == 0 ? 1.0 - in : in;
        if (share < 0.05) {
            continue;
        }
        const lv_coord_t at = (lv_coord_t) low + k;
        lv_area_t area;
        if (vertical) {
            area.x1 = at;
            area.x2 = at;
            area.y1 = a1;
            area.y2 = a2;
        } else {
            area.x1 = a1;
            area.x2 = a2;
            area.y1 = at;
            area.y2 = at;
        }
        lv_draw_rect_dsc_t dsc;
        lv_draw_rect_dsc_init(&dsc);
        dsc.bg_color = color;
        dsc.bg_opa = (lv_opa_t) (opa * share + 0.5);
        lv_draw_rect(ctx, &dsc, &area);
    }
}

/* The +: four arms round a clear middle, each with a dark edge both sides, so it shows
 * on any picture. Drawn where its middle is now, to the TV's own pixel. */
static void mark_draw(lv_event_t *event) {
    lv_draw_ctx_t *ctx = lv_event_get_draw_ctx(event);
    const double per = tv_per_point();
    const double cx = s_mark_x / per;
    const double cy = s_mark_y / per;
    const lv_coord_t x0 = (lv_coord_t) floor(cx), y0 = (lv_coord_t) floor(cy);
    const lv_color_t color = s_step == STEP_PC ? ALIGN_COL_PC_MARK : ALIGN_COL_TV_MARK;
    for (int pass = 0; pass < 2; ++pass) {
        const bool halo = pass == 0;
        const lv_color_t c = halo ? ALIGN_COL_HALO : color;
        const lv_opa_t opa = halo ? LV_OPA_70 : LV_OPA_COVER;
        for (int side = -1; side <= 1; side += 2) {
            /* The arms across: left and right of the middle, then above and below it. */
            const lv_coord_t a1 = (lv_coord_t) (side < 0 ? x0 - ALIGN_MARK_ARM : x0 + ALIGN_MARK_GAP + 1);
            const lv_coord_t a2 = (lv_coord_t) (side < 0 ? x0 - ALIGN_MARK_GAP : x0 + ALIGN_MARK_ARM + 1);
            const lv_coord_t b1 = (lv_coord_t) (side < 0 ? y0 - ALIGN_MARK_ARM : y0 + ALIGN_MARK_GAP + 1);
            const lv_coord_t b2 = (lv_coord_t) (side < 0 ? y0 - ALIGN_MARK_GAP : y0 + ALIGN_MARK_ARM + 1);
            if (halo) {
                mark_line(ctx, false, cy - 1, a1, a2, c, opa);
                mark_line(ctx, false, cy + 1, a1, a2, c, opa);
                mark_line(ctx, true, cx - 1, b1, b2, c, opa);
                mark_line(ctx, true, cx + 1, b1, b2, c, opa);
            } else {
                mark_line(ctx, false, cy, a1, a2, c, opa);
                mark_line(ctx, true, cx, b1, b2, c, opa);
            }
        }
    }
}

/* The + to a point of the screen, in the TV's own pixels. */
static void mark_to(int x, int y) {
    if (s_mark == NULL) {
        return;
    }
    s_mark_x = x;
    s_mark_y = y;
    const double per = tv_per_point();
    lv_obj_set_pos(s_mark, (lv_coord_t) (floor(x / per) - ALIGN_MARK_BOX),
                   (lv_coord_t) (floor(y / per) - ALIGN_MARK_BOX));
    lv_obj_invalidate(s_mark);
}

static void step_words(void) {
    if (s_title == NULL || s_hint == NULL) {
        return;
    }
    if (s_step == STEP_PC) {
        lv_label_set_text(s_title, locstr("Align the TV cursor   |   step 1 of 2: the PC's arrow"));
        lv_label_set_text(s_hint, locstr("Move the mouse until the tip of the PC's arrow is on the +, then click"
                                         "   |   Arrows or d-pad: move the + 1 TV pixel   |   Esc or B: cancel"));
    } else {
        lv_label_set_text(s_title, locstr("Align the TV cursor   |   step 2 of 2: the TV's arrow"));
        lv_label_set_text(s_hint, locstr("Move the mouse until the tip of the TV arrow is on the +, then click to save"
                                         "   |   Arrows or d-pad: move the + 1 TV pixel   |   Esc or B: cancel"));
    }
}

/* Step 1 answered: where the pointer stands with the PC's tip on the +. */
static void step_pc_done(void) {
    int px = 0, py = 0;
    SDL_GetMouseState(&px, &py);
    const double per = tv_per_point();
    s_pc_dx = s_mark_x - (int) lround(px * per);
    s_pc_dy = s_mark_y - (int) lround(py * per);
    tv_cursor_log("align: step 1, the PC's tip on the + at %d,%d of the TV's pixels with the pointer at %d,%d "
                  "of the app's points: the PC's tip %+d,%+d from the pointer",
                  s_mark_x, s_mark_y, px, py, s_pc_dx, s_pc_dy);
    s_step = STEP_TV;
    tv_cursor_get_shift(&s_shift_x, &s_shift_y);
    /* The TV arrow back, as it is now, and the PC's out of its way; the + stays, blue. */
    SDL_ShowCursor(SDL_ENABLE);
    host_cursor_to(0, lv_disp_get_ver_res(NULL) - 1);
    step_words();
    lv_obj_invalidate(s_mark);
    keep_awake();
}

/* Enter or A keeps the shift worked out, Esc or B leaves the cursor as it was. */
static void bar_close(bool keep) {
    if (!s_aligning) {
        return;
    }
    /* ⛔ First, while the bar still holds the input: a bridged keyboard taken
     * from webOS is let go, so the core can take it back as the hold ends
     * (input/bridge_keyboard.h). */
    bridge_keyboard_take_bridged(false);
    const align_step_t step = s_step;
    int px = 0, py = 0;
    SDL_GetMouseState(&px, &py);
    s_aligning = false;
    SDL_DelEventWatch(align_motion_watch, NULL);
    stop_awake();
    SDL_ShowCursor(s_shown_was);
    if (keep && step == STEP_TV) {
        /* The TV's tip, drawn with the shift it has, is on the + with the pointer here: it
         * sits tv off where it was asked to be. The PC's sits s_pc off the pointer. So
         * the shift that puts the one on the other is s_pc less tv: webOS's one new
         * picture. */
        const double per = tv_per_point();
        const int tv_dx = s_mark_x - (int) lround(px * per) - s_shift_x;
        const int tv_dy = s_mark_y - (int) lround(py * per) - s_shift_y;
        const int right = s_pc_dx - tv_dx;
        const int down = s_pc_dy - tv_dy;
        tv_cursor_log("align: step 2, the TV's tip on the + at %d,%d with the pointer at %d,%d: drawn %+d,%+d off "
                      "where it was asked to be; shifted %d,%d now, %d,%d before",
                      s_mark_x, s_mark_y, px, py, tv_dx, tv_dy, right, down, s_shift_x, s_shift_y);
        if (right != s_shift_x || down != s_shift_y) {
            tv_cursor_adjust(right, down, -1);
        }
        tv_cursor_keep();
    } else {
        tv_cursor_log("align bar: closed at step %d, nothing kept", (int) step);
    }
    host_cursor_to_pointer();
    lv_obj_t *bar = s_bar;
    lv_group_t *group = s_group;
    s_bar = NULL;
    s_group = NULL;
    s_title = NULL;
    s_hint = NULL;
    s_mark = NULL;   /* the layer's: it goes with it */
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
    const uint32_t key = lv_event_get_key(event);
    if (key == LV_KEY_ESC) {
        bar_close(false);
        return;
    }
    int x = s_mark_x, y = s_mark_y;
    switch (key) {
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
        default:
            return;
    }
    /* ⓘ Only the app's own + moves: webOS's picture is not touched until the end. */
    mark_to(x, y);
}

/* ⓘ Enter and A come as a click when they come UP, as everywhere in this
 * interface, and the mouse's click arrives here too, as the layer takes every
 * click: either ends either step. */
static void bar_clicked_cb(lv_event_t *event) {
    LV_UNUSED(event);
    if (!s_aligning) {
        return;
    }
    if (s_step == STEP_PC) {
        step_pc_done();
        return;
    }
    bar_close(true);
}

static void bar_open(void) {
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
    s_hint = lv_label_create(card);
    lv_obj_set_style_text_font(s_hint, lv_theme_get_font_small(s_hint), 0);
    lv_obj_set_style_text_color(s_hint, ALIGN_COL_SUB, 0);

    /* The +, drawn by the app over the stream; nothing of it takes a click. */
    s_mark = lv_obj_create(layer);
    lv_obj_remove_style_all(s_mark);
    lv_obj_set_size(s_mark, 2 * ALIGN_MARK_BOX + 1, 2 * ALIGN_MARK_BOX + 1);
    lv_obj_clear_flag(s_mark, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_mark, mark_draw, LV_EVENT_DRAW_MAIN, NULL);

    s_step = STEP_PC;
    s_pc_dx = s_pc_dy = 0;
    step_words();
    const double per = tv_per_point();
    mark_to((int) lround(lv_disp_get_hor_res(NULL) / 2 * per), (int) lround(lv_disp_get_ver_res(NULL) / 2 * per));

    s_aligning = true;
    /* ⭐ A bridged keyboard's keys for the bar alone: webOS hides its pointer at
     * every key it reads (rhoquinn8217: "the cursor disappears whenever I
     * click an arrow key"), and the core lets the keyboard go while the bar
     * holds the input. */
    bridge_keyboard_take_bridged(true);
    s_prev_group = app_input_get_group(&global->ui.input);
    app_input_set_group(&global->ui.input, s_group);
    lv_group_focus_obj(layer);
    SDL_AddEventWatch(align_motion_watch, NULL);
    /* Step 1: the TV arrow hidden, the PC's where the pointer is. */
    s_shown_was = SDL_ShowCursor(SDL_QUERY);
    SDL_ShowCursor(SDL_DISABLE);
    host_cursor_to_pointer();
    s_wake_said = -1;
    stop_awake();
    s_awake = lv_timer_create(awake_cb, ALIGN_AWAKE_MS, NULL);
    tv_cursor_log("align bar: open, step 1, the + at %d,%d of the TV's pixels", s_mark_x, s_mark_y);
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
    /* ⓘ Neither says anything on the screen (rhoquinn8217, 2026-11-12: "you created
     * too many messages about the cursor doesn't need explaining"). */
    if (!tv_cursor_mode_on()) {
        tv_cursor_log("align bar: not opened, the TV cursor is off");
        return;
    }
    if (!tv_cursor_drawn()) {
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
        bridge_keyboard_take_bridged(false);
        s_aligning = false;
        SDL_DelEventWatch(align_motion_watch, NULL);
        SDL_ShowCursor(s_shown_was);
        tv_cursor_log("align bar: closed with the stream, nothing kept");
    }
    stop_awake();
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
    s_hint = NULL;
    s_mark = NULL;
    s_prev_group = NULL;
}
