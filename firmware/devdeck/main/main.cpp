#include <freertos/FreeRTOS.h>
#include <esp_log.h>
#include <esp_heap_caps.h>
#include "lvgl_bsp.h"
#include "power_bsp.h"
#include "app.h"

static I2cMasterBus i2cbus(7, 8, 0);   // scl, sda, port — shared by PMU, touch, IMU

bool app_lock(void) { return Lvgl_lock(-1) == ESP_OK; }
void app_unlock(void) { Lvgl_unlock(); }

extern "C" void app_main(void)
{
    Custom_PmicPortInit(&i2cbus, 0x34);
    static DisplayPort display(i2cbus, 480, 480);
    display.DisplayPort_TouchInit();
    Lvgl_PortInit(display);

    if (Lvgl_lock(-1) == ESP_OK) {
        ui_create();
        link_install_snapshot(lv_display_get_default());
        Lvgl_unlock();
    }
    link_start();
    ESP_LOGI("main", "free heap %u, largest block %u", (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
}
