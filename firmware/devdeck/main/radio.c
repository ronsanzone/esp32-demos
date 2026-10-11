#include <string.h>
#include <esp_event.h>
#include <esp_log.h>
#include <esp_wifi.h>
#include "radio.h"

static const char *TAG = "radio";

bool radio_wifi_start(void)
{
    esp_event_loop_create_default();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t err = esp_wifi_init(&cfg);
    if (err == ESP_OK) err = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (err == ESP_OK) err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err == ESP_OK) err = esp_wifi_start();
    if (err == ESP_OK) esp_wifi_set_ps(WIFI_PS_NONE);       // keep the receiver on for sniffing
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "wifi start failed: %s", esp_err_to_name(err));
        return false;
    }
    return true;
}

bool radio_wifi_strongest_ap(uint8_t bssid[6], char ssid[33], uint8_t *channel, int8_t *rssi)
{
    wifi_scan_config_t sc = {0};
    if (esp_wifi_scan_start(&sc, true) != ESP_OK) return false;
    uint16_t n = 1;
    wifi_ap_record_t best;
    // Records come back sorted strongest first; we only need the first.
    if (esp_wifi_scan_get_ap_records(&n, &best) != ESP_OK || n == 0) {
        esp_wifi_clear_ap_list();
        return false;
    }
    esp_wifi_clear_ap_list();
    memcpy(bssid, best.bssid, 6);
    memcpy(ssid, best.ssid, 32);
    ssid[32] = 0;
    *channel = best.primary;
    *rssi = best.rssi;
    return true;
}

void radio_wifi_sniff(wifi_promiscuous_cb_t cb, uint32_t filter_mask)
{
    wifi_promiscuous_filter_t f = {.filter_mask = filter_mask};
    esp_wifi_set_promiscuous_filter(&f);
    esp_wifi_set_promiscuous_rx_cb(cb);
    esp_wifi_set_promiscuous(true);
}

void radio_wifi_channel(uint8_t ch)
{
    esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
}
