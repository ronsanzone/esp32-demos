#pragma once
// Each screen owns one tileview tile: build it once, then react to its topics.
#include "app.h"

#define COL_BG      0x000000
#define COL_DIM     0x3A3F47
#define COL_MUTED   0x8A919C
#define COL_TEXT    0xECEFF4
#define COL_CLAUDE  0xD97757

lv_obj_t *mk_label(lv_obj_t *parent, const lv_font_t *font, uint32_t color, const char *text);
lv_obj_t *mk_dot(lv_obj_t *parent, int size, uint32_t color);
uint32_t tool_color(const char *tool);
const char *json_str(const cJSON *obj, const char *key, const char *dflt);
int json_int(const cJSON *obj, const char *key, int dflt);

void orbit_create(lv_obj_t *tile);
void orbit_on(const char *topic, const cJSON *msg);
void context_create(lv_obj_t *tile);
void context_on(const char *topic, const cJSON *msg);
void pipeline_create(lv_obj_t *tile);
void pipeline_on(const char *topic, const cJSON *msg);
void pulse_create(lv_obj_t *tile);
void pulse_on(const char *topic, const cJSON *msg);
