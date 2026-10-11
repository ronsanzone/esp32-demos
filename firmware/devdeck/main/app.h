#pragma once
// Shared glue: the USB link, the app registry, and the LVGL lock.
#include <stdbool.h>
#include <stdint.h>
#include "lvgl.h"
#include "cJSON.h"
#include <driver/i2c_master.h>

#ifdef __cplusplus
extern "C" {
#endif

bool app_lock(void);                   // LVGL is single-threaded: wrap every lv_* call from other tasks
void app_unlock(void);
i2c_master_bus_handle_t app_i2c_bus(void); // the shared I2C bus (PMU, touch, IMU, RTC, audio codecs)

void link_start(void);                 // spawn the USB JSON-lines reader task
void link_install_snapshot(lv_display_t *disp);
void link_print(const char *line);     // write one raw line to the host (e.g. "@@APP lens")

// ---- apps ----
// The board runs one app at a time. Switching writes the choice to NVS and reboots,
// so every app starts on a clean heap and can claim radios or audio without teardown code.
typedef struct {
    const char *id, *name, *blurb;
    uint32_t color;
    int buf_lines;                                       // LVGL strip height; smaller frees heap for radios
    void (*start)(lv_obj_t *scr);                        // build the UI; called once under the LVGL lock
    void (*on)(const char *topic, const cJSON *msg);     // bridge messages, under the LVGL lock (may be NULL)
} app_t;

extern const app_t *const APPS[];      // launchable apps, in launcher order
extern const int N_APPS;
extern const app_t APP_HOME;

const app_t *apps_boot(void);          // which app this boot runs (NVS, with crash fallback to home)
const app_t *apps_current(void);
void apps_switch(const char *id);      // persist and reboot into another app ("home" for the launcher)
void apps_mark_healthy(void);          // the current app survived its first seconds

#ifdef __cplusplus
}
#endif
