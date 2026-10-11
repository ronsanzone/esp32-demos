#pragma once
// Listen-only radio helpers shared by Signal Field and Ghost Sense. Nothing here connects or transmits data.
#include <stdbool.h>
#include <stdint.h>
#include <esp_wifi.h>

#ifdef __cplusplus
extern "C" {
#endif

bool radio_wifi_start(void);                                  // Wi-Fi driver up in STA mode, not associated
bool radio_wifi_strongest_ap(uint8_t bssid[6], char ssid[33], uint8_t *channel, int8_t *rssi);
void radio_wifi_sniff(wifi_promiscuous_cb_t cb, uint32_t filter_mask);
void radio_wifi_channel(uint8_t ch);

#ifdef __cplusplus
}
#endif
