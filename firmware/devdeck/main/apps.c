// App registry and switching. The chosen app id lives in NVS; switching reboots into it.
#include <string.h>
#include <esp_log.h>
#include <esp_system.h>
#include <nvs_flash.h>
#include <nvs.h>
#include "app.h"

static const char *TAG = "apps";

void devdeck_start(lv_obj_t *scr);  void devdeck_on(const char *t, const cJSON *m);
void home_start(lv_obj_t *scr);
void painting_start(lv_obj_t *scr); void painting_on(const char *t, const cJSON *m);
void lens_start(lv_obj_t *scr);
void field_start(lv_obj_t *scr);
void ghost_start(lv_obj_t *scr);

const app_t APP_HOME = {"home", "Home", "", 0xECEFF4, 100, home_start, NULL};

static const app_t DEVDECK  = {"devdeck",  "DevDeck",         "agents, context, PRs",      0xD97757, 100, devdeck_start,  devdeck_on};
static const app_t PAINTING = {"painting", "Living Painting", "grows from your commits",   0x48C9B0, 100, painting_start, painting_on};
static const app_t LENS     = {"lens",     "Sound Lens",      "spectrogram and tuner",     0xF5C04A, 100, lens_start,     NULL};
static const app_t FIELD    = {"field",    "Signal Field",    "every radio around you",    0x5CC8F0,  40, field_start,    NULL};
static const app_t GHOST    = {"ghost",    "Ghost Sense",     "motion through Wi-Fi",      0xA98BF5,  40, ghost_start,    NULL};

const app_t *const APPS[] = {&DEVDECK, &PAINTING, &LENS, &FIELD, &GHOST};
const int N_APPS = sizeof APPS / sizeof APPS[0];

static const app_t *current = &APP_HOME;

static const app_t *find(const char *id)
{
    if (!strcmp(id, "home")) return &APP_HOME;
    for (int i = 0; i < N_APPS; i++)
        if (!strcmp(APPS[i]->id, id)) return APPS[i];
    return NULL;
}

const app_t *apps_current(void) { return current; }

const app_t *apps_boot(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
    char id[16] = "home";
    uint8_t healthy = 1;
    nvs_handle_t h;
    if (nvs_open("deck", NVS_READONLY, &h) == ESP_OK) {
        size_t n = sizeof id;
        if (nvs_get_str(h, "app", id, &n) != ESP_OK) strcpy(id, "home");
        nvs_get_u8(h, "healthy", &healthy);
        nvs_close(h);
    }
    // An app that crashed, hung or was power-cycled before it was marked healthy would otherwise boot-loop:
    // fall back home. A deliberate switch always sets healthy first.
    if (!healthy) {
        ESP_LOGW(TAG, "app '%s' did not finish starting (reset reason %d); opening home", id, esp_reset_reason());
        strcpy(id, "home");
    }
    const app_t *a = find(id);
    current = a ? a : &APP_HOME;
    if (nvs_open("deck", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "healthy", 0);
        nvs_commit(h);
        nvs_close(h);
    }
    ESP_LOGI(TAG, "booting app '%s'", current->id);
    return current;
}

void apps_mark_healthy(void)
{
    nvs_handle_t h;
    if (nvs_open("deck", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "healthy", 1);
        nvs_commit(h);
        nvs_close(h);
    }
}

void apps_switch(const char *id)
{
    if (!find(id)) {
        ESP_LOGW(TAG, "no app '%s'", id);
        return;
    }
    nvs_handle_t h;
    if (nvs_open("deck", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_str(h, "app", id);
        nvs_set_u8(h, "healthy", 1);   // a deliberate switch is not a crash
        nvs_commit(h);
        nvs_close(h);
    }
    ESP_LOGI(TAG, "switching to '%s'", id);
    esp_restart();
}
