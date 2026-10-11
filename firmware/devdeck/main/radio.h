#pragma once
// Radio helpers shared by Signal Field and Ghost Sense. Everything is listen-only except radio_wifi_join and
// radio_ping_gateway, which only Ghost Sense uses, and only when Wi-Fi credentials were provisioned over USB.
#include <stdbool.h>
#include <stdint.h>
#include <esp_wifi.h>

#ifdef __cplusplus
extern "C" {
#endif

bool radio_wifi_start(bool with_ip);                          // Wi-Fi driver up in STA mode, not associated
bool radio_creds_present(void);                               // credentials provisioned with `deck.py wifi`?
bool radio_creds_set(const char *ssid, const char *pass);     // empty ssid forgets them
bool radio_wifi_join(uint8_t bssid[6], char ssid[33], uint8_t *channel, int timeout_ms);
bool radio_ping_gateway(int interval_ms);                     // endless small pings to the router
bool radio_wifi_strongest_ap(uint8_t bssid[6], char ssid[33], uint8_t *channel, int8_t *rssi);
void radio_wifi_sniff(wifi_promiscuous_cb_t cb, uint32_t filter_mask);
void radio_wifi_channel(uint8_t ch);

#ifdef __cplusplus
}
#endif
