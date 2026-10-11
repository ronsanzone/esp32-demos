// HOME — the launcher. A card per app; tapping one reboots the board into it.
// Press BOOT inside any app to come back here.
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <esp_heap_caps.h>
#include "screens.h"

#define CARD_W 212
#define CARD_H 116

static lv_obj_t *heap_lbl;

static void line(lv_layer_t *layer, int x1, int y1, int x2, int y2, int w, uint32_t col, lv_opa_t opa)
{
    lv_draw_line_dsc_t d;
    lv_draw_line_dsc_init(&d);
    d.p1.x = x1; d.p1.y = y1; d.p2.x = x2; d.p2.y = y2;
    d.width = w; d.color = lv_color_hex(col); d.opa = opa;
    d.round_start = d.round_end = 1;
    lv_draw_line(layer, &d);
}

static void disc(lv_layer_t *layer, int cx, int cy, int r, uint32_t col, lv_opa_t opa)
{
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.bg_color = lv_color_hex(col); d.bg_opa = opa; d.radius = LV_RADIUS_CIRCLE;
    lv_area_t a = {cx - r, cy - r, cx + r - 1, cy + r - 1};
    lv_draw_rect(layer, &d, &a);
}

static void ring(lv_layer_t *layer, int cx, int cy, int r, uint32_t col, lv_opa_t opa)
{
    lv_draw_arc_dsc_t a;
    lv_draw_arc_dsc_init(&a);
    a.center.x = cx; a.center.y = cy; a.radius = r;
    a.start_angle = 0; a.end_angle = 360; a.width = 1;
    a.color = lv_color_hex(col); a.opa = opa;
    lv_draw_arc(layer, &a);
}

// A small generative glyph per app, drawn into a 56x56 box.
static void draw_icon(lv_event_t *e)
{
    lv_obj_t *o = lv_event_get_target(e);
    const app_t *app = lv_obj_get_user_data(o);
    lv_layer_t *L = lv_event_get_layer(e);
    lv_area_t c;
    lv_obj_get_coords(o, &c);
    int cx = (c.x1 + c.x2) / 2, cy = (c.y1 + c.y2) / 2;
    const char *id = app->id;
    if (!strcmp(id, "devdeck")) {
        ring(L, cx, cy, 24, 0x3A3F47, LV_OPA_COVER);
        ring(L, cx, cy, 15, 0x3A3F47, LV_OPA_COVER);
        disc(L, cx, cy, 8, 0xD97757, LV_OPA_COVER);
        disc(L, cx + 17, cy - 17, 4, 0x4DD0E1, LV_OPA_COVER);
        disc(L, cx - 15, cy + 2, 3, 0xFFB74D, LV_OPA_COVER);
    } else if (!strcmp(id, "painting")) {
        static const int8_t b[][3] = {{-12, -10, 9}, {6, -14, 7}, {14, 4, 10}, {-6, 10, 8}, {-18, 12, 4}, {4, 20, 5}};
        for (int i = 0; i < 6; i++) disc(L, cx + b[i][0], cy + b[i][1], b[i][2], i % 2 ? 0xF2A65A : 0x48C9B0, LV_OPA_80);
    } else if (!strcmp(id, "lens")) {
        static const uint8_t h[] = {10, 22, 40, 28, 46, 18, 30, 12, 6};
        for (int i = 0; i < 9; i++) line(L, cx - 24 + i * 6, cy + 22 - h[i], cx - 24 + i * 6, cy + 22, 3, 0xF5C04A, i == 4 ? LV_OPA_COVER : LV_OPA_70);
    } else if (!strcmp(id, "field")) {
        ring(L, cx, cy, 25, 0x2A3340, LV_OPA_COVER);
        ring(L, cx, cy, 14, 0x2A3340, LV_OPA_COVER);
        disc(L, cx - 12, cy - 16, 6, 0x5CC8F0, LV_OPA_COVER);
        disc(L, cx + 16, cy - 2, 4, 0xF2A65A, LV_OPA_COVER);
        disc(L, cx - 4, cy + 18, 5, 0xA98BF5, LV_OPA_COVER);
        disc(L, cx + 6, cy + 6, 2, 0xF2A65A, LV_OPA_COVER);
    } else if (!strcmp(id, "ghost")) {
        for (int k = 0; k < 5; k++) {
            int y0 = cy - 18 + k * 9, px = cx - 26, py = y0;
            for (int x = -26; x <= 26; x += 4) {
                float bump = (k == 2 ? 9.0f : 3.0f) * expf(-(x * x) / 120.0f);
                int ny = y0 - (int)bump;
                if (x > -26) line(L, px, py, cx + x, ny, 2, 0xA98BF5, k == 2 ? LV_OPA_COVER : LV_OPA_50);
                px = cx + x; py = ny;
            }
        }
    }
}

static void launch_cb(lv_timer_t *t) { apps_switch(((const app_t *)lv_timer_get_user_data(t))->id); }

static void on_card(lv_event_t *e)
{
    lv_obj_t *card = lv_event_get_current_target(e);
    const app_t *app = lv_obj_get_user_data(card);
    lv_obj_set_style_border_color(card, lv_color_hex(app->color), 0);
    lv_obj_set_style_border_width(card, 2, 0);
    lv_obj_t *blurb = lv_obj_get_child(card, 2);
    lv_label_set_text(blurb, "starting...");
    lv_obj_set_style_text_color(blurb, lv_color_hex(app->color), 0);
    lv_timer_t *t = lv_timer_create(launch_cb, 150, (void *)app);   // let the highlight paint first
    lv_timer_set_repeat_count(t, 1);
}

static lv_obj_t *mk_card(lv_obj_t *scr, int x, int y)
{
    lv_obj_t *card = lv_obj_create(scr);
    lv_obj_remove_style_all(card);
    lv_obj_set_pos(card, x, y);
    lv_obj_set_size(card, CARD_W, CARD_H);
    lv_obj_set_style_radius(card, 18, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x0B0D11), 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x161A21), LV_STATE_PRESSED);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(0x1E232B), 0);
    lv_obj_set_scrollable(card, false);
    return card;
}

static void app_card(lv_obj_t *scr, const app_t *app, int x, int y)
{
    lv_obj_t *card = mk_card(scr, x, y);
    lv_obj_set_user_data(card, (void *)app);
    lv_obj_set_clickable(card, true);
    lv_obj_add_event_cb(card, on_card, LV_EVENT_CLICKED, NULL);

    lv_obj_t *icon = lv_obj_create(card);
    lv_obj_remove_style_all(icon);
    lv_obj_set_size(icon, 56, 56);
    lv_obj_set_pos(icon, 12, 30);
    lv_obj_set_user_data(icon, (void *)app);
    lv_obj_set_clickable(icon, false);
    lv_obj_add_event_cb(icon, draw_icon, LV_EVENT_DRAW_MAIN, NULL);

    lv_obj_t *name = mk_label(card, &lv_font_montserrat_16, COL_TEXT, app->name);
    lv_obj_set_pos(name, 78, 34);
    lv_obj_set_width(name, CARD_W - 86);
    lv_obj_t *blurb = mk_label(card, &lv_font_montserrat_12, COL_MUTED, app->blurb);
    lv_obj_set_pos(blurb, 78, 60);
    lv_obj_set_width(blurb, CARD_W - 86);
    lv_label_set_long_mode(blurb, LV_LABEL_LONG_MODE_WRAP);
}

static void tick(lv_timer_t *t)
{
    char b[40];
    snprintf(b, sizeof b, "%u KB free", (unsigned)(heap_caps_get_free_size(MALLOC_CAP_8BIT) / 1024));
    lv_label_set_text(heap_lbl, b);
}

void home_start(lv_obj_t *scr)
{
    lv_obj_t *title = mk_label(scr, &lv_font_montserrat_28, COL_TEXT, "DECK");
    lv_obj_set_style_text_letter_space(title, 8, 0);
    lv_obj_set_pos(title, 22, 22);
    heap_lbl = mk_label(scr, &lv_font_montserrat_12, COL_MUTED, "");
    lv_obj_align(heap_lbl, LV_ALIGN_TOP_RIGHT, -22, 34);
    tick(NULL);
    lv_timer_create(tick, 2000, NULL);

    for (int i = 0; i < N_APPS && i < 5; i++)
        app_card(scr, APPS[i], 20 + (i % 2) * (CARD_W + 16), 78 + (i / 2) * (CARD_H + 14));

    lv_obj_t *info = mk_card(scr, 20 + CARD_W + 16, 78 + 2 * (CARD_H + 14));
    lv_obj_set_style_bg_opa(info, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(info, lv_color_hex(0x15181E), 0);
    lv_obj_t *h = mk_label(info, &lv_font_montserrat_12, COL_MUTED, "BOOT button\nbrings you back here\nfrom any app");
    lv_obj_set_style_text_line_space(h, 4, 0);
    lv_obj_set_pos(h, 18, 30);
}
