/* The marks of a controller's buttons. The why is in the header. */

#include "pad_marks.h"

#include "app.h"
#include "input/app_input.h"
#include "input/input_gamepad.h"
#include "input/loop_watch.h"

/* The page's grid in hundredths of a unit: 20 high, 20 wide, a shoulder button 24. */
#define GRID_HIGH 2000
#define GRID_WIDE 2000
#define GRID_WIDE_SHOULDER 2400
#define GRID_WIDE_TOUCHPAD 2800

typedef struct {
    lv_draw_ctx_t *ctx;
    lv_area_t box;
    int across;
    lv_coord_t stroke;
    lv_color_t color;
    lv_draw_line_dsc_t line;
} pen_t;

static lv_point_t pen_at(const pen_t *pen, int x, int y) {
    const lv_coord_t w = lv_area_get_width(&pen->box);
    const lv_coord_t h = lv_area_get_height(&pen->box);
    return (lv_point_t) {(lv_coord_t) (pen->box.x1 + (x * w + pen->across / 2) / pen->across),
                         (lv_coord_t) (pen->box.y1 + (y * h + GRID_HIGH / 2) / GRID_HIGH)};
}

static void pen_line(const pen_t *pen, int x1, int y1, int x2, int y2) {
    const lv_point_t from = pen_at(pen, x1, y1);
    const lv_point_t to = pen_at(pen, x2, y2);
    lv_draw_line(pen->ctx, &pen->line, &from, &to);
}

/* Through the points; `closed` back to the first. */
static void pen_path(const pen_t *pen, const int16_t (*points)[2], int count, bool closed) {
    for (int i = 0; i + 1 < count; ++i) {
        pen_line(pen, points[i][0], points[i][1], points[i + 1][0], points[i + 1][1]);
    }
    if (closed) {
        pen_line(pen, points[count - 1][0], points[count - 1][1], points[0][0], points[0][1]);
    }
}

/* A circle's outline, drawn as a rounded box's border, which the display keeps. */
static void pen_circle(const pen_t *pen, int cx, int cy, int r) {
    const lv_point_t centre = pen_at(pen, cx, cy);
    const lv_coord_t h = lv_area_get_height(&pen->box);
    /* ⓘ The page's stroke is centred on its circle; a border lies inside its box. */
    const lv_coord_t outer = (lv_coord_t) ((r * h + GRID_HIGH / 2) / GRID_HIGH + pen->stroke / 2);
    const lv_area_t area = {(lv_coord_t) (centre.x - outer), (lv_coord_t) (centre.y - outer),
                            (lv_coord_t) (centre.x + outer - 1), (lv_coord_t) (centre.y + outer - 1)};
    lv_draw_rect_dsc_t dsc;
    lv_draw_rect_dsc_init(&dsc);
    dsc.bg_opa = LV_OPA_TRANSP;
    dsc.radius = LV_RADIUS_CIRCLE;
    dsc.border_color = pen->color;
    dsc.border_width = pen->stroke;
    dsc.border_opa = LV_OPA_COVER;
    lv_draw_rect(pen->ctx, &dsc, &area);
}

/* A solid circle, `opa` of the mark's colour: a finger on the touchpad. */
static void pen_disc(const pen_t *pen, int cx, int cy, int r, lv_opa_t opa) {
    const lv_point_t centre = pen_at(pen, cx, cy);
    const lv_coord_t h = lv_area_get_height(&pen->box);
    const lv_coord_t radius = (lv_coord_t) LV_MAX(1, (r * h + GRID_HIGH / 2) / GRID_HIGH);
    const lv_area_t area = {(lv_coord_t) (centre.x - radius), (lv_coord_t) (centre.y - radius),
                            (lv_coord_t) (centre.x + radius - 1), (lv_coord_t) (centre.y + radius - 1)};
    lv_draw_rect_dsc_t dsc;
    lv_draw_rect_dsc_init(&dsc);
    dsc.bg_color = pen->color;
    dsc.bg_opa = opa;
    dsc.radius = LV_RADIUS_CIRCLE;
    lv_draw_rect(pen->ctx, &dsc, &area);
}

/* A filled box: one arm of the d-pad. */
static void pen_fill(const pen_t *pen, int x1, int y1, int x2, int y2) {
    const lv_point_t low = pen_at(pen, x1, y1);
    const lv_point_t high = pen_at(pen, x2, y2);
    const lv_area_t area = {low.x, low.y, (lv_coord_t) (high.x - 1), (lv_coord_t) (high.y - 1)};
    lv_draw_rect_dsc_t dsc;
    lv_draw_rect_dsc_init(&dsc);
    dsc.bg_color = pen->color;
    dsc.bg_opa = LV_OPA_COVER;
    lv_draw_rect(pen->ctx, &dsc, &area);
}

/* Words in the middle of the mark. */
static void pen_words(const pen_t *pen, const lv_font_t *font, const char *words) {
    lv_draw_label_dsc_t dsc;
    lv_draw_label_dsc_init(&dsc);
    dsc.color = pen->color;
    dsc.font = font;
    dsc.align = LV_TEXT_ALIGN_CENTER;
    const lv_coord_t line = lv_font_get_line_height(font);
    lv_area_t area = pen->box;
    area.y1 = (lv_coord_t) (pen->box.y1 + (lv_area_get_height(&pen->box) - line) / 2);
    area.y2 = (lv_coord_t) (area.y1 + line - 1);
    lv_draw_label(pen->ctx, &dsc, &area, words, NULL);
}

static const int16_t CROSS_A[2][2] = {{660, 660}, {1340, 1340}};
static const int16_t CROSS_B[2][2] = {{1340, 660}, {660, 1340}};
static const int16_t SQUARE[4][2] = {{660, 660}, {1340, 660}, {1340, 1340}, {660, 1340}};
static const int16_t TRIANGLE[3][2] = {{1000, 520}, {1430, 1260}, {570, 1260}};
static const int16_t DPAD[12][2] = {{760, 320}, {1240, 320}, {1240, 760}, {1680, 760}, {1680, 1240}, {1240, 1240},
                                    {1240, 1680}, {760, 1680}, {760, 1240}, {320, 1240}, {320, 760}, {760, 760}};
static const int16_t VIEW_BACK[5][2] = {{800, 870}, {800, 610}, {1440, 610}, {1440, 1130}, {1200, 1130}};
static const int16_t VIEW_FRONT[4][2] = {{560, 870}, {1200, 870}, {1200, 1390}, {560, 1390}};
/* A shoulder button, on its 24-wide grid: square at the foot, its top corners on a
 * radius of 6. ⓘ In short straight strokes: the first pictures on the monitor, drawn
 * with LVGL's arc, showed a step where each corner met the top line. */
static const int16_t SHOULDER[12][2] = {{110, 1890}, {110, 710}, {156, 480}, {286, 286}, {480, 156}, {710, 110},
                                        {1690, 110}, {1920, 156}, {2114, 286}, {2244, 480}, {2290, 710}, {2290, 1890}};
/* The PS button's two letters, drawn in strokes as the shapes are: the app's small type
 * is too large for two letters inside the circle at a legend's height. Each 800 high,
 * the pair centred: P's stem and its bowl, a half ellipse; S as two half ellipses. */
static const int16_t LETTER_P[10][2] = {{560, 1400}, {560, 600}, {780, 600}, {885, 628}, {962, 705},
                                        {990, 810}, {962, 915}, {885, 992}, {780, 1020}, {560, 1020}};
static const int16_t LETTER_S[17][2] = {{1425, 700}, {1355, 627}, {1260, 600}, {1165, 627}, {1095, 700},
                                        {1070, 800}, {1095, 900}, {1165, 973}, {1260, 1000}, {1355, 1027},
                                        {1425, 1100}, {1450, 1200}, {1425, 1300}, {1355, 1373}, {1260, 1400},
                                        {1165, 1373}, {1095, 1300}};

/* L3's L, and its 3: S's lower bowl under its upper one turned round. As P and S, each
 * 800 high, the pair centred; an Xbox pad's LS is the same L with the S above. */
static const int16_t LETTER_L[3][2] = {{560, 600}, {560, 1400}, {960, 1400}};
static const int16_t DIGIT_3[17][2] = {{1095, 700}, {1165, 627}, {1260, 600}, {1355, 627}, {1425, 700},
                                       {1450, 800}, {1425, 900}, {1355, 973}, {1260, 1000}, {1355, 1027},
                                       {1425, 1100}, {1450, 1200}, {1425, 1300}, {1355, 1373}, {1260, 1400},
                                       {1165, 1373}, {1095, 1300}};

/* The touchpad, on its 28-wide grid: the approved outline's path (M4.25 2.69, its top
 * an arc of radius 60.3 whose middle is at 1.9, its four corners of radius 2.7, the
 * bottom straight at 17) with each arc in short straight strokes, as the shoulder
 * button's corners are. */
static const int16_t TOUCHPAD[27][2] = {{425, 269}, {667, 234}, {911, 210}, {1155, 195}, {1400, 190}, {1645, 195},
                                        {1889, 210}, {2133, 234}, {2375, 269}, {2474, 306}, {2550, 378}, {2594, 475},
                                        {2597, 580}, {2444, 1475}, {2412, 1564}, {2352, 1636}, {2271, 1683},
                                        {2178, 1700}, {622, 1700}, {529, 1683}, {448, 1636}, {388, 1564},
                                        {356, 1475}, {203, 580}, {206, 475}, {250, 378}, {326, 306}};

static void draw_touchpad(const pen_t *pen, pad_mark_t mark) {
    pen_path(pen, TOUCHPAD, 27, true);
    if (mark == PAD_MARK_TOUCH_ONE) {
        pen_disc(pen, 1400, 990, 340, LV_OPA_40);
    } else if (mark == PAD_MARK_TOUCH_TWO) {
        /* The right one half a unit higher, as the approved picture has it. */
        pen_disc(pen, 1040, 1010, 289, LV_OPA_40);
        pen_disc(pen, 1760, 960, 289, LV_OPA_40);
    }
}

/* A stick tilted: the stick's outline, an inner ring, and its letter in strokes, as the
 * PS button's are (the app's small type is too large for a letter inside the ring). */
static const int16_t LETTER_STICK_L[3][2] = {{860, 660}, {860, 1340}, {1150, 1340}};
static const int16_t LETTER_STICK_R[10][2] = {{860, 1340}, {860, 660}, {1030, 660}, {1120, 695}, {1160, 775},
                                              {1120, 855}, {1030, 890}, {860, 890}, {1030, 890}, {1180, 1340}};

static void draw_stick(const pen_t *pen, pad_mark_t mark) {
    pen_circle(pen, 1000, 1000, 900);
    pen_circle(pen, 1000, 1000, 650);
    if (mark == PAD_MARK_STICK_R) {
        pen_path(pen, LETTER_STICK_R, 10, false);
    } else {
        pen_path(pen, LETTER_STICK_L, 3, false);
    }
}

/* A stick pressed: a solid flat arrow, pointing down, over an outline of the stick's
 * cap. The arrow's head is filled in short horizontal strokes, as the display has no
 * filled triangle. */
static const int16_t STICK_CAP[10][2] = {{400, 840}, {1600, 840}, {1860, 1060}, {1860, 1240}, {1600, 1240},
                                         {1600, 1860}, {400, 1860}, {400, 1240}, {140, 1240}, {140, 1060}};

static void draw_stick_press(const pen_t *pen) {
    pen_fill(pen, 880, 220, 1120, 360);
    for (int y = 360; y <= 690; y += 40) {
        const int half = (690 - y) * 460 / 330;
        pen_line(pen, 1000 - half, y, 1000 + half, y);
    }
    pen_path(pen, STICK_CAP, 10, true);
}

static void draw_dpad(const pen_t *pen, pad_mark_t mark) {
    pen_path(pen, DPAD, 12, true);
    switch (mark) {
        case PAD_MARK_UP:
            pen_fill(pen, 760, 320, 1240, 760);
            break;
        case PAD_MARK_DOWN:
            pen_fill(pen, 760, 1240, 1240, 1680);
            break;
        case PAD_MARK_LEFT:
            pen_fill(pen, 320, 760, 760, 1240);
            break;
        case PAD_MARK_RIGHT:
            pen_fill(pen, 1240, 760, 1680, 1240);
            break;
        default:
            break;
    }
}

static void draw_shoulder(const pen_t *pen, const lv_font_t *font, const char *name) {
    pen_path(pen, SHOULDER, 12, true);
    pen_words(pen, font, name);
}

/* What goes inside a round button's circle. */
static void draw_round(const pen_t *pen, const lv_font_t *font, pad_mark_t mark, pad_family_t family) {
    pen_circle(pen, 1000, 1000, 900);
    if (family == PAD_FAMILY_XBOX) {
        switch (mark) {
            case PAD_MARK_SOUTH:
                pen_words(pen, font, "A");
                return;
            case PAD_MARK_EAST:
                pen_words(pen, font, "B");
                return;
            case PAD_MARK_WEST:
                pen_words(pen, font, "X");
                return;
            case PAD_MARK_NORTH:
                pen_words(pen, font, "Y");
                return;
            case PAD_MARK_VIEW:
                pen_path(pen, VIEW_BACK, 5, false);
                pen_path(pen, VIEW_FRONT, 4, true);
                return;
            case PAD_MARK_HOME:
                /* No mark of its own: the circle alone (the header). */
                return;
            case PAD_MARK_L3:
                pen_path(pen, LETTER_L, 3, false);
                pen_path(pen, LETTER_S, 17, false);
                return;
            default:
                break;
        }
    } else {
        switch (mark) {
            case PAD_MARK_SOUTH:
                pen_path(pen, CROSS_A, 2, false);
                pen_path(pen, CROSS_B, 2, false);
                return;
            case PAD_MARK_EAST:
                pen_circle(pen, 1000, 1000, 390);
                return;
            case PAD_MARK_WEST:
                pen_path(pen, SQUARE, 4, true);
                return;
            case PAD_MARK_NORTH:
                pen_path(pen, TRIANGLE, 3, true);
                return;
            case PAD_MARK_VIEW:
                /* Create's three strokes, the middle one standing higher. */
                pen_line(pen, 534, 770, 690, 1280);
                pen_line(pen, 1000, 480, 1000, 1160);
                pen_line(pen, 1466, 770, 1310, 1280);
                return;
            case PAD_MARK_HOME:
                pen_path(pen, LETTER_P, 10, false);
                pen_path(pen, LETTER_S, 17, false);
                return;
            case PAD_MARK_L3:
                pen_path(pen, LETTER_L, 3, false);
                pen_path(pen, DIGIT_3, 17, false);
                return;
            default:
                break;
        }
    }
    /* Options and Menu: the same three lines. */
    pen_line(pen, 640, 730, 1360, 730);
    pen_line(pen, 640, 1000, 1360, 1000);
    pen_line(pen, 640, 1270, 1360, 1270);
}

static const char *shoulder_name(pad_mark_t mark, pad_family_t family) {
    const bool xbox = family == PAD_FAMILY_XBOX;
    switch (mark) {
        case PAD_MARK_L1:
            return xbox ? "LB" : "L1";
        case PAD_MARK_R1:
            return xbox ? "RB" : "R1";
        case PAD_MARK_L2:
            return xbox ? "LT" : "L2";
        default:
            return xbox ? "RT" : "R2";
    }
}

static bool mark_is_shoulder(pad_mark_t mark) {
    return mark == PAD_MARK_L1 || mark == PAD_MARK_R1 || mark == PAD_MARK_L2 || mark == PAD_MARK_R2;
}

static bool mark_is_stick(pad_mark_t mark) {
    return mark == PAD_MARK_STICK_R || mark == PAD_MARK_STICK_L || mark == PAD_MARK_STICK_PRESS;
}

static bool mark_is_touchpad(pad_mark_t mark) {
    return mark == PAD_MARK_TOUCHPAD || mark == PAD_MARK_TOUCH_ONE || mark == PAD_MARK_TOUCH_TWO;
}

static bool mark_is_dpad(pad_mark_t mark) {
    return mark == PAD_MARK_UP || mark == PAD_MARK_DOWN || mark == PAD_MARK_LEFT || mark == PAD_MARK_RIGHT ||
           mark == PAD_MARK_DPAD;
}

static void mark_draw_body(lv_event_t *event);

/* Timed for input/loop_watch.h. */
static void mark_draw(lv_event_t *event) {
    const unsigned long long watch_at = loop_watch_now();
    mark_draw_body(event);
    loop_watch_add("the pad marks' drawing", loop_watch_now() - watch_at);
}

static void mark_draw_body(lv_event_t *event) {
    lv_obj_t *obj = lv_event_get_target(event);
    const intptr_t packed = (intptr_t) lv_event_get_user_data(event);
    const pad_mark_t mark = (pad_mark_t) (packed >> 8);
    const pad_family_t family = (pad_family_t) (packed & 0xff);

    pen_t pen;
    pen.ctx = lv_event_get_draw_ctx(event);
    lv_obj_get_coords(obj, &pen.box);
    pen.across = mark_is_touchpad(mark)   ? GRID_WIDE_TOUCHPAD
                 : mark_is_shoulder(mark) ? GRID_WIDE_SHOULDER
                                          : GRID_WIDE;
    /* The page's stroke: 1.25 of the 20, and never thinner than two pixels. */
    pen.stroke = LV_MAX(2, lv_area_get_height(&pen.box) * 125 / GRID_HIGH);
    pen.color = lv_obj_get_style_text_color(obj, LV_PART_MAIN);
    lv_draw_line_dsc_init(&pen.line);
    pen.line.color = pen.color;
    pen.line.width = pen.stroke;
    pen.line.opa = LV_OPA_COVER;
    pen.line.round_start = 1;
    pen.line.round_end = 1;

    const lv_font_t *font = lv_obj_get_style_text_font(obj, LV_PART_MAIN);
    if (mark_is_stick(mark)) {
        if (mark == PAD_MARK_STICK_PRESS) {
            draw_stick_press(&pen);
        } else {
            draw_stick(&pen, mark);
        }
    } else if (mark_is_touchpad(mark)) {
        draw_touchpad(&pen, mark);
    } else if (mark_is_dpad(mark)) {
        draw_dpad(&pen, mark);
    } else if (mark_is_shoulder(mark)) {
        draw_shoulder(&pen, font, shoulder_name(mark, family));
    } else {
        draw_round(&pen, font, mark, family);
    }
}

lv_obj_t *pad_mark_create(lv_obj_t *parent, pad_mark_t mark, pad_family_t family, lv_coord_t height,
                          lv_color_t color) {
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(obj,
                    mark_is_touchpad(mark)   ? height * GRID_WIDE_TOUCHPAD / GRID_HIGH
                    : mark_is_shoulder(mark) ? height * GRID_WIDE_SHOULDER / GRID_HIGH
                                             : height,
                    height);
    lv_obj_set_style_text_color(obj, color, 0);
    /* The small type a legend is written in: a letter in a mark at a keyboard's
     * hint height still clears the circle. */
    lv_obj_set_style_text_font(obj, lv_theme_get_font_small(parent), 0);
    const intptr_t packed = ((intptr_t) mark << 8) | (intptr_t) family;
    lv_obj_add_event_cb(obj, mark_draw, LV_EVENT_DRAW_MAIN, (void *) packed);
    return obj;
}

/* ---------------------------------------------------------- whose marks -- */

static SDL_JoystickID s_last_pad = -1;
static int s_forced = -1;

static pad_family_t family_of(SDL_GameController *controller) {
    switch (SDL_GameControllerGetType(controller)) {
        case SDL_CONTROLLER_TYPE_PS3:
        case SDL_CONTROLLER_TYPE_PS4:
        case SDL_CONTROLLER_TYPE_PS5:
            return PAD_FAMILY_PLAYSTATION;
        default:
            return PAD_FAMILY_XBOX;
    }
}

void pad_family_note(SDL_JoystickID instance_id) {
    s_last_pad = instance_id;
}

void pad_family_force(int force) {
    s_forced = force;
}

pad_family_t pad_family_current(void) {
    if (s_forced >= 0) {
        return (pad_family_t) s_forced;
    }
    if (global == NULL) {
        return PAD_FAMILY_PLAYSTATION;
    }
    app_gamepad_state_t *pad = s_last_pad >= 0 ? app_input_gamepad_state_by_instance_id(&global->input, s_last_pad)
                                               : NULL;
    if (pad != NULL && pad->controller != NULL) {
        return family_of(pad->controller);
    }
    const short max = app_input_get_max_gamepads(&global->input);
    for (short i = 0; i < max; ++i) {
        pad = app_input_gamepad_state_by_index(&global->input, i);
        if (pad != NULL && pad->controller != NULL) {
            return family_of(pad->controller);
        }
    }
    return PAD_FAMILY_PLAYSTATION;
}
