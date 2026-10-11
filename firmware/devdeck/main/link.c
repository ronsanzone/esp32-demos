// USB-Serial/JTAG link to the laptop.
//   host -> device : one JSON object per line, {"t":"<topic>", ...}
//   device -> host : "@@ACK <topic>" per message; "@@SNAP x1 y1 x2 y2 n\n"+raw RGB565 per flushed strip;
//                    "@@APP <id>" once per boot, naming the running app
// Every app gets every message; {"t":"app","id":"lens"} switches apps (the board reboots into it).
// {"t":"wifi","s":ssid,"p":password} stores Ghost Sense's credentials in NVS ("s":"" forgets them).
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <freertos/FreeRTOS.h>
#include <driver/usb_serial_jtag.h>
#include <driver/usb_serial_jtag_vfs.h>
#include <esp_log.h>
#include "app.h"
#include "radio.h"

static const char *TAG = "link";
#define LINE_CAP (16 * 1024)
static volatile bool snapping;

static void out(const void *buf, size_t n)
{
    const uint8_t *p = (const uint8_t *)buf;
    while (n) {
        size_t c = n > 1024 ? 1024 : n;   // the driver rejects writes larger than its TX ring
        int w = usb_serial_jtag_write_bytes(p, c, pdMS_TO_TICKS(200));
        if (w <= 0) return;            // host not reading: drop rather than wedge the UI
        p += w; n -= w;
    }
}

// Called by LVGL right before each strip goes to the panel; px is native RGB565.
static void on_flush_start(lv_event_t *e)
{
    if (!snapping) return;
    const lv_area_t *a = (const lv_area_t *)lv_event_get_param(e);
    lv_display_t *disp = (lv_display_t *)lv_event_get_target(e);
    uint32_t n = lv_area_get_size(a) * 2;
    char hdr[64];
    int len = snprintf(hdr, sizeof hdr, "\n@@SNAP %d %d %d %d %lu\n",
                       (int)a->x1, (int)a->y1, (int)a->x2, (int)a->y2, (unsigned long)n);
    out(hdr, len);
    out(lv_display_get_buf_active(disp)->data, n);
}

void link_print(const char *line)
{
    out("\n", 1);
    out(line, strlen(line));
    out("\n", 1);
}

void link_install_snapshot(lv_display_t *disp)
{
    lv_display_add_event_cb(disp, on_flush_start, LV_EVENT_FLUSH_START, NULL);
}

static void snapshot(void)
{
    esp_log_level_set("*", ESP_LOG_NONE);   // keep log lines out of the pixel stream
    if (app_lock()) {
        lv_obj_invalidate(lv_screen_active());
        snapping = true;
        lv_refr_now(NULL);
        snapping = false;
        app_unlock();
    }
    out("\n@@SNAPEND\n", 11);
    esp_log_level_set("*", ESP_LOG_INFO);
}

static void handle_line(char *line)
{
    cJSON *msg = cJSON_Parse(line);
    const cJSON *t = cJSON_GetObjectItem(msg, "t");
    if (!cJSON_IsString(t)) {
        ESP_LOGW(TAG, "bad line (%u bytes)", (unsigned)strlen(line));
        cJSON_Delete(msg);
        return;
    }
    char ack[48];
    int n = snprintf(ack, sizeof ack, "@@ACK %.32s\n", t->valuestring);
    const app_t *app = apps_current();
    if (strcmp(t->valuestring, "snap") == 0) {
        snapshot();
    } else if (strcmp(t->valuestring, "app") == 0) {
        const cJSON *id = cJSON_GetObjectItem(msg, "id");
        out(ack, n);                                 // ack first: the switch reboots the board
        vTaskDelay(pdMS_TO_TICKS(50));
        if (cJSON_IsString(id)) apps_switch(id->valuestring);   // returns only for an unknown id
        cJSON_Delete(msg);
        return;
    } else if (strcmp(t->valuestring, "wifi") == 0) {    // credentials for Ghost Sense; never logged
        bool ok = radio_creds_set(cJSON_GetStringValue(cJSON_GetObjectItem(msg, "s")),
                                  cJSON_GetStringValue(cJSON_GetObjectItem(msg, "p")));
        if (!ok) n = snprintf(ack, sizeof ack, "@@NAK wifi\n");
        char *pw = cJSON_GetStringValue(cJSON_GetObjectItem(msg, "p"));
        if (pw) memset(pw, 0, strlen(pw));               // don't leave the password in freed heap
        memset(line, 0, strlen(line));                   // ... or in the line buffer
    } else if (app->on && app_lock()) {
        app->on(t->valuestring, msg);
        app_unlock();
    }
    out(ack, n);
    cJSON_Delete(msg);
}

static void link_task(void *arg)
{
    static char line[LINE_CAP];
    size_t len = 0;
    uint8_t chunk[256];
    for (;;) {
        int n = usb_serial_jtag_read_bytes(chunk, sizeof chunk, portMAX_DELAY);
        for (int i = 0; i < n; i++) {
            if (chunk[i] == '\n') {
                line[len] = 0;
                if (len) handle_line(line);
                len = 0;
            } else if (len < LINE_CAP - 1) {
                line[len++] = chunk[i];
            }
        }
    }
}

void link_start(void)
{
    usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    cfg.rx_buffer_size = 4096;
    cfg.tx_buffer_size = 4096;
    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&cfg));
    usb_serial_jtag_vfs_use_driver();      // printf/ESP_LOG now share the driver with raw writes
    xTaskCreate(link_task, "link", 6 * 1024, NULL, 5, NULL);
    ESP_LOGI(TAG, "ready");
}
