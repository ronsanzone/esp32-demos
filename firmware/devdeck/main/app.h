#pragma once
// Shared glue between the USB link (link.c) and the screens (ui.c).
#include <stdbool.h>
#include "lvgl.h"
#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

bool app_lock(void);                   // LVGL is single-threaded: wrap every lv_* call from other tasks
void app_unlock(void);

void link_start(void);                 // spawn the USB JSON-lines reader task
void link_install_snapshot(lv_display_t *disp);

void ui_create(void);                  // build all screens; call under the LVGL lock
void ui_on_message(const char *topic, const cJSON *msg); // dispatch; called under the LVGL lock

#ifdef __cplusplus
}
#endif
