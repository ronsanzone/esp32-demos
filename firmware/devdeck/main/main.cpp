#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <driver/gpio.h>
#include <esp_log.h>
#include <esp_heap_caps.h>
#include "lvgl_bsp.h"
#include "power_bsp.h"
#include "app.h"

#define BOOT_BUTTON GPIO_NUM_9          // the BOOT key: press it in any app to return to the launcher

static I2cMasterBus i2cbus(7, 8, 0);   // scl, sda, port — shared by PMU, touch, IMU, RTC, audio codecs

bool app_lock(void) { return Lvgl_lock(-1) == ESP_OK; }
void app_unlock(void) { Lvgl_unlock(); }
i2c_master_bus_handle_t app_i2c_bus(void) { return i2cbus.Get_I2cBusHandle(); }

static void log_heap(const char *when)
{
    ESP_LOGI("main", "%s: free heap %u, largest block %u", when,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
}

static volatile TickType_t started_at;  // set once the app's start() has returned

// Polls BOOT (active low). A press anywhere but the launcher goes home, even if the app hangs during start.
// 5 s after start() returns the app is marked healthy; a reset before that makes the next boot go home.
static void button_task(void *arg)
{
    gpio_config_t io = {};
    io.pin_bit_mask = 1ULL << BOOT_BUTTON;
    io.mode = GPIO_MODE_INPUT;
    io.pull_up_en = GPIO_PULLUP_ENABLE;
    gpio_config(&io);
    vTaskDelay(pdMS_TO_TICKS(800));     // ignore the key if it is still held from a reset
    int held = 0;
    bool healthy = false;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(25));
        if (!healthy && started_at && xTaskGetTickCount() - started_at > pdMS_TO_TICKS(5000)) {
            apps_mark_healthy();
            healthy = true;
            log_heap("app running");
        }
        if (gpio_get_level(BOOT_BUTTON) == 0) {
            if (++held == 2 && apps_current() != &APP_HOME) apps_switch("home");
        } else {
            held = 0;
        }
    }
}

extern "C" void app_main(void)
{
    const app_t *app = apps_boot();
    xTaskCreate(button_task, "button", 3 * 1024, NULL, 3, NULL);
    Custom_PmicPortInit(&i2cbus, 0x34);
    static DisplayPort display(i2cbus, 480, 480);
    display.DisplayPort_TouchInit();
    Lvgl_PortInit(display, app->buf_lines);
    log_heap("display up");

    if (Lvgl_lock(-1) == ESP_OK) {
        lv_obj_t *scr = lv_screen_active();
        lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
        app->start(scr);
        link_install_snapshot(lv_display_get_default());
        Lvgl_unlock();
    }
    link_start();
    char line[32];
    snprintf(line, sizeof line, "@@APP %s", app->id);
    link_print(line);
    started_at = xTaskGetTickCount() | 1;
    log_heap("app started");
}
