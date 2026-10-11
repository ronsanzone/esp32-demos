// Tileview shell: swipeable screens, page dots, shared widgets, message dispatch.
#include <string.h>
#include "screens.h"

#define N_TILES 4
#define AUTOPLAY_IDLE_MS 60000   // untouched this long → rotate screens
#define AUTOPLAY_EVERY_MS 15000

static lv_obj_t *dots[N_TILES], *tv;
static uint32_t last_flip;

lv_obj_t *mk_label(lv_obj_t *parent, const lv_font_t *font, uint32_t color, const char *text)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_label_set_text(l, text);
    return l;
}

lv_obj_t *mk_dot(lv_obj_t *parent, int size, uint32_t color)
{
    lv_obj_t *d = lv_obj_create(parent);
    lv_obj_remove_style_all(d);
    lv_obj_set_size(d, size, size);
    lv_obj_set_style_radius(d, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(d, lv_color_hex(color), 0);
    lv_obj_set_clickable(d, false);
    lv_obj_set_scrollable(d, false);
    return d;
}

uint32_t tool_color(const char *t)
{
    if (!strcmp(t, "Bash")) return 0x69F0AE;
    if (!strcmp(t, "Edit") || !strcmp(t, "Write") || !strcmp(t, "NotebookEdit")) return 0xFF8A65;
    if (!strcmp(t, "Read") || !strcmp(t, "Grep") || !strcmp(t, "Glob")) return 0x4FC3F7;
    if (!strcmp(t, "Agent") || !strcmp(t, "Task")) return 0xB388FF;
    if (!strncmp(t, "Web", 3)) return 0xFFD54F;
    if (!strcmp(t, "Skill")) return 0xF06292;
    return 0xB0BEC5;
}

const char *json_str(const cJSON *o, const char *k, const char *dflt)
{
    const cJSON *v = cJSON_GetObjectItem(o, k);
    return cJSON_IsString(v) ? v->valuestring : dflt;
}

int json_int(const cJSON *o, const char *k, int dflt)
{
    const cJSON *v = cJSON_GetObjectItem(o, k);
    return cJSON_IsNumber(v) ? v->valueint : dflt;
}

static void on_tile_changed(lv_event_t *e)
{
    lv_obj_t *tv = lv_event_get_target(e);
    lv_obj_t *active = lv_tileview_get_tile_active(tv);
    for (int i = 0; i < N_TILES; i++)
        lv_obj_set_style_bg_opa(dots[i], lv_obj_get_index(active) == i ? LV_OPA_COVER : LV_OPA_30, 0);
}

// Hands-off mode: when nobody has touched the screen for a while, rotate through the screens.
static void autoplay(lv_timer_t *t)
{
    uint32_t now = lv_tick_get();
    if (lv_display_get_inactive_time(NULL) < AUTOPLAY_IDLE_MS || now - last_flip < AUTOPLAY_EVERY_MS) return;
    last_flip = now;
    int next = (lv_obj_get_index(lv_tileview_get_tile_active(tv)) + 1) % N_TILES;
    lv_tileview_set_tile_by_index(tv, next, 0, LV_ANIM_ON);
}

static lv_obj_t *mk_tile(lv_obj_t *tv, int i)
{
    lv_obj_t *t = lv_tileview_add_tile(tv, i, 0, LV_DIR_LEFT | LV_DIR_RIGHT);
    lv_obj_set_scrollable(t, false);
    return t;
}

void ui_create(void)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    tv = lv_tileview_create(scr);
    lv_obj_set_style_bg_color(tv, lv_color_black(), 0);
    lv_obj_set_scrollbar_mode(tv, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_event_cb(tv, on_tile_changed, LV_EVENT_VALUE_CHANGED, NULL);

    orbit_create(mk_tile(tv, 0));
    context_create(mk_tile(tv, 1));
    pipeline_create(mk_tile(tv, 2));
    pulse_create(mk_tile(tv, 3));
    lv_timer_create(autoplay, 1000, NULL);

    lv_obj_t *row = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_SIZE_CONTENT, 8);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, 8, 0);
    lv_obj_align(row, LV_ALIGN_BOTTOM_MID, 0, -10);
    for (int i = 0; i < N_TILES; i++) {
        dots[i] = mk_dot(row, 6, COL_TEXT);
        lv_obj_set_style_bg_opa(dots[i], i ? LV_OPA_30 : LV_OPA_COVER, 0);
    }
}

void ui_on_message(const char *topic, const cJSON *msg)
{
    if (!strcmp(topic, "tile")) lv_tileview_set_tile_by_index(tv, json_int(msg, "i", 0), 0, LV_ANIM_OFF);
    orbit_on(topic, msg);
    context_on(topic, msg);
    pipeline_on(topic, msg);
    pulse_on(topic, msg);
}
