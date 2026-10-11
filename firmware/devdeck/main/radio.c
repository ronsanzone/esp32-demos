#include <string.h>
#include <esp_event.h>
#include <esp_log.h>
#include <esp_wifi.h>
#include <esp_netif.h>
#include <nvs.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include "ping/ping_sock.h"
#include "radio.h"

static const char *TAG = "radio";

bool radio_wifi_start(bool with_ip)
{
    esp_event_loop_create_default();
    if (with_ip) {
        esp_netif_init();
        esp_netif_create_default_wifi_sta();
    }
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

// ---- joining (Ghost Sense only) ----
// Credentials live in NVS namespace "wifi", written over USB by `deck.py wifi`; they are never logged.
static bool read_creds(char ssid[33], char pass[65])
{
    nvs_handle_t h;
    if (nvs_open("wifi", NVS_READONLY, &h) != ESP_OK) return false;
    size_t n1 = 33, n2 = 65;
    bool ok = nvs_get_str(h, "ssid", ssid, &n1) == ESP_OK && nvs_get_str(h, "pass", pass, &n2) == ESP_OK && ssid[0];
    nvs_close(h);
    return ok;
}

bool radio_creds_present(void)
{
    char ssid[33], pass[65];
    bool ok = read_creds(ssid, pass);
    memset(pass, 0, sizeof pass);
    return ok;
}

bool radio_creds_set(const char *ssid, const char *pass)
{
    nvs_handle_t h;
    if (nvs_open("wifi", NVS_READWRITE, &h) != ESP_OK) return false;
    esp_err_t err;
    if (!ssid || !ssid[0]) err = nvs_erase_all(h);
    else if (strlen(ssid) > 32 || !pass || strlen(pass) > 64) err = ESP_ERR_INVALID_ARG;
    else {
        err = nvs_set_str(h, "ssid", ssid);
        if (err == ESP_OK) err = nvs_set_str(h, "pass", pass);
    }
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err == ESP_OK;
}

static EventGroupHandle_t join_ev;
static bool joined;

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        joined = true;
        xEventGroupSetBits(join_ev, 1);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        if (joined) esp_wifi_connect();                  // dropped after joining: keep trying
        else xEventGroupSetBits(join_ev, 2);
    }
}

bool radio_wifi_join(uint8_t bssid[6], char ssid_out[33], uint8_t *channel, int timeout_ms)
{
    char ssid[33], pass[65];
    if (!read_creds(ssid, pass)) return false;
    if (!join_ev) {
        join_ev = xEventGroupCreate();
        esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL);
        esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, on_event, NULL);
    }
    esp_log_level_set("wifi", ESP_LOG_WARN);              // the driver logs the network name on connect
    wifi_config_t wc = {0};
    memcpy(wc.sta.ssid, ssid, strlen(ssid));
    memcpy(wc.sta.password, pass, strlen(pass));
    memset(pass, 0, sizeof pass);
    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &wc);
    memset(&wc, 0, sizeof wc);
    xEventGroupClearBits(join_ev, 3);
    if (err == ESP_OK) err = esp_wifi_connect();
    EventBits_t b = err == ESP_OK ? xEventGroupWaitBits(join_ev, 3, pdFALSE, pdFALSE, pdMS_TO_TICKS(timeout_ms)) : 0;
    if (!(b & 1)) {
        ESP_LOGW(TAG, "could not join the provisioned network");
        esp_wifi_disconnect();
        return false;
    }
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) return false;
    memcpy(bssid, ap.bssid, 6);
    memcpy(ssid_out, ap.ssid, 32);
    ssid_out[32] = 0;
    *channel = ap.primary;
    return true;
}

bool radio_ping_gateway(int interval_ms)
{
    esp_netif_t *n = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip;
    if (!n || esp_netif_get_ip_info(n, &ip) != ESP_OK || !ip.gw.addr) return false;
    esp_ping_config_t c = ESP_PING_DEFAULT_CONFIG();
    c.count = ESP_PING_COUNT_INFINITE;
    c.interval_ms = interval_ms;
    c.timeout_ms = 1000;
    c.data_size = 8;
    ip4_addr_set_u32(ip_2_ip4(&c.target_addr), ip.gw.addr);
    IP_SET_TYPE(&c.target_addr, IPADDR_TYPE_V4);
    esp_ping_callbacks_t cb = {0};
    esp_ping_handle_t h;
    if (esp_ping_new_session(&c, &cb, &h) != ESP_OK) return false;
    return esp_ping_start(h) == ESP_OK;
}
