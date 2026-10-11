#pragma once

#include "lvgl.h"
#include "display_bsp.h"

void Lvgl_PortInit(DisplayPort &display, int buf_lines = 100);
void Lvgl_Refresh(void);
esp_err_t Lvgl_lock(int timeout_ms);
void Lvgl_unlock(void);