// PIPELINE — the deep-work loop, each stage lit by how often its skills actually ran.
//   {"t":"pipe","c":[8 counts],"last":2,"skill":"dw-02-research","ago":"3d ago","total":36}
#include <math.h>
#include <string.h>
#include "screens.h"

#define N 8
#define CX 240
#define CY 258
#define R 140
#define NP 14

static const char *names[N] = {"Ticket", "Questions", "Research", "Design", "Outline", "Plan", "Build", "Ship"};
static const uint32_t cols[N] = {0xF06292, 0xBA68C8, 0x9575CD, 0x7986CB, 0x4FC3F7, 0x4DD0E1, 0x4DB6AC, 0x81C784};

static lv_obj_t *node[N], *cnt[N], *halo, *total, *last_lbl, *ago_lbl;
static lv_obj_t *particles[NP];
static float p_ang[NP], p_spd[NP];

static float stage_angle(int i) { return (-90.0f + 22.5f + i * 45.0f) * (float)M_PI / 180.0f; }

static void center_on(lv_obj_t *o, int x, int y, int size) { lv_obj_set_pos(o, x - size / 2, y - size / 2); }

static void backdrop(lv_event_t *e)
{
    lv_draw_arc_dsc_t a;
    lv_draw_arc_dsc_init(&a);
    a.center.x = CX; a.center.y = CY; a.radius = R;
    a.start_angle = 0; a.end_angle = 360; a.width = 2;
    a.color = lv_color_hex(0x262C36);
    lv_draw_arc(lv_event_get_layer(e), &a);
}

static void flow(lv_timer_t *t)
{
    for (int i = 0; i < NP; i++) {
        p_ang[i] += p_spd[i];
        if (p_ang[i] > 2 * (float)M_PI) p_ang[i] -= 2 * (float)M_PI;
        center_on(particles[i], CX + (int)(R * cosf(p_ang[i])), CY + (int)(R * sinf(p_ang[i])), 5);
    }
}

static void halo_opa(void *o, int32_t v) { lv_obj_set_style_bg_opa(o, (lv_opa_t)v, 0); }

void pipeline_create(lv_obj_t *tile)
{
    lv_obj_add_event_cb(tile, backdrop, LV_EVENT_DRAW_MAIN_BEGIN, NULL);
    lv_obj_t *title = mk_label(tile, &lv_font_montserrat_20, COL_TEXT, "DEEP WORK");
    lv_obj_set_style_text_letter_space(title, 3, 0);
    lv_obj_set_pos(title, 22, 18);
    lv_obj_set_pos(mk_label(tile, &lv_font_montserrat_12, COL_MUTED, "skill runs per stage, all sessions"), 22, 44);

    for (int i = 0; i < NP; i++) {
        particles[i] = mk_dot(tile, 5, 0xFFFFFF);
        lv_obj_set_style_bg_opa(particles[i], LV_OPA_50, 0);
        p_ang[i] = i * 2 * (float)M_PI / NP;
        p_spd[i] = 0.006f + 0.002f * (i % 4);
    }

    halo = mk_dot(tile, 80, 0xFFFFFF);
    lv_obj_set_hidden(halo, true);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, halo);
    lv_anim_set_exec_cb(&a, halo_opa);
    lv_anim_set_values(&a, 30, 110);
    lv_anim_set_duration(&a, 1100);
    lv_anim_set_reverse_duration(&a, 1100);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_start(&a);

    for (int i = 0; i < N; i++) {
        float ang = stage_angle(i);
        int x = CX + (int)(R * cosf(ang)), y = CY + (int)(R * sinf(ang));
        node[i] = mk_dot(tile, 44, cols[i]);
        lv_obj_set_style_bg_opa(node[i], LV_OPA_20, 0);
        lv_obj_set_style_border_width(node[i], 2, 0);
        lv_obj_set_style_border_color(node[i], lv_color_hex(cols[i]), 0);
        center_on(node[i], x, y, 44);
        cnt[i] = mk_label(node[i], &lv_font_montserrat_16, COL_TEXT, "0");
        lv_obj_center(cnt[i]);
        // side stages get their label beside the node, top/bottom stages above/below it
        lv_obj_t *l = mk_label(tile, &lv_font_montserrat_12, cols[i], names[i]);
        lv_obj_set_width(l, 80);
        float cs = cosf(ang), sn = sinf(ang);
        if (cs > 0.5f) {
            lv_obj_set_pos(l, x + 38, y - 7);
        } else if (cs < -0.5f) {
            lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_RIGHT, 0);
            lv_obj_set_pos(l, x - 38 - 80, y - 7);
        } else {
            lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_set_pos(l, x - 40, sn < 0 ? y - 52 : y + 38);
        }
    }

    total = mk_label(tile, &lv_font_montserrat_48, COL_TEXT, "--");
    lv_obj_align(total, LV_ALIGN_TOP_MID, 0, CY - 56);
    lv_obj_t *cap = mk_label(tile, &lv_font_montserrat_14, COL_MUTED, "skill runs");
    lv_obj_align(cap, LV_ALIGN_TOP_MID, 0, CY);
    last_lbl = mk_label(tile, &lv_font_montserrat_12, COL_TEXT, "");
    lv_label_set_long_mode(last_lbl, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_width(last_lbl, 190);
    lv_obj_set_height(last_lbl, 16);            // DOTS only truncates when the height is fixed
    lv_obj_set_style_text_align(last_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(last_lbl, LV_ALIGN_TOP_MID, 0, CY + 26);
    ago_lbl = mk_label(tile, &lv_font_montserrat_12, COL_MUTED, "");
    lv_obj_align(ago_lbl, LV_ALIGN_TOP_MID, 0, CY + 42);

    lv_timer_create(flow, 40, NULL);
}

void pipeline_on(const char *topic, const cJSON *msg)
{
    if (strcmp(topic, "pipe")) return;
    const cJSON *c = cJSON_GetObjectItem(msg, "c");
    int mx = 1;
    for (int i = 0; i < N; i++) { int v = cJSON_GetArrayItem(c, i) ? cJSON_GetArrayItem(c, i)->valueint : 0; if (v > mx) mx = v; }
    for (int i = 0; i < N; i++) {
        int v = cJSON_GetArrayItem(c, i) ? cJSON_GetArrayItem(c, i)->valueint : 0;
        int size = 40 + (v ? 8 + 20 * v / mx : 0);        // unused stages stay small and hollow
        float ang = stage_angle(i);
        lv_obj_set_size(node[i], size, size);
        center_on(node[i], CX + (int)(R * cosf(ang)), CY + (int)(R * sinf(ang)), size);
        lv_obj_set_style_bg_opa(node[i], v ? (lv_opa_t)(110 + 145 * v / mx) : LV_OPA_10, 0);
        lv_label_set_text_fmt(cnt[i], "%d", v);
    }
    int last = json_int(msg, "last", -1);
    if (last >= 0 && last < N) {
        int hs = lv_obj_get_style_width(node[last], 0) + 26;
        float ang = stage_angle(last);
        lv_obj_set_size(halo, hs, hs);
        lv_obj_set_style_bg_color(halo, lv_color_hex(cols[last]), 0);
        center_on(halo, CX + (int)(R * cosf(ang)), CY + (int)(R * sinf(ang)), hs);
        lv_obj_set_hidden(halo, false);
        lv_obj_set_style_text_color(last_lbl, lv_color_hex(cols[last]), 0);
    }
    lv_label_set_text_fmt(total, "%d", json_int(msg, "total", 0));
    lv_label_set_text_fmt(last_lbl, "last: %s", json_str(msg, "skill", "-"));
    lv_label_set_text(ago_lbl, json_str(msg, "ago", ""));
}
