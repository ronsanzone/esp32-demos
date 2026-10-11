// SIGNAL FIELD — every transmitter the board can hear, drawn as an orb.
//   distance from the center = signal strength (close = strong), angle = a hash of the address,
//   size = how chatty it is, color = protocol: Wi-Fi cyan, Bluetooth amber, 802.15.4 (Zigbee/Thread) violet.
//   Each frame heard makes its orb's halo pulse. Tap an orb for details.
// Listen-only: Wi-Fi promiscuous mode hopping channels 1-13, a passive BLE scan, and 802.15.4 promiscuous
// receive hopping channels 11-26. Addresses are only kept in RAM and only shown as a vendor or "random".
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <esp_heap_caps.h>
#include <esp_ieee802154.h>
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "radio.h"
#include "screens.h"

static const char *TAG = "field";

#define CX 240
#define CY 246
#define R_MIN 34
#define R_MAX 196
#define MAX_DEV 80
#define FORGET_MS 90000

enum { P_WIFI, P_BLE, P_154, N_PROTO };
static const uint32_t PCOL[N_PROTO] = {0x5CC8F0, 0xF2A65A, 0xA98BF5};
static const char *PNAME[N_PROTO] = {"Wi-Fi", "Bluetooth", "802.15.4"};

typedef struct {
    uint8_t proto, alen, kind;           // kind: Wi-Fi 0=access point 1=client; BLE 1=random address
    uint8_t addr[8];
    int8_t rssi;
    char name[24];
} rx_ev_t;

typedef struct {
    bool used;
    uint8_t proto, alen, kind;
    uint8_t addr[8];
    char name[24];
    float rssi;                          // smoothed
    float angle;                         // radians
    uint32_t first_ms, last_ms;
    uint32_t frames, recent;             // recent: frames in the current 10 s window
    float rate;                          // frames per 10 s, smoothed
    int16_t x, y, r;                     // drawn position and core radius
    uint8_t pulse;                       // halo strength, decays per frame
} dev_t;

static dev_t devs[MAX_DEV];
static SemaphoreHandle_t dev_mx;
static QueueHandle_t rx_q;
static volatile uint32_t frames_total;
static volatile uint8_t wifi_ch = 1, z_ch = 11;
static bool have_wifi, have_ble, have_154;

static lv_obj_t *canvas_obj, *rate_lbl, *count_lbl[N_PROTO], *card, *card_name, *card_meta, *ch_lbl;
static int selected = -1;
static uint32_t card_until;

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

static uint32_t hash_addr(const uint8_t *a, int n)
{
    uint32_t h = 2166136261u;
    for (int i = 0; i < n; i++) h = (h ^ a[i]) * 16777619u;
    return h;
}

// ---- radio callbacks: copy the minimum into a queue; all bookkeeping happens in field_task ----
static void wifi_rx(void *buf, wifi_promiscuous_pkt_type_t type)
{
    const wifi_promiscuous_pkt_t *p = buf;
    const uint8_t *f = p->payload;
    int len = p->rx_ctrl.sig_len;
    if (len < 24) return;
    rx_ev_t ev = {.proto = P_WIFI, .alen = 6, .rssi = p->rx_ctrl.rssi};
    memcpy(ev.addr, f + 10, 6);                                      // addr2 = transmitter
    int ftype = (f[0] >> 2) & 3, sub = f[0] >> 4;
    if (ftype == 0 && (sub == 8 || sub == 5)) {                     // beacon / probe response: an access point
        ev.kind = 0;
        if (len > 38 && f[36] == 0) {                               // SSID element
            int n = f[37] < 23 ? f[37] : 23;
            if (n && f[38]) memcpy(ev.name, f + 38, n);
        }
        if (!ev.name[0]) strcpy(ev.name, "hidden network");
    } else if (ftype == 2 && (f[1] & 3) == 2) {                     // data from an AP (FromDS)
        ev.kind = 0;
    } else {
        ev.kind = 1;
        strcpy(ev.name, (ev.addr[0] & 2) ? "phone or laptop" : "Wi-Fi device");
    }
    xQueueSend(rx_q, &ev, 0);
}

static const char *ble_company(uint16_t id)
{
    switch (id) {
    case 0x004C: return "Apple device";
    case 0x0006: return "Microsoft device";
    case 0x0075: return "Samsung device";
    case 0x00E0: return "Google device";
    case 0x0171: return "Amazon device";
    case 0x0087: return "Garmin device";
    default: return NULL;
    }
}

static int ble_event(struct ble_gap_event *event, void *arg)
{
    if (event->type != BLE_GAP_EVENT_DISC) return 0;
    const struct ble_gap_disc_desc *d = &event->disc;
    rx_ev_t ev = {.proto = P_BLE, .alen = 6, .rssi = d->rssi, .kind = d->addr.type != BLE_ADDR_PUBLIC};
    memcpy(ev.addr, d->addr.val, 6);
    struct ble_hs_adv_fields fl;
    if (ble_hs_adv_parse_fields(&fl, d->data, d->length_data) == 0) {
        if (fl.name && fl.name_len) {
            int n = fl.name_len < 23 ? fl.name_len : 23;
            memcpy(ev.name, fl.name, n);
        } else if (fl.mfg_data && fl.mfg_data_len >= 2) {
            const char *c = ble_company(fl.mfg_data[0] | fl.mfg_data[1] << 8);
            if (c) strcpy(ev.name, c);
        }
    }
    if (!ev.name[0]) strcpy(ev.name, ev.kind ? "BLE device (random)" : "BLE device");
    xQueueSend(rx_q, &ev, 0);
    return 0;
}

static void ble_on_sync(void)
{
    struct ble_gap_disc_params p = {.itvl = 0x60, .window = 0x30, .passive = 1, .filter_duplicates = 0};
    int rc = ble_gap_disc(BLE_OWN_ADDR_PUBLIC, BLE_HS_FOREVER, &p, ble_event, NULL);
    if (rc) ESP_LOGW(TAG, "ble scan failed: %d", rc);
}

static void ble_host_task(void *arg)
{
    nimble_port_run();
    nimble_port_freertos_deinit();
}

// Called from the 802.15.4 ISR: parse the MAC header for the source address, then release the frame.
void esp_ieee802154_receive_done(uint8_t *frame, esp_ieee802154_frame_info_t *info)
{
    int len = frame[0];
    const uint8_t *m = frame + 1;
    if (len >= 5) {
        uint16_t fcf = m[0] | m[1] << 8;
        int dst_mode = (fcf >> 10) & 3, src_mode = (fcf >> 14) & 3, comp = (fcf >> 6) & 1;
        int o = 3;
        if (dst_mode) o += 2 + (dst_mode == 3 ? 8 : 2);
        if (src_mode && !comp) o += 2;
        int alen = src_mode == 3 ? 8 : src_mode == 2 ? 2 : 0;
        if (alen && o + alen <= len) {
            rx_ev_t ev = {.proto = P_154, .alen = (uint8_t)alen, .rssi = info->rssi};
            memcpy(ev.addr, m + o, alen);
            strcpy(ev.name, (fcf & 7) == 0 ? "802.15.4 coordinator" : "Zigbee/Thread node");
            BaseType_t woken = pdFALSE;
            xQueueSendFromISR(rx_q, &ev, &woken);
            portYIELD_FROM_ISR(woken);
        }
    }
    esp_ieee802154_receive_handle_done(frame);
}

// ---- bookkeeping ----
static void ingest(const rx_ev_t *ev)
{
    uint32_t t = now_ms();
    int free_slot = -1, oldest = 0;
    for (int i = 0; i < MAX_DEV; i++) {
        dev_t *d = &devs[i];
        if (!d->used) { if (free_slot < 0) free_slot = i; continue; }
        if (d->proto == ev->proto && d->alen == ev->alen && !memcmp(d->addr, ev->addr, ev->alen)) {
            d->rssi = d->rssi * 0.8f + ev->rssi * 0.2f;
            d->last_ms = t;
            d->frames++;
            d->recent++;
            d->pulse = 255;
            if (ev->name[0] && (!d->name[0] || ev->proto != P_WIFI || ev->kind == 0)) strlcpy(d->name, ev->name, sizeof d->name);
            if (ev->kind == 0 && ev->proto == P_WIFI) d->kind = 0;
            return;
        }
        if (devs[oldest].used && d->last_ms < devs[oldest].last_ms) oldest = i;
    }
    int i = free_slot >= 0 ? free_slot : oldest;
    dev_t *d = &devs[i];
    memset(d, 0, sizeof *d);
    d->used = true;
    d->proto = ev->proto; d->alen = ev->alen; d->kind = ev->kind;
    memcpy(d->addr, ev->addr, ev->alen);
    strlcpy(d->name, ev->name, sizeof d->name);
    d->rssi = ev->rssi;
    d->angle = (hash_addr(ev->addr, ev->alen) % 6283) / 1000.0f;
    d->first_ms = d->last_ms = t;
    d->frames = d->recent = 1;
    d->pulse = 255;
    d->x = d->y = -100;
}

static void field_task(void *arg)
{
    uint32_t last_hop = 0, last_z = 0, last_window = now_ms();
    rx_ev_t ev;
    for (;;) {
        if (xQueueReceive(rx_q, &ev, pdMS_TO_TICKS(20))) {
            xSemaphoreTake(dev_mx, portMAX_DELAY);
            do { ingest(&ev); frames_total++; } while (xQueueReceive(rx_q, &ev, 0));
            xSemaphoreGive(dev_mx);
        }
        uint32_t t = now_ms();
        if (have_wifi && t - last_hop > 250) {
            wifi_ch = wifi_ch % 13 + 1;
            radio_wifi_channel(wifi_ch);
            last_hop = t;
        }
        if (have_154 && t - last_z > 400) {
            z_ch = z_ch >= 26 ? 11 : z_ch + 1;
            esp_ieee802154_set_channel(z_ch);
            esp_ieee802154_receive();
            last_z = t;
        }
        if (t - last_window > 10000) {
            xSemaphoreTake(dev_mx, portMAX_DELAY);
            for (int i = 0; i < MAX_DEV; i++) {
                dev_t *d = &devs[i];
                if (!d->used) continue;
                d->rate = d->rate * 0.6f + d->recent * 0.4f;
                d->recent = 0;
                if (t - d->last_ms > FORGET_MS) d->used = false;
            }
            xSemaphoreGive(dev_mx);
            last_window = t;
        }
    }
}

// ---- drawing ----
static void disc(lv_layer_t *L, int cx, int cy, int r, uint32_t col, lv_opa_t opa)
{
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.bg_color = lv_color_hex(col); d.bg_opa = opa; d.radius = LV_RADIUS_CIRCLE;
    lv_area_t a = {cx - r, cy - r, cx + r, cy + r};
    lv_draw_rect(L, &d, &a);
}

static void ring(lv_layer_t *L, int cx, int cy, int r, int w, uint32_t col, lv_opa_t opa, int a0, int a1)
{
    lv_draw_arc_dsc_t a;
    lv_draw_arc_dsc_init(&a);
    a.center.x = cx; a.center.y = cy; a.radius = r; a.width = w;
    a.start_angle = a0; a.end_angle = a1;
    a.color = lv_color_hex(col); a.opa = opa;
    lv_draw_arc(L, &a);
}

static void seg(lv_layer_t *L, int x1, int y1, int x2, int y2, uint32_t col, lv_opa_t opa)
{
    lv_draw_line_dsc_t d;
    lv_draw_line_dsc_init(&d);
    d.p1.x = x1; d.p1.y = y1; d.p2.x = x2; d.p2.y = y2;
    d.width = 1; d.color = lv_color_hex(col); d.opa = opa;
    lv_draw_line(L, &d);
}

static void text(lv_layer_t *L, int x, int y, const char *s, uint32_t col, lv_opa_t opa)
{
    lv_draw_label_dsc_t d;
    lv_draw_label_dsc_init(&d);
    d.font = &lv_font_montserrat_12; d.color = lv_color_hex(col); d.opa = opa;
    d.text = s; d.text_local = 1;
    lv_area_t a = {x, y, x + 200, y + 16};
    lv_draw_label(L, &d, &a);
}

static int rssi_radius(float rssi)
{
    float t = (-rssi - 30) / 65.0f;                      // -30 dBm at the center ring, -95 at the edge
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    return R_MIN + (int)(t * (R_MAX - R_MIN));
}

static void draw_field(lv_event_t *e)
{
    lv_layer_t *L = lv_event_get_layer(e);
    // Bezel: dashed RSSI rings with labels, crosshair, and degree ticks.
    static const int ring_db[] = {-40, -55, -70, -85};
    for (int i = 0; i < 4; i++) {
        int r = rssi_radius(ring_db[i]);
        for (int a = 0; a < 360; a += 6) ring(L, CX, CY, r, 1, 0x2A3340, LV_OPA_COVER, a, a + 3);
        char b[8];
        snprintf(b, sizeof b, "%d", ring_db[i]);
        text(L, CX + 4, CY - r - 15, b, 0x5B6472, LV_OPA_COVER);
    }
    ring(L, CX, CY, R_MAX + 10, 1, 0x2A3340, LV_OPA_COVER, 0, 360);
    for (int a = 0; a < 360; a += 10) {
        float rad = a * (float)M_PI / 180;
        int r0 = R_MAX + 10, r1 = r0 + (a % 30 ? 4 : 9);
        seg(L, CX + (int)(r0 * cosf(rad)), CY + (int)(r0 * sinf(rad)), CX + (int)(r1 * cosf(rad)), CY + (int)(r1 * sinf(rad)), 0x3A4452, LV_OPA_COVER);
    }
    seg(L, CX - R_MAX, CY, CX + R_MAX, CY, 0x161C24, LV_OPA_COVER);
    seg(L, CX, CY - R_MAX, CX, CY + R_MAX, 0x161C24, LV_OPA_COVER);
    ring(L, CX, CY, 9, 2, 0xECEFF4, LV_OPA_70, 0, 360);

    xSemaphoreTake(dev_mx, portMAX_DELAY);
    // Beam to the most recently heard device.
    int newest = -1;
    for (int i = 0; i < MAX_DEV; i++)
        if (devs[i].used && devs[i].x > 0 && (newest < 0 || devs[i].last_ms > devs[newest].last_ms)) newest = i;
    if (newest >= 0 && devs[newest].pulse > 60)
        seg(L, CX, CY, devs[newest].x, devs[newest].y, PCOL[devs[newest].proto], devs[newest].pulse / 3);
    // Orbs: halo pulse, then core.
    for (int i = 0; i < MAX_DEV; i++) {
        dev_t *d = &devs[i];
        if (!d->used || d->x < 0) continue;
        uint32_t col = PCOL[d->proto];
        if (d->pulse > 8) ring(L, d->x, d->y, d->r + 3 + (255 - d->pulse) / 20, 2, col, d->pulse / 2, 0, 360);
        disc(L, d->x, d->y, d->r + 3, col, LV_OPA_20);
        disc(L, d->x, d->y, d->r, col, d->kind == 1 && d->proto == P_WIFI ? LV_OPA_60 : LV_OPA_COVER);
        if (i == selected) ring(L, d->x, d->y, d->r + 9, 1, 0xECEFF4, LV_OPA_COVER, 0, 360);
    }
    // Labels for the three strongest named access points or devices.
    int top[3] = {-1, -1, -1};
    for (int i = 0; i < MAX_DEV; i++) {
        dev_t *d = &devs[i];
        if (!d->used || d->x < 0 || !(d->proto == P_WIFI ? d->kind == 0 && strcmp(d->name, "hidden network") : d->name[0] && !strstr(d->name, "BLE device"))) continue;
        for (int k = 0; k < 3; k++)
            if (top[k] < 0 || d->rssi > devs[top[k]].rssi) {
                for (int m = 2; m > k; m--) top[m] = top[m - 1];
                top[k] = i;
                break;
            }
    }
    for (int k = 0; k < 3; k++) {
        if (top[k] < 0) continue;
        dev_t *d = &devs[top[k]];
        int dx = d->x >= CX ? 1 : -1, lx = d->x + dx * 14, ly = d->y - 14;
        seg(L, d->x + dx * (d->r + 2), d->y - 2, lx, ly, 0x5B6472, LV_OPA_COVER);
        text(L, dx > 0 ? lx + 2 : lx - 2 - (int)strlen(d->name) * 7, ly - 8, d->name, 0xC8CED6, LV_OPA_COVER);
    }
    xSemaphoreGive(dev_mx);
}

static void ui_tick(lv_timer_t *t)
{
    static uint32_t last_move, last_text, last_frames, last_frames_ms;
    uint32_t ms = now_ms();
    bool move = ms - last_move > 250;
    int counts[N_PROTO] = {0};
    xSemaphoreTake(dev_mx, portMAX_DELAY);
    for (int i = 0; i < MAX_DEV; i++) {
        dev_t *d = &devs[i];
        if (!d->used) continue;
        counts[d->proto]++;
        if (d->pulse) d->pulse = d->pulse > 24 ? d->pulse - 24 : 0;
        if (move) {
            d->angle += 0.004f;                                        // a slow collective drift
            int r = rssi_radius(d->rssi);
            d->x = CX + (int)(r * cosf(d->angle));
            d->y = CY + (int)(r * sinf(d->angle));
            float act = d->rate < 1 ? 1 : d->rate;
            d->r = 3 + (int)(log2f(act) * 1.2f);
            if (d->r > 10) d->r = 10;
        }
    }
    xSemaphoreGive(dev_mx);
    if (move) last_move = ms;
    lv_obj_invalidate(canvas_obj);

    if (ms - last_text < 1000) return;
    last_text = ms;
    char b[48];
    uint32_t f = frames_total;
    snprintf(b, sizeof b, "%lu frames/s", (unsigned long)((f - last_frames) * 1000 / (ms - last_frames_ms + 1)));
    last_frames = f; last_frames_ms = ms;
    lv_label_set_text(rate_lbl, b);
    for (int p = 0; p < N_PROTO; p++) {
        bool on = p == P_WIFI ? have_wifi : p == P_BLE ? have_ble : have_154;
        if (on) snprintf(b, sizeof b, "%s  %d", PNAME[p], counts[p]);
        else snprintf(b, sizeof b, "%s  off", PNAME[p]);
        lv_label_set_text(count_lbl[p], b);
    }
    snprintf(b, sizeof b, "wifi ch %d   15.4 ch %d", wifi_ch, z_ch);
    lv_label_set_text(ch_lbl, b);

    if (selected >= 0 && ms < card_until) {
        xSemaphoreTake(dev_mx, portMAX_DELAY);
        dev_t *d = &devs[selected];
        if (d->used) {
            lv_label_set_text(card_name, d->name);
            snprintf(b, sizeof b, "%s   %d dBm   %lu frames   seen %lus ago", PNAME[d->proto], (int)d->rssi,
                     (unsigned long)d->frames, (unsigned long)((ms - d->last_ms) / 1000));
            lv_label_set_text(card_meta, b);
        }
        xSemaphoreGive(dev_mx);
        lv_obj_set_hidden(card, false);
    } else {
        selected = -1;
        lv_obj_set_hidden(card, true);
    }
}

static void on_tap(lv_event_t *e)
{
    lv_point_t p;
    lv_indev_get_point(lv_indev_active(), &p);
    int best = -1, bd = 30 * 30;
    xSemaphoreTake(dev_mx, portMAX_DELAY);
    for (int i = 0; i < MAX_DEV; i++) {
        if (!devs[i].used || devs[i].x < 0) continue;
        int dx = devs[i].x - p.x, dy = devs[i].y - p.y;
        if (dx * dx + dy * dy < bd) { bd = dx * dx + dy * dy; best = i; }
    }
    xSemaphoreGive(dev_mx);
    selected = best;
    card_until = now_ms() + 5000;
}

void field_start(lv_obj_t *scr)
{
    dev_mx = xSemaphoreCreateMutex();
    rx_q = xQueueCreate(96, sizeof(rx_ev_t));

    canvas_obj = lv_obj_create(scr);
    lv_obj_remove_style_all(canvas_obj);
    lv_obj_set_size(canvas_obj, 480, 480);
    lv_obj_set_clickable(canvas_obj, true);
    lv_obj_add_event_cb(canvas_obj, draw_field, LV_EVENT_DRAW_MAIN, NULL);
    lv_obj_add_event_cb(canvas_obj, on_tap, LV_EVENT_CLICKED, NULL);

    lv_obj_t *title = mk_label(scr, &lv_font_montserrat_20, COL_TEXT, "SIGNAL FIELD");
    lv_obj_set_style_text_letter_space(title, 3, 0);
    lv_obj_set_pos(title, 20, 16);
    rate_lbl = mk_label(scr, &lv_font_montserrat_12, COL_MUTED, "listening...");
    lv_obj_align(rate_lbl, LV_ALIGN_TOP_RIGHT, -20, 22);
    ch_lbl = mk_label(scr, &lv_font_montserrat_12, 0x5B6472, "");
    lv_obj_align(ch_lbl, LV_ALIGN_TOP_RIGHT, -20, 40);
    for (int p = 0; p < N_PROTO; p++) {
        lv_obj_t *dot = mk_dot(scr, 8, PCOL[p]);
        lv_obj_set_pos(dot, 20, 404 + p * 22);
        count_lbl[p] = mk_label(scr, &lv_font_montserrat_12, COL_TEXT, PNAME[p]);
        lv_obj_set_pos(count_lbl[p], 34, 399 + p * 22);
    }

    card = lv_obj_create(scr);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, 300, 58);
    lv_obj_align(card, LV_ALIGN_BOTTOM_RIGHT, -14, -12);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x0E1117), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_90, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(0x2A3340), 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_radius(card, 10, 0);
    lv_obj_set_clickable(card, false);
    card_name = mk_label(card, &lv_font_montserrat_14, COL_TEXT, "");
    lv_obj_set_pos(card_name, 12, 8);
    card_meta = mk_label(card, &lv_font_montserrat_12, COL_MUTED, "");
    lv_obj_set_pos(card_meta, 12, 32);
    lv_obj_set_hidden(card, true);

    // Radios: each one is optional; whatever starts gets drawn.
    have_wifi = radio_wifi_start();
    if (have_wifi) radio_wifi_sniff(wifi_rx, WIFI_PROMIS_FILTER_MASK_MGMT | WIFI_PROMIS_FILTER_MASK_DATA);
    if (nimble_port_init() == ESP_OK) {
        ble_hs_cfg.sync_cb = ble_on_sync;
        nimble_port_freertos_init(ble_host_task);
        have_ble = true;
    }
    if (esp_ieee802154_enable() == ESP_OK) {
        esp_ieee802154_set_promiscuous(true);
        esp_ieee802154_set_rx_when_idle(true);
        esp_ieee802154_set_channel(z_ch);
        have_154 = esp_ieee802154_receive() == ESP_OK;
    }
    ESP_LOGI(TAG, "radios: wifi %d ble %d 15.4 %d, free heap %u", have_wifi, have_ble, have_154,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT));
    xTaskCreate(field_task, "field", 4 * 1024, NULL, 4, NULL);
    lv_timer_create(ui_tick, 80, NULL);
}
