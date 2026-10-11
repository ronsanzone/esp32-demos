// AGENT ORBIT — live Claude Code sessions as planets; every tool call is a spark into the core.
//   {"t":"agents","s":[{"p":"esp","n":12,"live":1}, ...]}   session roster (slot order is stable)
//   {"t":"tool","s":0,"n":"Bash","d":"idf.py build"}          one tool call from session slot s
#include <math.h>
#include <string.h>
#include "screens.h"

#define CX 240
#define CY 236
#define MAX_PLANETS 6
#define MAX_SPARKS 14
#define CORE 76

static const int ring_r[3] = {92, 136, 178};
static const uint32_t planet_cols[MAX_PLANETS] = {0xFFB74D, 0x4DD0E1, 0xBA68C8, 0x81C784, 0xF06292, 0x90A4AE};

typedef struct {
    lv_obj_t *body, *halo, *name;
    float angle, speed;
    int size;
    bool used;
} planet_t;

typedef struct {
    lv_obj_t *obj;
    int x0, y0, cx, cy;
    uint32_t color;
    bool busy;
} spark_t;

static lv_obj_t *core, *core_halo, *rate_lbl;
static lv_obj_t *ticker[3];
static planet_t planets[MAX_PLANETS];
static spark_t sparks[MAX_SPARKS];

static void planet_xy(const planet_t *p, int i, int *x, int *y)
{
    int r = ring_r[i % 3];
    *x = CX + (int)(r * cosf(p->angle));
    *y = CY + (int)(r * sinf(p->angle));
}

// Centre a dot on (x,y). Uses the requested style size: get_width() lags until the next layout pass.
static void place(lv_obj_t *o, int x, int y)
{
    int w = lv_obj_get_style_width(o, 0), h = lv_obj_get_style_height(o, 0);
    lv_obj_set_pos(o, x - w / 2, y - h / 2);
}

// Static backdrop: orbit rings, a scatter of stars, and the spokes of the core.
static void draw_backdrop(lv_event_t *e)
{
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_draw_arc_dsc_t arc;
    lv_draw_arc_dsc_init(&arc);
    arc.center.x = CX; arc.center.y = CY;
    arc.start_angle = 0; arc.end_angle = 360;
    arc.width = 1;
    arc.color = lv_color_hex(0x2A3340);
    for (int i = 0; i < 3; i++) { arc.radius = ring_r[i]; lv_draw_arc(layer, &arc); }

    lv_draw_rect_dsc_t star;
    lv_draw_rect_dsc_init(&star);
    star.bg_color = lv_color_white();
    uint32_t seed = 0x9E3779B9;
    for (int i = 0; i < 70; i++) {
        seed = seed * 1664525 + 1013904223;
        int x = seed % 480, y = (seed >> 9) % 480, s = 1 + ((seed >> 20) % 3 == 0);
        star.bg_opa = 40 + (seed >> 24) % 120;
        lv_area_t a = {x, y, x + s - 1, y + s - 1};
        lv_draw_rect(layer, &star, &a);
    }
}

// Claude-style starburst: tapered rays drawn over the core.
static void draw_starburst(lv_event_t *e)
{
    lv_obj_t *o = lv_event_get_target(e);
    lv_area_t c;
    lv_obj_get_coords(o, &c);
    int cx = (c.x1 + c.x2) / 2, cy = (c.y1 + c.y2) / 2;
    lv_draw_line_dsc_t ln;
    lv_draw_line_dsc_init(&ln);
    ln.color = lv_color_hex(0x2A140C);
    ln.round_start = ln.round_end = 1;
    for (int i = 0; i < 12; i++) {
        float a = i * (float)M_PI / 6, r = i % 2 ? 17 : 25;
        ln.width = i % 2 ? 3 : 4;
        ln.p1.x = cx; ln.p1.y = cy;
        ln.p2.x = cx + (int)(r * cosf(a)); ln.p2.y = cy + (int)(r * sinf(a));
        lv_draw_line(lv_event_get_layer(e), &ln);
    }
}

static void tick(lv_timer_t *t)
{
    for (int i = 0; i < MAX_PLANETS; i++) {
        planet_t *p = &planets[i];
        if (!p->used) continue;
        p->angle += p->speed;
        if (p->angle > 2 * (float)M_PI) p->angle -= 2 * (float)M_PI;
        int x, y;
        planet_xy(p, i, &x, &y);
        place(p->halo, x, y);
        place(p->body, x, y);
        lv_obj_set_pos(p->name, x - lv_obj_get_width(p->name) / 2, y + p->size / 2 + 4);
    }
}

static void core_size(void *o, int32_t v) { lv_obj_set_size(o, v, v); place(o, CX, CY); }

static void pulse_core(uint32_t color)
{
    lv_obj_set_style_bg_color(core_halo, lv_color_hex(color), 0);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, core_halo);
    lv_anim_set_exec_cb(&a, core_size);
    lv_anim_set_values(&a, CORE + 40, CORE + 12);
    lv_anim_set_duration(&a, 450);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
}

// Spark follows a quadratic Bezier from planet to core, bowing sideways.
static void spark_step(void *var, int32_t v)
{
    spark_t *s = var;
    float t = v / 1000.0f, u = 1 - t;
    int x = (int)(u * u * s->x0 + 2 * u * t * s->cx + t * t * CX);
    int y = (int)(u * u * s->y0 + 2 * u * t * s->cy + t * t * CY);
    place(s->obj, x, y);
    lv_obj_set_style_opa(s->obj, v < 850 ? LV_OPA_COVER : (lv_opa_t)((1000 - v) * 255 / 150), 0);
}

static void spark_done(lv_anim_t *a)
{
    spark_t *s = a->var;
    s->busy = false;
    lv_obj_set_hidden(s->obj, true);
    pulse_core(s->color);
}

static void fire(int slot, uint32_t color)
{
    if (slot < 0 || slot >= MAX_PLANETS || !planets[slot].used) return;
    spark_t *s = NULL;
    for (int i = 0; i < MAX_SPARKS && !s; i++) if (!sparks[i].busy) s = &sparks[i];
    if (!s) return;
    planet_xy(&planets[slot], slot, &s->x0, &s->y0);
    // control point: midpoint pushed perpendicular to the planet→core line
    int mx = (s->x0 + CX) / 2, my = (s->y0 + CY) / 2, dx = CX - s->x0, dy = CY - s->y0;
    s->cx = mx - dy / 3; s->cy = my + dx / 3;
    s->color = color; s->busy = true;
    lv_obj_set_style_bg_color(s->obj, lv_color_hex(color), 0);
    lv_obj_set_style_shadow_color(s->obj, lv_color_hex(color), 0);
    lv_obj_set_hidden(s->obj, false);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s);
    lv_anim_set_exec_cb(&a, spark_step);
    lv_anim_set_values(&a, 0, 1000);
    lv_anim_set_duration(&a, 900);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in);
    lv_anim_set_completed_cb(&a, spark_done);
    lv_anim_start(&a);
}

static void push_ticker(const char *tool, const char *detail, uint32_t color)
{
    for (int i = 0; i < 2; i++) {
        lv_label_set_text(ticker[i], lv_label_get_text(ticker[i + 1]));
        lv_obj_set_style_text_color(ticker[i], lv_obj_get_style_text_color(ticker[i + 1], 0), 0);
    }
    lv_label_set_text_fmt(ticker[2], "%s  %s", tool, detail);
    lv_obj_set_style_text_color(ticker[2], lv_color_hex(color), 0);
}

void orbit_create(lv_obj_t *tile)
{
    lv_obj_add_event_cb(tile, draw_backdrop, LV_EVENT_DRAW_MAIN_BEGIN, NULL);

    lv_obj_t *title = mk_label(tile, &lv_font_montserrat_20, COL_TEXT, "AGENT ORBIT");
    lv_obj_set_style_text_letter_space(title, 3, 0);
    lv_obj_set_pos(title, 22, 18);
    lv_obj_t *sub = mk_label(tile, &lv_font_montserrat_12, COL_MUTED, "live Claude Code sessions");
    lv_obj_set_pos(sub, 22, 44);
    rate_lbl = mk_label(tile, &lv_font_montserrat_14, COL_CLAUDE, "0 calls / min");
    lv_obj_align(rate_lbl, LV_ALIGN_TOP_RIGHT, -22, 22);

    // legend
    static const char *names[] = {"bash", "edit", "read", "agent", "web", "skill"};
    static const char *tools[] = {"Bash", "Edit", "Read", "Agent", "WebFetch", "Skill"};
    lv_obj_t *leg = lv_obj_create(tile);
    lv_obj_remove_style_all(leg);
    lv_obj_set_size(leg, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(leg, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(leg, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(leg, 5, 0);
    lv_obj_align(leg, LV_ALIGN_TOP_RIGHT, -22, 44);
    for (int i = 0; i < 6; i++) {
        mk_dot(leg, 6, tool_color(tools[i]));
        mk_label(leg, &lv_font_montserrat_12, COL_MUTED, names[i]);
    }

    core_halo = mk_dot(tile, CORE + 12, COL_CLAUDE);
    lv_obj_set_style_bg_opa(core_halo, LV_OPA_20, 0);
    place(core_halo, CX, CY);
    core = mk_dot(tile, CORE, COL_CLAUDE);
    lv_obj_set_style_shadow_width(core, 30, 0);
    lv_obj_set_style_shadow_color(core, lv_color_hex(COL_CLAUDE), 0);
    lv_obj_set_style_shadow_opa(core, LV_OPA_60, 0);
    place(core, CX, CY);
    lv_obj_add_event_cb(core, draw_starburst, LV_EVENT_DRAW_MAIN_END, NULL);

    for (int i = 0; i < MAX_PLANETS; i++) {
        planet_t *p = &planets[i];
        p->angle = i * 2.4f;                 // golden-ish spread so planets never stack
        p->speed = 0.012f - 0.003f * (i % 3); // outer rings drift slower
        p->halo = mk_dot(tile, 10, planet_cols[i]);
        lv_obj_set_style_bg_opa(p->halo, LV_OPA_20, 0);
        p->body = mk_dot(tile, 10, planet_cols[i]);
        p->name = mk_label(tile, &lv_font_montserrat_12, planet_cols[i], "");
        lv_obj_set_hidden(p->halo, true);
        lv_obj_set_hidden(p->body, true);
        lv_obj_set_hidden(p->name, true);
    }
    for (int i = 0; i < MAX_SPARKS; i++) {
        sparks[i].obj = mk_dot(tile, 9, 0xFFFFFF);
        lv_obj_set_style_shadow_width(sparks[i].obj, 12, 0);
        lv_obj_set_style_shadow_opa(sparks[i].obj, LV_OPA_80, 0);
        lv_obj_set_hidden(sparks[i].obj, true);
    }
    for (int i = 0; i < 3; i++) {
        ticker[i] = mk_label(tile, &lv_font_montserrat_14, COL_MUTED, "");
        lv_label_set_long_mode(ticker[i], LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_width(ticker[i], 436);
        lv_obj_set_pos(ticker[i], 22, 410 + i * 18);
        lv_obj_set_style_opa(ticker[i], (lv_opa_t)(90 + i * 80), 0);
    }
    lv_label_set_text(ticker[2], "waiting for the bridge...");
    lv_timer_create(tick, 40, NULL);
}

void orbit_on(const char *topic, const cJSON *msg)
{
    if (!strcmp(topic, "agents")) {
        const cJSON *arr = cJSON_GetObjectItem(msg, "s");
        int n = cJSON_GetArraySize(arr);
        lv_label_set_text_fmt(rate_lbl, "%d calls / min", json_int(msg, "r", 0));
        if (!strcmp(lv_label_get_text(ticker[2]), "waiting for the bridge...")) lv_label_set_text(ticker[2], "");
        for (int i = 0; i < MAX_PLANETS; i++) {
            planet_t *p = &planets[i];
            const cJSON *s = i < n ? cJSON_GetArrayItem(arr, i) : NULL;
            p->used = s != NULL;
            lv_obj_t *objs[] = {p->halo, p->body, p->name};
            for (int k = 0; k < 3; k++) {
                lv_obj_set_hidden(objs[k], !p->used);
            }
            if (!s) continue;
            int calls = json_int(s, "n", 0), live = json_int(s, "live", 0);
            p->size = 14 + (calls > 60 ? 60 : calls) / 3;
            lv_obj_set_size(p->body, p->size, p->size);
            lv_obj_set_size(p->halo, p->size + (live ? 16 : 6), p->size + (live ? 16 : 6));
            lv_obj_set_style_opa(p->body, live ? LV_OPA_COVER : LV_OPA_50, 0);
            lv_obj_set_style_opa(p->name, live ? LV_OPA_COVER : LV_OPA_50, 0);
            lv_label_set_text(p->name, json_str(s, "p", "?"));
        }
    } else if (!strcmp(topic, "tool")) {
        const char *tool = json_str(msg, "n", "?");
        uint32_t c = tool_color(tool);
        fire(json_int(msg, "s", -1), c);
        push_ticker(tool, json_str(msg, "d", ""), c);
    }
}
