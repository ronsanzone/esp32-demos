// CONTEXT — how full each live session's context window is. Tap to cycle sessions.
//   {"t":"ctx","focus":0,"s":[{"p":"devdeck","u":141281,"max":200000,"cr":139000,"m":"opus-5-5","h":[...],"cmp":0}]}
#include <string.h>
#include "screens.h"

#define MAXS 6
#define NH 32
#define CX 240
#define CY 214

typedef struct {
    char p[32], m[32];
    int u, max, cr, cmp, nh;
    int32_t h[NH];
} sess_t;

static sess_t ss[MAXS];
static int n_s, sel, shown;
static uint32_t tapped_at;
static lv_obj_t *arc, *cache_arc, *num, *of_lbl, *pct, *proj, *model, *cache_lbl, *cmp_lbl, *idx_lbl, *chart;
static lv_chart_series_t *ser;

static uint32_t fill_color(int permille)
{
    if (permille < 500) return 0x69F0AE;
    if (permille < 750) return 0xFFD54F;
    if (permille < 900) return 0xFF8A65;
    return 0xFF5252;
}

static void show_tokens(void *unused, int32_t v)
{
    const sess_t *s = &ss[sel];
    shown = v;
    int pm = s->max ? (int)((int64_t)v * 1000 / s->max) : 0;
    if (pm > 1000) pm = 1000;
    uint32_t c = fill_color(pm);
    lv_arc_set_value(arc, pm);
    lv_obj_set_style_arc_color(arc, lv_color_hex(c), LV_PART_INDICATOR);
    lv_obj_set_style_shadow_color(arc, lv_color_hex(c), LV_PART_INDICATOR);
    lv_label_set_text_fmt(num, "%dK", (int)(v / 1000));
    lv_label_set_text_fmt(pct, "%d%%", pm / 10);
    lv_obj_set_style_text_color(pct, lv_color_hex(c), 0);
    lv_chart_set_series_color(chart, ser, lv_color_hex(c));
}

static void render(void)
{
    if (!n_s) return;
    const sess_t *s = &ss[sel];
    lv_label_set_text(proj, s->p);
    lv_label_set_text(model, s->m);
    lv_label_set_text_fmt(of_lbl, "of %s tokens", s->max >= 1000000 ? "1M" : "200K");
    int hit = s->u ? (int)((int64_t)s->cr * 100 / s->u) : 0;
    lv_arc_set_value(cache_arc, hit);
    lv_label_set_text_fmt(cache_lbl, "cache hit %d%%", hit);
    lv_label_set_text_fmt(cmp_lbl, s->cmp == 1 ? "1 compaction" : "%d compactions", s->cmp);
    lv_label_set_text_fmt(idx_lbl, "%d / %d", sel + 1, n_s);

    lv_chart_set_point_count(chart, s->nh > 1 ? s->nh : 2);
    int lo = s->max, hi = 0;                      // autoscale so growth and compactions read clearly
    for (int i = 0; i < s->nh; i++) { if (s->h[i] < lo) lo = s->h[i]; if (s->h[i] > hi) hi = s->h[i]; }
    int pad = (hi - lo) / 8 + 1000;
    lv_chart_set_axis_range(chart, LV_CHART_AXIS_PRIMARY_Y, lo > pad ? lo - pad : 0, hi + pad);
    lv_chart_set_series_values(chart, ser, s->h, s->nh);

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_exec_cb(&a, show_tokens);
    lv_anim_set_var(&a, NULL);
    lv_anim_set_values(&a, shown, s->u);
    lv_anim_set_duration(&a, 900);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
}

static void on_tap(lv_event_t *e)
{
    if (!n_s) return;
    sel = (sel + 1) % n_s;
    shown = 0;
    tapped_at = lv_tick_get();
    render();
}

static lv_obj_t *mk_arc(lv_obj_t *parent, int d, int w, uint32_t bg, int range)
{
    lv_obj_t *a = lv_arc_create(parent);
    lv_obj_set_size(a, d, d);
    lv_obj_set_pos(a, CX - d / 2, CY - d / 2);
    lv_arc_set_bg_angles(a, 135, 45);
    lv_arc_set_range(a, 0, range);
    lv_arc_set_value(a, 0);
    lv_obj_remove_style(a, NULL, LV_PART_KNOB);
    lv_obj_set_clickable(a, false);
    lv_obj_set_style_arc_width(a, w, LV_PART_MAIN);
    lv_obj_set_style_arc_width(a, w, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(a, lv_color_hex(bg), LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(a, true, LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(a, true, LV_PART_INDICATOR);
    return a;
}

static lv_obj_t *centered(lv_obj_t *parent, const lv_font_t *f, uint32_t col, int dy)
{
    lv_obj_t *l = mk_label(parent, f, col, "");
    lv_obj_set_width(l, 260);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(l, LV_ALIGN_TOP_MID, 0, CY + dy);
    return l;
}

void context_create(lv_obj_t *tile)
{
    lv_obj_set_clickable(tile, true);
    lv_obj_add_event_cb(tile, on_tap, LV_EVENT_SHORT_CLICKED, NULL);

    lv_obj_t *title = mk_label(tile, &lv_font_montserrat_20, COL_TEXT, "CONTEXT");
    lv_obj_set_style_text_letter_space(title, 3, 0);
    lv_obj_set_pos(title, 22, 18);
    lv_obj_set_pos(mk_label(tile, &lv_font_montserrat_12, COL_MUTED, "our most important commodity"), 22, 44);
    idx_lbl = mk_label(tile, &lv_font_montserrat_14, COL_MUTED, "");
    lv_obj_align(idx_lbl, LV_ALIGN_TOP_RIGHT, -22, 22);
    lv_obj_t *hint = mk_label(tile, &lv_font_montserrat_12, COL_DIM, "tap to switch");
    lv_obj_align(hint, LV_ALIGN_TOP_RIGHT, -22, 44);

    arc = mk_arc(tile, 320, 22, 0x1C2128, 1000);
    lv_obj_set_style_shadow_width(arc, 18, LV_PART_INDICATOR);
    lv_obj_set_style_shadow_opa(arc, LV_OPA_50, LV_PART_INDICATOR);
    cache_arc = mk_arc(tile, 266, 5, 0x141A20, 100);
    lv_obj_set_style_arc_color(cache_arc, lv_color_hex(0x4FC3F7), LV_PART_INDICATOR);

    pct = centered(tile, &lv_font_montserrat_28, COL_TEXT, -88);
    num = centered(tile, &lv_font_montserrat_48, COL_TEXT, -46);
    of_lbl = centered(tile, &lv_font_montserrat_14, COL_MUTED, 8);
    proj = centered(tile, &lv_font_montserrat_20, COL_CLAUDE, 40);
    model = centered(tile, &lv_font_montserrat_12, COL_MUTED, 66);
    cache_lbl = centered(tile, &lv_font_montserrat_12, 0x4FC3F7, 128);
    cmp_lbl = centered(tile, &lv_font_montserrat_12, COL_MUTED, 146);
    lv_label_set_text(num, "--");

    chart = lv_chart_create(tile);
    lv_obj_set_size(chart, 436, 50);
    lv_obj_set_pos(chart, 22, 400);
    lv_chart_set_type(chart, LV_CHART_TYPE_LINE);
    lv_chart_set_div_line_count(chart, 0, 0);
    lv_obj_set_style_bg_opa(chart, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(chart, 0, 0);
    lv_obj_set_style_pad_all(chart, 0, 0);
    lv_obj_set_style_line_width(chart, 2, LV_PART_ITEMS);
    lv_obj_set_style_width(chart, 0, LV_PART_INDICATOR);
    lv_obj_set_style_height(chart, 0, LV_PART_INDICATOR);
    lv_obj_set_clickable(chart, false);
    ser = lv_chart_add_series(chart, lv_color_hex(0x69F0AE), LV_CHART_AXIS_PRIMARY_Y);
    lv_obj_t *cap = mk_label(tile, &lv_font_montserrat_12, COL_DIM, "context per turn");
    lv_obj_set_pos(cap, 22, 384);
}

void context_on(const char *topic, const cJSON *msg)
{
    if (strcmp(topic, "ctx")) return;
    const cJSON *arr = cJSON_GetObjectItem(msg, "s");
    n_s = cJSON_GetArraySize(arr);
    if (n_s > MAXS) n_s = MAXS;
    for (int i = 0; i < n_s; i++) {
        const cJSON *o = cJSON_GetArrayItem(arr, i);
        sess_t *s = &ss[i];
        strlcpy(s->p, json_str(o, "p", "?"), sizeof s->p);
        strlcpy(s->m, json_str(o, "m", ""), sizeof s->m);
        s->u = json_int(o, "u", 0);
        s->max = json_int(o, "max", 200000);
        s->cr = json_int(o, "cr", 0);
        s->cmp = json_int(o, "cmp", 0);
        const cJSON *h = cJSON_GetObjectItem(o, "h");
        s->nh = 0;
        for (const cJSON *v = h ? h->child : NULL; v && s->nh < NH; v = v->next) s->h[s->nh++] = v->valueint;
    }
    if (lv_tick_get() - tapped_at > 60000 || !tapped_at) sel = json_int(msg, "focus", 0);
    if (sel >= n_s) sel = 0;
    render();
}
