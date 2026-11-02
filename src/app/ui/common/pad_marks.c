/* The marks of a controller's buttons. The why is in the header. */

#include "pad_marks.h"

#include "app.h"
#include "input/app_input.h"
#include "input/input_gamepad.h"

/* The page's grid in hundredths of a unit: 20 high, 20 wide, a shoulder button 24. */
#define GRID_HIGH 2000
#define GRID_WIDE 2000
#define GRID_WIDE_SHOULDER 2400

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

static bool mark_is_dpad(pad_mark_t mark) {
    return mark == PAD_MARK_UP || mark == PAD_MARK_DOWN || mark == PAD_MARK_LEFT || mark == PAD_MARK_RIGHT ||
           mark == PAD_MARK_DPAD;
}

static void mark_draw(lv_event_t *event) {
    lv_obj_t *obj = lv_event_get_target(event);
    const intptr_t packed = (intptr_t) lv_event_get_user_data(event);
    const pad_mark_t mark = (pad_mark_t) (packed >> 8);
    const pad_family_t family = (pad_family_t) (packed & 0xff);

    pen_t pen;
    pen.ctx = lv_event_get_draw_ctx(event);
    lv_obj_get_coords(obj, &pen.box);
    pen.across = mark_is_shoulder(mark) ? GRID_WIDE_SHOULDER : GRID_WIDE;
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
    if (mark_is_dpad(mark)) {
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
    lv_obj_set_size(obj, mark_is_shoulder(mark) ? height * GRID_WIDE_SHOULDER / GRID_HIGH : height, height);
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
