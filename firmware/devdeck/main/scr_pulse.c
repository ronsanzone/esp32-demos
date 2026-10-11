// SHIP PULSE — 26 weeks of GitHub contributions, plus open PRs and their check status.
//   {"t":"pulse","total":809,"streak":4,"lv":"0123...(week-major, 7 per week)","prs":[{"r":"repo#2","ti":"..","st":"pass"}]}
#include <string.h>
#include "screens.h"

#define WEEKS 26
#define CELL 14
#define GAP 3
#define HX 19
#define HY 74
#define MAXPR 2
#define NBARS 14

static const uint32_t level_col[5] = {0x161B22, 0x0E4429, 0x006D32, 0x26A641, 0x39D353};
static char levels[WEEKS * 7 + 1];
static int reveal;
static lv_obj_t *heat, *today, *total, *streak, *pr_dot[MAXPR], *pr_name[MAXPR], *pr_st[MAXPR], *pr_title[MAXPR], *none_lbl;
static lv_obj_t *bars, *bars_max;
static lv_chart_series_t *bar_ser;

static void draw_heat(lv_event_t *e)
{
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_area_t c;
    lv_obj_get_coords(heat, &c);
    lv_draw_rect_dsc_t r;
    lv_draw_rect_dsc_init(&r);
    r.radius = 3;
    int n = strlen(levels);
    for (int w = 0; w < WEEKS; w++) {
        for (int d = 0; d < 7; d++) {
            int i = w * 7 + d;
            int lv = (i < n && w < reveal) ? levels[i] - '0' : 0;
            if (i >= n) r.bg_opa = LV_OPA_30; else r.bg_opa = LV_OPA_COVER;
            r.bg_color = lv_color_hex(level_col[lv < 0 || lv > 4 ? 0 : lv]);
            lv_area_t a = {c.x1 + w * (CELL + GAP), c.y1 + d * (CELL + GAP), 0, 0};
            a.x2 = a.x1 + CELL - 1; a.y2 = a.y1 + CELL - 1;
            lv_draw_rect(layer, &r, &a);
        }
    }
}

static void set_reveal(void *o, int32_t v) { reveal = v; lv_obj_invalidate(heat); }
static void pulse_opa(void *o, int32_t v) { lv_obj_set_style_border_opa(o, (lv_opa_t)v, 0); }

static uint32_t status_col(const char *st)
{
    if (!strcmp(st, "pass")) return 0x69F0AE;
    if (!strcmp(st, "fail")) return 0xFF5252;
    if (!strcmp(st, "pending")) return 0xFFD54F;
    return COL_MUTED;
}

void pulse_create(lv_obj_t *tile)
{
    lv_obj_t *title = mk_label(tile, &lv_font_montserrat_20, COL_TEXT, "SHIP PULSE");
    lv_obj_set_style_text_letter_space(title, 3, 0);
    lv_obj_set_pos(title, 22, 18);
    lv_obj_set_pos(mk_label(tile, &lv_font_montserrat_12, COL_MUTED, "GitHub contributions, last 26 weeks"), 22, 44);
    streak = mk_label(tile, &lv_font_montserrat_14, COL_CLAUDE, "");
    lv_obj_align(streak, LV_ALIGN_TOP_RIGHT, -22, 22);

    heat = lv_obj_create(tile);
    lv_obj_remove_style_all(heat);
    lv_obj_set_size(heat, WEEKS * (CELL + GAP) - GAP, 7 * (CELL + GAP) - GAP);
    lv_obj_set_pos(heat, HX, HY);
    lv_obj_add_event_cb(heat, draw_heat, LV_EVENT_DRAW_MAIN, NULL);
    lv_obj_set_clickable(heat, false);

    today = lv_obj_create(tile);
    lv_obj_remove_style_all(today);
    lv_obj_set_size(today, CELL + 6, CELL + 6);
    lv_obj_set_style_radius(today, 5, 0);
    lv_obj_set_style_border_width(today, 2, 0);
    lv_obj_set_style_border_color(today, lv_color_white(), 0);
    lv_obj_set_hidden(today, true);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, today);
    lv_anim_set_exec_cb(&a, pulse_opa);
    lv_anim_set_values(&a, 40, 255);
    lv_anim_set_duration(&a, 900);
    lv_anim_set_reverse_duration(&a, 900);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_start(&a);

    lv_obj_t *row = lv_obj_create(tile);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 14, 0);
    lv_obj_set_pos(row, 22, 200);
    total = mk_label(row, &lv_font_montserrat_40, COL_TEXT, "--");
    mk_label(row, &lv_font_montserrat_14, COL_MUTED, "contributions\nin the last year");

    lv_obj_t *hdr = mk_label(tile, &lv_font_montserrat_12, COL_MUTED, "OPEN PULL REQUESTS");
    lv_obj_set_style_text_letter_space(hdr, 2, 0);
    lv_obj_set_pos(hdr, 22, 262);
    none_lbl = mk_label(tile, &lv_font_montserrat_14, COL_DIM, "none open");
    lv_obj_set_pos(none_lbl, 22, 284);
    for (int i = 0; i < MAXPR; i++) {
        int y = 284 + i * 40;
        pr_dot[i] = mk_dot(tile, 10, COL_MUTED);
        lv_obj_set_pos(pr_dot[i], 24, y + 4);
        pr_name[i] = mk_label(tile, &lv_font_montserrat_14, COL_TEXT, "");
        lv_obj_set_pos(pr_name[i], 44, y);
        pr_st[i] = mk_label(tile, &lv_font_montserrat_12, COL_MUTED, "");
        lv_obj_align(pr_st[i], LV_ALIGN_TOP_RIGHT, -22, y + 2);
        pr_title[i] = mk_label(tile, &lv_font_montserrat_12, COL_MUTED, "");
        lv_label_set_long_mode(pr_title[i], LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_width(pr_title[i], 410);
        lv_obj_set_pos(pr_title[i], 44, y + 18);
    }

    lv_obj_t *bh = mk_label(tile, &lv_font_montserrat_12, COL_MUTED, "LAST 14 DAYS");
    lv_obj_set_style_text_letter_space(bh, 2, 0);
    lv_obj_set_pos(bh, 22, 368);
    bars_max = mk_label(tile, &lv_font_montserrat_12, COL_DIM, "");
    lv_obj_align(bars_max, LV_ALIGN_TOP_RIGHT, -22, 368);
    bars = lv_chart_create(tile);
    lv_obj_set_size(bars, 436, 60);
    lv_obj_set_pos(bars, 22, 388);
    lv_chart_set_type(bars, LV_CHART_TYPE_BAR);
    lv_chart_set_point_count(bars, NBARS);
    lv_chart_set_div_line_count(bars, 0, 0);
    lv_obj_set_style_bg_opa(bars, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(bars, 0, 0);
    lv_obj_set_style_pad_all(bars, 0, 0);
    lv_obj_set_style_pad_column(bars, 6, 0);
    lv_obj_set_style_radius(bars, 3, LV_PART_ITEMS);
    lv_obj_set_clickable(bars, false);
    bar_ser = lv_chart_add_series(bars, lv_color_hex(0x39D353), LV_CHART_AXIS_PRIMARY_Y);
}

void pulse_on(const char *topic, const cJSON *msg)
{
    if (strcmp(topic, "pulse")) return;
    strlcpy(levels, json_str(msg, "lv", ""), sizeof levels);
    lv_label_set_text_fmt(total, "%d", json_int(msg, "total", 0));
    lv_label_set_text_fmt(streak, "%d-day streak", json_int(msg, "streak", 0));

    int n = strlen(levels);
    if (n) {
        int w = (n - 1) / 7, d = (n - 1) % 7;
        lv_obj_set_pos(today, HX + w * (CELL + GAP) - 3, HY + d * (CELL + GAP) - 3);
        lv_obj_set_hidden(today, false);
    }
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, heat);
    lv_anim_set_exec_cb(&a, set_reveal);
    lv_anim_set_values(&a, 0, WEEKS);
    lv_anim_set_duration(&a, 1400);
    lv_anim_start(&a);

    const cJSON *d14 = cJSON_GetObjectItem(msg, "d14");
    int32_t v[NBARS] = {0}, mx = 1;
    for (int i = 0; i < NBARS; i++) {
        const cJSON *x = cJSON_GetArrayItem(d14, i);
        v[i] = x ? x->valueint : 0;
        if (v[i] > mx) mx = v[i];
    }
    lv_chart_set_axis_range(bars, LV_CHART_AXIS_PRIMARY_Y, 0, mx);
    lv_chart_set_series_values(bars, bar_ser, v, NBARS);
    lv_label_set_text_fmt(bars_max, "peak %d / day", (int)mx);

    const cJSON *prs = cJSON_GetObjectItem(msg, "prs");
    int np = cJSON_GetArraySize(prs);
    lv_obj_set_hidden(none_lbl, np > 0);
    for (int i = 0; i < MAXPR; i++) {
        const cJSON *p = i < np ? cJSON_GetArrayItem(prs, i) : NULL;
        lv_obj_set_hidden(pr_dot[i], !p);
        lv_obj_set_hidden(pr_name[i], !p);
        lv_obj_set_hidden(pr_title[i], !p);
        lv_obj_set_hidden(pr_st[i], !p);
        if (!p) continue;
        lv_obj_set_style_bg_color(pr_dot[i], lv_color_hex(status_col(json_str(p, "st", ""))), 0);
        lv_label_set_text(pr_name[i], json_str(p, "r", ""));
        lv_label_set_text(pr_st[i], json_str(p, "st", ""));
        lv_obj_set_style_text_color(pr_st[i], lv_color_hex(status_col(json_str(p, "st", ""))), 0);
        lv_label_set_text(pr_title[i], json_str(p, "ti", ""));
    }
}
