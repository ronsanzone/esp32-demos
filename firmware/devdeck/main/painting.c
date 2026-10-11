// LIVING PAINTING — a Gray-Scott reaction-diffusion field that keeps growing for weeks.
//   Tap to drop a seed. Hold to change the palette.
//   {"t":"commit","h":"a1b2c3d","r":"esp32-demos"}   a new commit seeds a bloom
//   {"t":"tool","n":"Edit",...}                       agent edits drop small seeds (rate-limited)
// The field is 96x96 cells in Q14 fixed point (the C6 has no FPU), shown 5x upscaled with bilinear
// filtering, and saved to the storage partition every few minutes so a reboot doesn't erase it.
#include <stdio.h>
#include <string.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_partition.h>
#include <esp_random.h>
#include <esp_rom_crc.h>
#include <nvs.h>
#include "screens.h"

static const char *TAG = "painting";

#define N 96
#define CELLS (N * N)
#define ONE 16384                       // 1.0 in Q14
#define DU 2621                         // 0.16
#define DV 1311                         // 0.08
#define K_RATE 1032                     // 0.063
#define F_LO 819                        // 0.050 at the top-left corner ...
#define F_HI 950                        // ... rising to 0.058 at the bottom-right
#define SAVE_EVERY_MS (5 * 60 * 1000)
#define SLOT_BYTES (40 * 1024)
#define N_SLOTS 32
#define MAGIC 0x544E504C                // "LPNT"

typedef struct {
    uint32_t magic, seq, seeds, crc;
    uint64_t gen;
    uint8_t palette, pad[7];
} save_hdr_t;

static int16_t *U, *V, *U2, *V2;
static volatile uint64_t gen;
static volatile uint32_t seeds;
static uint32_t save_seq;
static uint8_t palette;
static const esp_partition_t *part;

typedef struct { int16_t x, y, r; } seed_t;
static QueueHandle_t seed_q;

static lv_obj_t *img, *overlay, *gen_lbl, *toast;
static lv_draw_buf_t *buf;
static uint16_t lut[256];
static uint32_t last_touch, last_tool_seed;

// ---- palettes: five stops each, deep to bright ----
static const uint32_t PALETTES[][5] = {
    {0x071426, 0x0E3B5C, 0x1F8A8A, 0xF2A65A, 0xFFE3B0},   // reef
    {0x0A0507, 0x3B0F1E, 0xA3243B, 0xF06A3B, 0xFFD38A},   // ember
    {0x050A06, 0x12301C, 0x2E7D4F, 0x9BD36A, 0xF4F7C8},   // moss
    {0x07070C, 0x22223A, 0x5A5A90, 0xB8B8E6, 0xFFFFFF},   // ink
};
#define N_PALETTES (sizeof PALETTES / sizeof PALETTES[0])

static void build_lut(void)
{
    const uint32_t *p = PALETTES[palette % N_PALETTES];
    for (int i = 0; i < 256; i++) {
        int seg = i * 4 / 256, t = (i * 4) % 256;
        uint32_t a = p[seg], b = p[seg + 1];
        int r = ((a >> 16 & 255) * (256 - t) + (b >> 16 & 255) * t) >> 8;
        int g = ((a >> 8 & 255) * (256 - t) + (b >> 8 & 255) * t) >> 8;
        int bl = ((a & 255) * (256 - t) + (b & 255) * t) >> 8;
        lut[i] = (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (bl >> 3));
    }
}

// ---- simulation ----
static void put_seed(int cx, int cy, int r)
{
    for (int dy = -r; dy <= r; dy++)
        for (int dx = -r; dx <= r; dx++) {
            if (dx * dx + dy * dy > r * r) continue;
            int i = ((cy + dy + N) % N) * N + (cx + dx + N) % N;
            U[i] = ONE / 2;
            V[i] = ONE / 4 + (int16_t)(esp_random() % (ONE / 8));
        }
    seeds++;
}

static void step(void)
{
    for (int y = 0; y < N; y++) {
        const int16_t *ur = U + y * N, *vr = V + y * N;
        const int16_t *uu = U + ((y + N - 1) % N) * N, *ud = U + ((y + 1) % N) * N;
        const int16_t *vu = V + ((y + N - 1) % N) * N, *vd = V + ((y + 1) % N) * N;
        int16_t *uo = U2 + y * N, *vo = V2 + y * N;
        for (int x = 0; x < N; x++) {
            int xl = x ? x - 1 : N - 1, xr = x < N - 1 ? x + 1 : 0;
            int32_t u = ur[x], v = vr[x];
            int32_t lu = ur[xl] + ur[xr] + uu[x] + ud[x] - 4 * u;
            int32_t lv = vr[xl] + vr[xr] + vu[x] + vd[x] - 4 * v;
            int32_t uvv = ((u * v) >> 14) * v >> 14;
            int32_t f = F_LO + (F_HI - F_LO) * (x + y) / (2 * N);
            int32_t nu = u + ((DU * lu) >> 14) - uvv + ((f * (ONE - u)) >> 14);
            int32_t nv = v + ((DV * lv) >> 14) + uvv - (((f + K_RATE) * v) >> 14);
            uo[x] = (int16_t)(nu < 0 ? 0 : nu > ONE ? ONE : nu);
            vo[x] = (int16_t)(nv < 0 ? 0 : nv > ONE ? ONE : nv);
        }
    }
    int16_t *t = U; U = U2; U2 = t;
    t = V; V = V2; V2 = t;
    gen++;
}

// ---- persistence: a ring of slots in the storage partition, newest seq wins ----
static uint32_t state_crc(void)
{
    uint32_t c = esp_rom_crc32_le(0, (const uint8_t *)U, CELLS * 2);
    return esp_rom_crc32_le(c, (const uint8_t *)V, CELLS * 2);
}

static bool load(void)
{
    if (!part) return false;
    int best = -1;
    save_hdr_t h, bh = {0};
    for (int s = 0; s < N_SLOTS; s++) {
        if (esp_partition_read(part, s * SLOT_BYTES, &h, sizeof h) != ESP_OK) continue;
        if (h.magic == MAGIC && (best < 0 || h.seq > bh.seq)) { best = s; bh = h; }
    }
    if (best < 0) return false;
    size_t off = best * SLOT_BYTES + sizeof h;
    esp_partition_read(part, off, U, CELLS * 2);
    esp_partition_read(part, off + CELLS * 2, V, CELLS * 2);
    if (state_crc() != bh.crc) {
        ESP_LOGW(TAG, "slot %d failed its checksum; starting fresh", best);
        return false;
    }
    gen = bh.gen; seeds = bh.seeds; save_seq = bh.seq; palette = bh.palette;
    ESP_LOGI(TAG, "restored slot %d: gen %llu, %lu seeds", best, (unsigned long long)gen, (unsigned long)seeds);
    return true;
}

static void save(void)
{
    if (!part) return;
    save_hdr_t h = {.magic = MAGIC, .seq = ++save_seq, .seeds = seeds, .gen = gen, .palette = palette};
    h.crc = state_crc();
    size_t off = (h.seq % N_SLOTS) * SLOT_BYTES;
    if (esp_partition_erase_range(part, off, SLOT_BYTES) != ESP_OK) return;
    esp_partition_write(part, off + sizeof h, U, CELLS * 2);
    esp_partition_write(part, off + sizeof h + CELLS * 2, V, CELLS * 2);
    esp_partition_write(part, off, &h, sizeof h);           // header last: a torn save stays invalid
    ESP_LOGI(TAG, "saved gen %llu to slot %lu", (unsigned long long)gen, (unsigned long)(h.seq % N_SLOTS));
}

static void sim_task(void *arg)
{
    TickType_t last_save = xTaskGetTickCount();
    for (;;) {
        seed_t s;
        while (xQueueReceive(seed_q, &s, 0)) put_seed(s.x, s.y, s.r);
        for (int i = 0; i < 4; i++) step();
        if (xTaskGetTickCount() - last_save > pdMS_TO_TICKS(SAVE_EVERY_MS)) {
            save();
            last_save = xTaskGetTickCount();
        }
        vTaskDelay(1);                                         // let the idle task feed the watchdog
    }
}

// ---- UI ----
static void fmt_gen(char *b, size_t n)
{
    uint64_t g = gen;
    if (g >= 1000000) snprintf(b, n, "GEN %llu.%lluM   %lu seeds", (unsigned long long)(g / 1000000), (unsigned long long)(g / 100000 % 10), (unsigned long)seeds);
    else snprintf(b, n, "GEN %lluK   %lu seeds", (unsigned long long)(g / 1000), (unsigned long)seeds);
}

static void show_overlay(void)
{
    last_touch = lv_tick_get();
    lv_obj_set_style_opa(overlay, LV_OPA_COVER, 0);
}

static void frame(lv_timer_t *t)
{
    uint16_t *px = (uint16_t *)buf->data;
    const int16_t *v = V;
    for (int i = 0; i < CELLS; i++) {
        int idx = v[i] >> 5;                                   // V rarely exceeds 0.45: stretch it
        px[i] = lut[idx > 255 ? 255 : idx];
    }
    lv_obj_invalidate(img);
    char b[64];
    fmt_gen(b, sizeof b);
    lv_label_set_text(gen_lbl, b);
    uint32_t idle = lv_tick_elaps(last_touch);
    if (idle > 6000 && idle < 7200) lv_obj_set_style_opa(overlay, (lv_opa_t)(255 - (idle - 6000) * 255 / 1200), 0);
    else if (idle >= 7200) lv_obj_set_style_opa(overlay, LV_OPA_TRANSP, 0);
}

static void hide_toast(lv_timer_t *t) { lv_obj_set_hidden(toast, true); }

static void say(const char *msg)
{
    lv_label_set_text(toast, msg);
    lv_obj_set_hidden(toast, false);
    lv_timer_t *t = lv_timer_create(hide_toast, 3500, NULL);
    lv_timer_set_repeat_count(t, 1);
}

static void on_press(lv_event_t *e)
{
    lv_point_t p;
    lv_indev_get_point(lv_indev_active(), &p);
    seed_t s = {(int16_t)(p.x * N / 480), (int16_t)(p.y * N / 480), 3};
    xQueueSend(seed_q, &s, 0);
    show_overlay();
}

static void on_long(lv_event_t *e)
{
    palette = (palette + 1) % N_PALETTES;
    build_lut();
    static const char *names[] = {"reef", "ember", "moss", "ink"};
    char b[32];
    snprintf(b, sizeof b, "palette: %s", names[palette]);
    say(b);
}

void painting_start(lv_obj_t *scr)
{
    U = heap_caps_malloc(CELLS * 2, MALLOC_CAP_8BIT);
    V = heap_caps_malloc(CELLS * 2, MALLOC_CAP_8BIT);
    U2 = heap_caps_malloc(CELLS * 2, MALLOC_CAP_8BIT);
    V2 = heap_caps_malloc(CELLS * 2, MALLOC_CAP_8BIT);
    buf = lv_draw_buf_create(N, N, LV_COLOR_FORMAT_RGB565, 0);
    if (!U || !V || !U2 || !V2 || !buf) {
        mk_label(scr, &lv_font_montserrat_16, COL_TEXT, "Living Painting: out of memory");
        return;
    }
    seed_q = xQueueCreate(16, sizeof(seed_t));
    part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, "storage");
    if (!load()) {
        for (int i = 0; i < CELLS; i++) { U[i] = ONE; V[i] = 0; }
        for (int i = 0; i < 14; i++) put_seed(esp_random() % N, esp_random() % N, 3);
    }
    build_lut();

    img = lv_image_create(scr);
    lv_image_set_src(img, buf);
    lv_obj_set_size(img, 480, 480);
    lv_image_set_inner_align(img, LV_IMAGE_ALIGN_STRETCH);
    lv_image_set_antialias(img, true);
    lv_obj_center(img);
    lv_obj_set_clickable(img, true);
    lv_obj_add_event_cb(img, on_press, LV_EVENT_SHORT_CLICKED, NULL);
    lv_obj_add_event_cb(img, on_long, LV_EVENT_LONG_PRESSED, NULL);

    overlay = lv_obj_create(scr);
    lv_obj_remove_style_all(overlay);
    lv_obj_set_size(overlay, 480, 76);
    lv_obj_align(overlay, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(overlay, LV_OPA_70, 0);
    lv_obj_set_clickable(overlay, false);
    lv_obj_t *title = mk_label(overlay, &lv_font_montserrat_16, COL_TEXT, "LIVING PAINTING");
    lv_obj_set_style_text_letter_space(title, 3, 0);
    lv_obj_set_pos(title, 24, 14);
    gen_lbl = mk_label(overlay, &lv_font_montserrat_12, COL_MUTED, "");
    lv_obj_set_pos(gen_lbl, 24, 42);
    lv_obj_t *hint = mk_label(overlay, &lv_font_montserrat_12, COL_MUTED, "tap to seed\nhold for palette");
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(hint, LV_ALIGN_TOP_RIGHT, -24, 16);

    toast = mk_label(scr, &lv_font_montserrat_16, COL_TEXT, "");
    lv_obj_set_style_bg_color(toast, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(toast, LV_OPA_70, 0);
    lv_obj_set_style_pad_all(toast, 10, 0);
    lv_obj_set_style_radius(toast, 10, 0);
    lv_obj_align(toast, LV_ALIGN_TOP_MID, 0, 24);
    lv_obj_set_hidden(toast, true);

    show_overlay();
    lv_timer_create(frame, 120, NULL);
    xTaskCreate(sim_task, "rd", 4 * 1024, NULL, 1, NULL);
}

void painting_on(const char *topic, const cJSON *msg)
{
    if (!strcmp(topic, "commit")) {
        const char *h = json_str(msg, "h", "");
        uint32_t hash = 2166136261u;
        for (const char *c = h; *c; c++) hash = (hash ^ (uint8_t)*c) * 16777619u;
        seed_t s = {(int16_t)(hash % N), (int16_t)((hash >> 8) % N), 5};
        xQueueSend(seed_q, &s, 0);
        char b[64];
        snprintf(b, sizeof b, "commit %.7s in %.24s", h, json_str(msg, "r", "?"));
        say(b);
    } else if (!strcmp(topic, "tool")) {
        const char *n = json_str(msg, "n", "");
        if ((strcmp(n, "Edit") && strcmp(n, "Write")) || lv_tick_elaps(last_tool_seed) < 2000) return;
        last_tool_seed = lv_tick_get();
        seed_t s = {(int16_t)(esp_random() % N), (int16_t)(esp_random() % N), 2};
        xQueueSend(seed_q, &s, 0);
    }
}
