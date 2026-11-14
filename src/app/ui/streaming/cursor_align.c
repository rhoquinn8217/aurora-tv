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
#include "input/bridge_keyboard.h"
#include "input/tv_cursor.h"
#include "input/mouse_seen.h"     /* mouse_seen_wake */
#include "input/loop_watch.h"     /* a diagnostic: where a slow pass of the app's loop went */
#include "input/touchpad_pointer.h"   /* the touchpad moves the TV cursor (touchpad_pointer_stream_move) */
#if defined(TARGET_WEBOS)
#include "ctm_bridge_glue.h"   /* ctm_bridge_held_touches: a bridged pad's touchpad */
#endif

#include <stdio.h>
#include <string.h>

/* The USB Bridge panel's colours (ctm_panel.c), so the two read as one family. */
#define ALIGN_COL_CARD    lv_color_hex(0x12181d)
#define ALIGN_COL_BORDER  lv_color_hex(0x2a3540)
#define ALIGN_COL_TXT     lv_color_hex(0xf5f8fa)
#define ALIGN_COL_SUB     lv_color_hex(0x95a3b0)

/* The bar: a clear layer over the whole screen, which takes the keys and every
 * click, with the bar at its top. */
static lv_obj_t *s_bar = NULL;
static lv_group_t *s_group = NULL;
static lv_group_t *s_prev_group = NULL;   /* where the keys went before, and go back to */
static lv_obj_t *s_title = NULL;
static lv_timer_t *s_awake = NULL;        /* keeps webOS's pointer awake */
static int s_wake_said = -1;              /* what the log last said of it: -1 nothing, 0 none, 1 some */
static bool s_aligning = false;
static bool s_open_queued = false;
static int s_was_x = 0, s_was_y = 0;      /* the shift before aligning, which Esc puts back */

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

static void awake_cb(lv_timer_t *timer) {
    LV_UNUSED(timer);
    if (s_aligning) {
        const unsigned long long watch_at = loop_watch_now();
        keep_awake();
        /* With no mouse since the app started, the touchpad's own cursor is the TV
         * cursor here: kept up while the bar is, as webOS's pointer is kept awake. */
        if (!mouse_seen_any() && global != NULL && global->session != NULL) {
            touchpad_pointer_stream_move(session_get_input(global->session), 0, 0);
        }
        loop_watch_add("the bar's keep-awake", loop_watch_now() - watch_at);
    }
}

static void stop_awake(void) {
    if (s_awake != NULL) {
        lv_timer_del(s_awake);
        s_awake = NULL;
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

/* ⭐ THE TOUCHPAD MOVES BOTH CURSORS WHILE THE BAR IS UP, as the mouse does
 * (rhoquinn8217, 2026-11-14, at the B4, of a bar only the mouse moved: "the touchpad
 * should move the webOS cursor too which is what the alignment is also meant for").
 * The bar holds the input, so the stream reads no pad, and a bridged pad's touchpad
 * is let go (stream/input/bridged_touchpad.h). So one finger's movement is read here,
 * from SDL for a pad the TV holds and from the bridge core for a bridged one, and
 * handed to the touchpad's own move (input/touchpad_pointer.h): webOS's pointer once
 * a mouse has moved, Aurora's own cursor before that, and the host told the point.
 * Taps, presses and a second finger do nothing here, as the mouse's clicks do nothing.
 * ⓘ Gathered as SDL takes the events in, and moved on the bar's timer: a pointer jump
 * from inside SDL's own taking in of events would add an event there. */
#define ALIGN_TOUCH_MS 8
#define ALIGN_PS5_ASPECT (1070.0f / 1920.0f)   /* a pad's height in its widths, as session_gamepad.c has it */
#define ALIGN_PS4_ASPECT (920.0f / 1920.0f)
static lv_timer_t *s_touch = NULL;
static SDL_JoystickID s_touch_pad = -1;
static int s_touch_finger = -1;
static float s_touch_x = 0, s_touch_y = 0, s_touch_aspect = ALIGN_PS5_ASPECT;
static float s_touch_dx = 0, s_touch_dy = 0;   /* gathered, in the pad's widths */
/* ⭐ L3 closes the bar it opens, kept as Cross keeps it (rhoquinn8217, 2026-11-14, "A":
 * L3 opens and closes the bar, ui/streaming/desktop_mode.h). The bar has the input, so
 * the TV's controls never see it: it is caught here, with the touchpad. */
static bool s_l3_close = false;

static void bar_close(bool keep);

static float pad_aspect(SDL_JoystickID which) {
    SDL_GameController *pad = SDL_GameControllerFromInstanceID(which);
    if (pad != NULL && SDL_GameControllerGetType(pad) == SDL_CONTROLLER_TYPE_PS4) {
        return ALIGN_PS4_ASPECT;
    }
    return ALIGN_PS5_ASPECT;
}

static int SDLCALL align_touch_watch(void *userdata, SDL_Event *event) {
    LV_UNUSED(userdata);
    if (!s_aligning) {
        return 1;
    }
    switch (event->type) {
        case SDL_CONTROLLERTOUCHPADDOWN:
            if (s_touch_finger < 0) {
                s_touch_pad = event->ctouchpad.which;
                s_touch_finger = event->ctouchpad.finger;
                s_touch_x = event->ctouchpad.x;
                s_touch_y = event->ctouchpad.y;
                s_touch_aspect = pad_aspect(event->ctouchpad.which);
            }
            break;
        case SDL_CONTROLLERTOUCHPADMOTION:
            if (event->ctouchpad.which == s_touch_pad && event->ctouchpad.finger == s_touch_finger) {
                s_touch_dx += event->ctouchpad.x - s_touch_x;
                s_touch_dy += (event->ctouchpad.y - s_touch_y) * s_touch_aspect;
                s_touch_x = event->ctouchpad.x;
                s_touch_y = event->ctouchpad.y;
            }
            break;
        case SDL_CONTROLLERTOUCHPADUP:
            if (event->ctouchpad.which == s_touch_pad && event->ctouchpad.finger == s_touch_finger) {
                s_touch_pad = -1;
                s_touch_finger = -1;
            }
            break;
        case SDL_CONTROLLERBUTTONDOWN:
            if (event->cbutton.button == SDL_CONTROLLER_BUTTON_LEFTSTICK) {
                s_l3_close = true;
            }
            break;
        default:
            break;
    }
    return 1;
}

#if defined(TARGET_WEBOS)
/* A bridged pad's first touch, as the core reads it from the pad's own report: X 0 to
 * 1919, Y 0 to 1079, a DualSense's. */
#define ALIGN_BRIDGED 4
static struct {
    char node[64];
    int id;
    int x, y;   /* x -1: no touch down */
} s_bridged[ALIGN_BRIDGED];

static void bridged_touch_gather(void) {
    ctm_bridge_touch_t touches[ALIGN_BRIDGED];
    const int n = ctm_bridge_held_touches(touches, ALIGN_BRIDGED);
    for (int t = 0; t < n; ++t) {
        int k = -1, free_slot = -1;
        for (int j = 0; j < ALIGN_BRIDGED; ++j) {
            if (s_bridged[j].node[0] != '\0' && strcmp(s_bridged[j].node, touches[t].node) == 0) {
                k = j;
                break;
            }
            if (free_slot < 0 && s_bridged[j].node[0] == '\0') {
                free_slot = j;
            }
        }
        if (k < 0) {
            if (free_slot < 0) {
                continue;
            }
            k = free_slot;
            snprintf(s_bridged[k].node, sizeof s_bridged[k].node, "%s", touches[t].node);
            s_bridged[k].x = -1;
        }
        if (!touches[t].reading || touches[t].fingers < 1) {
            s_bridged[k].x = -1;
            continue;
        }
        if (s_bridged[k].x >= 0 && s_bridged[k].id == touches[t].id) {
            s_touch_dx += (float) (touches[t].x - s_bridged[k].x) / 1920.0f;
            s_touch_dy += (float) (touches[t].y - s_bridged[k].y) / 1080.0f * ALIGN_PS5_ASPECT;
        }
        s_bridged[k].id = touches[t].id;
        s_bridged[k].x = touches[t].x;
        s_bridged[k].y = touches[t].y;
    }
}
#endif

static void touch_cb(lv_timer_t *timer) {
    LV_UNUSED(timer);
    if (!s_aligning || global == NULL || global->session == NULL) {
        return;
    }
    if (s_l3_close) {
        s_l3_close = false;
        tv_cursor_log("align bar: L3, closed and kept");
        bar_close(true);
        return;
    }
#if defined(TARGET_WEBOS)
    bridged_touch_gather();
#endif
    if (s_touch_dx == 0.0f && s_touch_dy == 0.0f) {
        return;
    }
    const float dx = s_touch_dx, dy = s_touch_dy;
    s_touch_dx = s_touch_dy = 0.0f;
    touchpad_pointer_stream_move(session_get_input(global->session), dx, dy);
}

static void touch_start(void) {
    s_l3_close = false;
    s_touch_pad = -1;
    s_touch_finger = -1;
    s_touch_dx = s_touch_dy = 0.0f;
#if defined(TARGET_WEBOS)
    memset(s_bridged, 0, sizeof s_bridged);
#endif
    SDL_AddEventWatch(align_touch_watch, NULL);
    if (s_touch == NULL) {
        s_touch = lv_timer_create(touch_cb, ALIGN_TOUCH_MS, NULL);
    }
}

static void touch_stop(void) {
    SDL_DelEventWatch(align_touch_watch, NULL);
    if (s_touch != NULL) {
        lv_timer_del(s_touch);
        s_touch = NULL;
    }
}

static void bar_refresh(void) {
    if (s_title == NULL) {
        return;
    }
    int x = 0, y = 0;
    tv_cursor_get_shift(&x, &y);
    /* ⓘ Which cursor, as each keeps its own place (input/tv_cursor.h). */
    lv_label_set_text_fmt(s_title, tv_cursor_regular()
                                   ? locstr("Align the regular TV cursor with the PC's   |   right %d, down %d")
                                   : locstr("Align the sharp TV cursor with the PC's   |   right %d, down %d"),
                          x, y);
}

/* Enter or A keeps it, Esc or B puts back what was there. */
static void bar_close(bool keep) {
    if (!s_aligning) {
        return;
    }
    /* ⛔ First, while the bar still holds the input: a bridged keyboard taken
     * from webOS is let go, so the core can take it back as the hold ends
     * (input/bridge_keyboard.h). */
    bridge_keyboard_take_bridged(false);
    s_aligning = false;
    SDL_DelEventWatch(align_motion_watch, NULL);
    touch_stop();
    stop_awake();
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
    const unsigned long long watch_at = loop_watch_now();
    tv_cursor_adjust(x, y, -1);
    bar_refresh();
    keep_awake();
    loop_watch_add("the bar's tip moved", loop_watch_now() - watch_at);
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
    /* ⭐ A bridged keyboard's keys for the bar alone: webOS hides its pointer at
     * every key it reads (rhoquinn8217: "the cursor disappears whenever I
     * click an arrow key"), and the core lets the keyboard go while the bar
     * holds the input. */
    bridge_keyboard_take_bridged(true);
    s_prev_group = app_input_get_group(&global->ui.input);
    app_input_set_group(&global->ui.input, s_group);
    lv_group_focus_obj(layer);
    SDL_AddEventWatch(align_motion_watch, NULL);
    /* ⓘ With no mouse since the app started, webOS shows no pointer and SDL's is at
     * 0,0, which sent the PC's cursor to the top left (rhoquinn8217, 2026-11-14, at
     * the B4: "entering alignment mode throws the windows cursor to the top left
     * corner and there is no webOS cursor"). Then the touchpad's own cursor shows,
     * where it was last, and the host goes there. */
    if (mouse_seen_any()) {
        host_cursor_to_tip();
    } else {
        touchpad_pointer_stream_move(session_get_input(global->session), 0, 0);
    }
    touch_start();
    s_wake_said = -1;
    keep_awake();
    stop_awake();
    s_awake = lv_timer_create(awake_cb, ALIGN_AWAKE_MS, NULL);
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
        touch_stop();
        tv_cursor_adjust(s_was_x, s_was_y, -1);   /* never kept, so not kept now */
        tv_cursor_log("align bar: closed with the stream, nothing kept");
    }
    stop_awake();
    touch_stop();
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
