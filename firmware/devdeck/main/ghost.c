// GHOST SENSE — motion through Wi-Fi.
// The board locks onto the strongest access point and listens to its frames on one channel. For each frame
// the radio reports channel state information (CSI): the amplitude and phase of 64 subcarriers. People
// moving in the room change the multipath, so the subcarrier amplitude profile wobbles. The screen draws
// the recent profiles as ridgelines; a motion score is the short-term wobble against a learned quiet floor.
// When the room has been still for 30 s it also tries a breathing estimate (marked experimental).
// With Wi-Fi credentials stored by `deck.py wifi`, it joins that network and pings the router 50 times a
// second; the replies give a steady CSI stream. This is the only app that connects or transmits.
// Without credentials it only listens: the strongest router, or if its frames carry no CSI (11b beacons),
// the busiest transmitter on that channel, shown as "a nearby device". Names and addresses stay in RAM.
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include "radio.h"
#include "screens.h"

static const char *TAG = "ghost";

#define ACCENT 0xA98BF5
#define NSC 50                    // frequency bands across the channel, each the average of its subcarriers
#define NPT 25                    // points per ridgeline (subcarrier pairs)
#define NRIDGE 24
#define WIN 12                    // packets in the short motion window
#define SPARK 120                 // sparkline points, one per 150 ms
#define BR_HZ 5
#define BR_N (60 * BR_HZ)         // 60 s of breathing signal
#define BR_GROUPS 4
#define RIDGE_TOP 112
#define RIDGE_GAP 11
#define X0 48
#define DX 16

typedef struct {
    uint8_t mac[6];
    uint8_t fmt;
    int8_t rssi;
    uint16_t amp[NSC];            // amplitude x8
} csi_s;

enum { S_SEARCH, S_WAIT, S_CALIB, S_STILL, S_MOTION };
static const char *STATE_TXT[] = {"SEARCHING", "LISTENING", "CALIBRATING", "STILL", "MOTION"};

// Everything big lives in one heap block so other apps don't pay for it.
typedef struct {
    SemaphoreHandle_t mx;
    // shared with the UI (under mx)
    float ridge[NRIDGE][NPT];     // newest at the highest index
    float ridge_base[NPT];        // long-term profile, to emphasize change
    float spark[SPARK];           // motion ratio history
    int state;
    float ratio, bpm;
    uint32_t pkt_rate;
    char src[64];
    uint8_t ch;
    bool quiet;                   // listen-only and no transmitter sends enough CSI
    // task-only
    float win[WIN][NSC];
    int win_n, win_i;
    float acc[NSC];
    int acc_n;
    float br[BR_GROUPS][BR_N];
    int br_i, br_n;
    float br_acc[BR_GROUPS];
    float br_tmp[BR_N];
    int br_acc_n;
} ghost_t;

static ghost_t *G;
static QueueHandle_t csi_q;
static volatile bool want_any;
static bool have_creds;
static uint8_t want_mac[6];
static volatile int lock_fmt = -1;
static volatile uint32_t csi_total, csi_queued;

static lv_obj_t *canvas, *chip, *chip_lbl, *breath_lbl, *src_lbl, *hint_lbl;

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

// ---- radio side: runs in the Wi-Fi task, so keep it short ----
static void csi_cb(void *ctx, wifi_csi_info_t *info)
{
    if (!info || !info->buf || info->len < 128) return;
    csi_total++;
    if (!want_any && memcmp(info->mac, want_mac, 6)) return;
    if (lock_fmt >= 0 && info->rx_ctrl.cur_bb_format != lock_fmt) return;
    static int64_t last;
    int64_t t = esp_timer_get_time();
    if (!want_any && t - last < 15000) return;             // cap at ~65 Hz; plenty for motion
    last = t;
    csi_s s;
    memcpy(s.mac, info->mac, 6);
    s.fmt = info->rx_ctrl.cur_bb_format;
    s.rssi = info->rx_ctrl.rssi;
    // The buffer is (imag, real) pairs: 64 subcarriers for legacy/HT, more for HE (11ax). Rather than hard-code
    // each layout, split it into NSC equal bands and average the power of the non-zero subcarriers in each;
    // guard, DC and null tones read as zero. Empty bands copy their neighbour.
    const int8_t *b = info->buf;
    int n = info->len / 2, first = info->first_word_invalid ? 2 : 0;
    if (n > 256) n = 256;
    uint16_t prev = 0;
    for (int k = 0; k < NSC; k++) {
        int lo = first + k * (n - first) / NSC, hi = first + (k + 1) * (n - first) / NSC;
        uint32_t pw = 0, cnt = 0;
        for (int sc = lo; sc < hi; sc++) {
            int im = b[2 * sc], re = b[2 * sc + 1];
            if (im | re) { pw += im * im + re * re; cnt++; }
        }
        s.amp[k] = cnt ? (uint16_t)(sqrtf((float)pw / cnt) * 8) : prev;
        prev = s.amp[k];
    }
    if (xQueueSend(csi_q, &s, 0) == pdTRUE) csi_queued++;
}

static void sniff_nop(void *buf, wifi_promiscuous_pkt_type_t type) {}

// ---- analysis ----
static void push_spark(float v)
{
    memmove(G->spark, G->spark + 1, (SPARK - 1) * sizeof(float));
    G->spark[SPARK - 1] = v;
}

static float breathing(void)
{
    // Detrend each group with a 4 s moving average, then look for an autocorrelation peak between
    // 2 s and 10 s (30 to 6 breaths per minute). Returns 0 when nothing periodic stands out.
    float *y = G->br_tmp;
    float best_r = 0, best_lag = 0;
    for (int g = 0; g < BR_GROUPS; g++) {
        const float *x = G->br[g];
        for (int i = 0; i < BR_N; i++) {
            int a = i - 10 < 0 ? 0 : i - 10, b = i + 10 >= BR_N ? BR_N - 1 : i + 10;
            float m = 0;
            for (int j = a; j <= b; j++) m += x[(G->br_i + j) % BR_N];
            y[i] = x[(G->br_i + i) % BR_N] - m / (b - a + 1);
        }
        float e = 0;
        for (int i = 0; i < BR_N; i++) e += y[i] * y[i];
        if (e < 1e-9f) continue;
        float r[52];
        for (int lag = 9; lag <= 51; lag++) {
            float s = 0;
            for (int i = 0; i + lag < BR_N; i++) s += y[i] * y[i + lag];
            r[lag] = s / e;
        }
        for (int lag = 10; lag <= 50; lag++)
            if (r[lag] > r[lag - 1] && r[lag] >= r[lag + 1] && r[lag] > best_r) {
                float a = r[lag - 1], b = r[lag], c = r[lag + 1], den = a - 2 * b + c;
                best_r = b;
                best_lag = lag + (den != 0 ? 0.5f * (a - c) / den : 0);
            }
    }
    return best_r > 0.35f && best_lag > 0 ? 60.0f * BR_HZ / best_lag : 0;
}

static void ingest(const csi_s *s, uint32_t ms)
{
    static uint32_t calib_until, still_since, last_ridge, last_br, last_bpm, quiet_ms;
    static float motion_ema, floor_v;
    float n[NSC], mean = 0;
    for (int k = 0; k < NSC; k++) mean += s->amp[k];
    mean /= NSC;
    if (mean < 8) return;                                   // nothing usable (amp x8 < 1)
    for (int k = 0; k < NSC; k++) n[k] = s->amp[k] / mean;  // removes the receiver's gain changes

    memcpy(G->win[G->win_i], n, sizeof n);
    G->win_i = (G->win_i + 1) % WIN;
    if (G->win_n < WIN) G->win_n++;
    for (int k = 0; k < NSC; k++) G->acc[k] += n[k];
    G->acc_n++;
    for (int g = 0; g < BR_GROUPS; g++) {
        float m = 0;
        for (int k = g * NSC / BR_GROUPS; k < (g + 1) * NSC / BR_GROUPS; k++) m += n[k];
        G->br_acc[g] += m / (NSC / BR_GROUPS);
    }
    G->br_acc_n++;
    if (G->win_n < WIN) return;

    // Motion score: mean over subcarriers of the short-window standard deviation.
    float score = 0;
    for (int k = 0; k < NSC; k++) {
        float m = 0, v = 0;
        for (int i = 0; i < WIN; i++) m += G->win[i][k];
        m /= WIN;
        for (int i = 0; i < WIN; i++) v += (G->win[i][k] - m) * (G->win[i][k] - m);
        score += sqrtf(v / WIN);
    }
    score /= NSC;
    motion_ema = motion_ema ? 0.8f * motion_ema + 0.2f * score : score;
    // Quiet floor: falls quickly to the quietest level seen, creeps up slowly so it can re-learn a room.
    if (!floor_v) { floor_v = motion_ema; calib_until = ms + 8000; }
    if (motion_ema < floor_v) floor_v = 0.9f * floor_v + 0.1f * motion_ema;
    else floor_v *= 1.0003f;
    float ratio = motion_ema / (floor_v + 1e-6f);

    xSemaphoreTake(G->mx, portMAX_DELAY);
    G->ratio = ratio;
    if ((int32_t)(ms - calib_until) < 0) G->state = S_CALIB;
    else if (G->state != S_MOTION && ratio > 2.5f) G->state = S_MOTION;
    else if (G->state == S_MOTION && ratio < 1.8f) {
        if (!quiet_ms) quiet_ms = ms;
        if (ms - quiet_ms > 2000) G->state = S_STILL;
    } else if (G->state != S_MOTION) G->state = S_STILL;
    if (ratio >= 1.8f) quiet_ms = 0;
    if (G->state != S_STILL) still_since = ms;

    if (ms - last_ridge >= 150 && G->acc_n) {
        last_ridge = ms;
        memmove(G->ridge[0], G->ridge[1], (NRIDGE - 1) * sizeof G->ridge[0]);
        for (int j = 0; j < NPT; j++) {
            float v = (G->acc[2 * j] + G->acc[2 * j + 1]) / (2.0f * G->acc_n);
            G->ridge[NRIDGE - 1][j] = v;
            G->ridge_base[j] = G->ridge_base[j] ? 0.98f * G->ridge_base[j] + 0.02f * v : v;
        }
        memset(G->acc, 0, sizeof G->acc);
        G->acc_n = 0;
        push_spark(ratio);
    }
    xSemaphoreGive(G->mx);

    if (ms - last_br >= 1000 / BR_HZ && G->br_acc_n) {
        last_br = ms;
        for (int g = 0; g < BR_GROUPS; g++) {
            G->br[g][G->br_i] = G->br_acc[g] / G->br_acc_n;
            G->br_acc[g] = 0;
        }
        G->br_acc_n = 0;
        G->br_i = (G->br_i + 1) % BR_N;
        if (G->br_n < BR_N) G->br_n++;
    }
    if (ms - last_bpm >= 2000) {
        last_bpm = ms;
        float bpm = G->br_n == BR_N && ms - still_since > 30000 ? breathing() : 0;
        xSemaphoreTake(G->mx, portMAX_DELAY);
        G->bpm = bpm;
        xSemaphoreGive(G->mx);
    }
}

static void set_src(const char *txt)
{
    xSemaphoreTake(G->mx, portMAX_DELAY);
    snprintf(G->src, sizeof G->src, "%s", txt);
    xSemaphoreGive(G->mx);
}

static void ghost_task(void *arg)
{
    uint8_t bssid[6], ch = 0;
    char ssid[33];
    int8_t rssi = 0;
    bool joined = false;
    if (have_creds) {
        set_src("joining your Wi-Fi...");
        joined = radio_wifi_join(bssid, ssid, &ch, 20000);
        if (!joined) {
            set_src("couldn't join the saved network; listening only");
            vTaskDelay(pdMS_TO_TICKS(2000));
        }
    }
    while (!joined && !radio_wifi_strongest_ap(bssid, ssid, &ch, &rssi)) {
        set_src("no Wi-Fi network in range, retrying");
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
    G->ch = ch;
    char b[64];
    snprintf(b, sizeof b, "%s%s", ssid[0] ? ssid : "hidden network", joined ? " (joined)" : "");
    set_src(b);
    if (joined) ESP_LOGI(TAG, "joined the saved network on channel %d", ch);            // no names in logs
    else ESP_LOGI(TAG, "locked to the strongest router on channel %d (%d dBm)", ch, rssi);

    wifi_csi_config_t cfg = {0};
    cfg.enable = 1;
    cfg.acquire_csi_legacy = 1;
    cfg.acquire_csi_ht20 = 1;
    cfg.acquire_csi_su = 1;                                // 11ax routers answer pings in HE SU
    memcpy(want_mac, bssid, 6);
    want_any = false;
    if (!joined) {
        radio_wifi_sniff(sniff_nop, WIFI_PROMIS_FILTER_MASK_MGMT | WIFI_PROMIS_FILTER_MASK_DATA);
        radio_wifi_channel(ch);
    }
    esp_wifi_set_csi_config(&cfg);
    esp_wifi_set_csi_rx_cb(csi_cb, NULL);
    esp_wifi_set_csi(true);
    if (joined && !radio_ping_gateway(20)) ESP_LOGW(TAG, "could not start pinging the router");
    xSemaphoreTake(G->mx, portMAX_DELAY);
    G->state = S_WAIT;
    xSemaphoreGive(G->mx);

    // Pick a source: the router if it gives us CSI, else the busiest transmitter on its channel.
    // Then lock to the frame format it uses most, since legacy and HT training fields differ in shape.
    uint32_t t0 = now_ms(), got = 0, last_any = now_ms(), last_rate = now_ms();
    int fmt_count[16] = {0};
    struct { uint8_t mac[6]; uint32_t n; } cand[8] = {0};
    bool fallback_tried = joined;                          // joined: the router's replies are the source
    csi_s s;
    for (;;) {
        uint32_t ms = now_ms();
        if (xQueueReceive(csi_q, &s, pdMS_TO_TICKS(200)) == pdTRUE) {
            ms = now_ms();
            if (want_any) {
                int i, free_i = -1;
                for (i = 0; i < 8; i++) {
                    if (cand[i].n && !memcmp(cand[i].mac, s.mac, 6)) { cand[i].n++; break; }
                    if (!cand[i].n && free_i < 0) free_i = i;
                }
                if (i == 8 && free_i >= 0) { memcpy(cand[free_i].mac, s.mac, 6); cand[free_i].n = 1; }
            } else if (lock_fmt < 0) {
                fmt_count[s.fmt & 15]++;
                if (++got >= 30) {
                    int best = 0;
                    for (int f = 1; f < 16; f++) if (fmt_count[f] > fmt_count[best]) best = f;
                    lock_fmt = best;
                    ESP_LOGI(TAG, "locked to frame format %d", best);
                }
            } else {
                last_any = ms;
                ingest(&s, ms);
            }
        }
        if (!want_any && lock_fmt < 0 && !fallback_tried && got < 10 && ms - t0 > 6000) {
            fallback_tried = true;
            want_any = true;                               // tally transmitters for 3 s
            t0 = ms;
            memset(cand, 0, sizeof cand);
        }
        if (want_any && ms - t0 > 3000) {
            int best = -1;
            for (int i = 0; i < 8; i++) if (cand[i].n && (best < 0 || cand[i].n > cand[best].n)) best = i;
            if (best >= 0 && cand[best].n >= 10) {
                memcpy(want_mac, cand[best].mac, 6);
                snprintf(b, sizeof b, "a nearby device (%s is quiet)", ssid[0] ? ssid : "router");
                set_src(b);
            } else {
                G->quiet = true;                           // nothing to measure without joining
            }
            memset(cand, 0, sizeof cand);                  // forget the addresses once chosen
            want_any = false;
            got = 0;
            memset(fmt_count, 0, sizeof fmt_count);
        }
        if (lock_fmt >= 0 && ms - last_any > 15000) {      // source went quiet: re-pick the format
            lock_fmt = -1;
            got = 0;
            memset(fmt_count, 0, sizeof fmt_count);
            last_any = ms;
        }
        if (ms - last_rate >= 1000) {
            xSemaphoreTake(G->mx, portMAX_DELAY);
            static uint32_t last_q;                        // counted where frames arrive, not where they're used
            uint32_t q = csi_queued;
            G->pkt_rate = (q - last_q) * 1000 / (ms - last_rate);
            last_q = q;
            xSemaphoreGive(G->mx);
            last_rate = ms;
        }
    }
}

// ---- UI ----
static lv_color_t mix(uint32_t a, uint32_t b, int t)        // t 0..255
{
    return lv_color_mix(lv_color_hex(b), lv_color_hex(a), t);
}

static void draw(lv_event_t *e)
{
    lv_layer_t *L = lv_event_get_layer(e);
    const lv_area_t *clip = &L->_clip_area;
    lv_point_precise_t pts[SPARK];
    lv_draw_line_dsc_t d;

    xSemaphoreTake(G->mx, portMAX_DELAY);
    bool moving = G->state == S_MOTION;
    // Ridgelines, oldest at the top. Each is the subcarrier profile, tapered at the edges, with
    // its change from the long-term profile exaggerated so motion reads as ripples.
    for (int i = 0; i < NRIDGE; i++) {
        int base = RIDGE_TOP + i * RIDGE_GAP;
        if (base + 10 < clip->y1 || base - 50 > clip->y2) continue;
        const float *r = G->ridge[i];
        if (!r[NPT / 2]) continue;                         // not filled yet
        for (int j = 0; j < NPT; j++) {
            float w = sinf((float)M_PI * (j + 0.5f) / NPT);
            float h = (r[j] - 1) * 26 + (r[j] - G->ridge_base[j]) * 110;
            h = h * w * w;
            if (h > 48) h = 48;
            if (h < -8) h = -8;
            pts[j].x = X0 + j * DX;
            pts[j].y = base - (int)h;
        }
        lv_draw_line_dsc_init(&d);
        d.points = pts;
        d.point_cnt = NPT;
        int t = 40 + i * 215 / (NRIDGE - 1);
        bool newest = i == NRIDGE - 1;
        d.color = newest ? lv_color_hex(moving ? 0xFFFFFF : 0xE4DAFF) : mix(0x2A2342, ACCENT, t);
        d.opa = newest ? LV_OPA_COVER : (lv_opa_t)(60 + t * 195 / 255);
        d.width = newest ? 2 : 1;
        d.round_start = d.round_end = 1;
        lv_draw_line(L, &d);
    }
    // Motion sparkline with the trigger level.
    const int sy = 432, sh = 30;
    if (clip->y2 >= sy - sh && clip->y1 <= sy) {
        lv_draw_line_dsc_init(&d);
        d.p1.x = 20; d.p1.y = sy - (int)(2.5f / 6 * sh);
        d.p2.x = 460; d.p2.y = d.p1.y;
        d.color = lv_color_hex(0x3A3F47); d.width = 1; d.dash_width = 3; d.dash_gap = 4;
        lv_draw_line(L, &d);
        int n = 0;
        for (int i = 0; i < SPARK; i++) {
            float v = G->spark[i];
            if (!v) continue;
            if (v > 6) v = 6;
            pts[n].x = 20 + i * 440 / (SPARK - 1);
            pts[n].y = sy - (int)(v / 6 * sh);
            n++;
        }
        if (n > 1) {
            lv_draw_line_dsc_init(&d);
            d.points = pts;
            d.point_cnt = n;
            d.color = lv_color_hex(moving ? 0xF5C04A : ACCENT);
            d.width = 2;
            d.round_start = d.round_end = 1;
            lv_draw_line(L, &d);
        }
    }
    xSemaphoreGive(G->mx);
}

static void ui_tick(lv_timer_t *t)
{
    lv_obj_invalidate(canvas);
    static uint32_t last_text;
    static int shown_state = -1;
    if (lv_tick_elaps(last_text) < 500) return;
    last_text = lv_tick_get();

    xSemaphoreTake(G->mx, portMAX_DELAY);
    int st = G->state;
    float bpm = G->bpm;
    uint32_t rate = G->pkt_rate;
    char src[64];
    memcpy(src, G->src, sizeof src);
    uint8_t ch = G->ch;
    xSemaphoreGive(G->mx);

    if (st != shown_state) {
        shown_state = st;
        lv_label_set_text(chip_lbl, STATE_TXT[st]);
        uint32_t col = st == S_MOTION ? 0xF5C04A : st == S_STILL ? ACCENT : 0x5B6472;
        lv_obj_set_style_border_color(chip, lv_color_hex(col), 0);
        lv_obj_set_style_text_color(chip_lbl, lv_color_hex(col), 0);
        lv_obj_set_style_bg_opa(chip, st == S_MOTION ? LV_OPA_30 : LV_OPA_TRANSP, 0);
        lv_obj_set_style_bg_color(chip, lv_color_hex(col), 0);
        lv_obj_set_hidden(hint_lbl, st >= S_STILL);
        if (st == S_CALIB) lv_label_set_text(hint_lbl, "learning the room, keep still for a moment");
    }
    static int shown_quiet = -1;
    if (st == S_WAIT && G->quiet != shown_quiet) {
        shown_quiet = G->quiet;
        lv_label_set_text(hint_lbl, G->quiet ? "Too little Wi-Fi traffic to measure.\nRun deck.py wifi so Ghost Sense can join and ping your router."
                                             : "waiting for frames...");
    }
    char b[96];
    if (bpm > 0) snprintf(b, sizeof b, "breathing  ~%d /min   experimental", (int)lroundf(bpm));
    else snprintf(b, sizeof b, "breathing  --   (needs 30 s of stillness)");
    lv_label_set_text(breath_lbl, b);
    if (ch) snprintf(b, sizeof b, "%s   ch %d   %lu pkt/s", src, ch, (unsigned long)rate);
    else snprintf(b, sizeof b, "%s", src[0] ? src : "scanning for the strongest router...");
    lv_label_set_text(src_lbl, b);
}

void ghost_start(lv_obj_t *scr)
{
    G = heap_caps_calloc(1, sizeof *G, MALLOC_CAP_INTERNAL);
    if (!G) {
        mk_label(scr, &lv_font_montserrat_16, COL_TEXT, "Ghost Sense: not enough memory");
        return;
    }
    G->mx = xSemaphoreCreateMutex();
    G->state = S_SEARCH;
    csi_q = xQueueCreate(24, sizeof(csi_s));

    canvas = lv_obj_create(scr);
    lv_obj_remove_style_all(canvas);
    lv_obj_set_size(canvas, 480, 480);
    lv_obj_set_clickable(canvas, false);
    lv_obj_add_event_cb(canvas, draw, LV_EVENT_DRAW_MAIN, NULL);

    lv_obj_t *title = mk_label(scr, &lv_font_montserrat_20, COL_TEXT, "GHOST SENSE");
    lv_obj_set_style_text_letter_space(title, 3, 0);
    lv_obj_set_pos(title, 20, 16);
    lv_obj_t *sub = mk_label(scr, &lv_font_montserrat_12, COL_MUTED, "motion through Wi-Fi");
    lv_obj_set_pos(sub, 21, 42);

    chip = lv_obj_create(scr);
    lv_obj_remove_style_all(chip);
    lv_obj_set_size(chip, 124, 28);
    lv_obj_align(chip, LV_ALIGN_TOP_RIGHT, -18, 18);
    lv_obj_set_style_radius(chip, 14, 0);
    lv_obj_set_style_border_width(chip, 1, 0);
    lv_obj_set_style_border_color(chip, lv_color_hex(0x5B6472), 0);
    lv_obj_set_clickable(chip, false);
    chip_lbl = mk_label(chip, &lv_font_montserrat_12, 0x5B6472, STATE_TXT[S_SEARCH]);
    lv_obj_set_style_text_letter_space(chip_lbl, 2, 0);
    lv_obj_center(chip_lbl);

    hint_lbl = mk_label(scr, &lv_font_montserrat_14, COL_MUTED, "finding the strongest router...");
    lv_obj_set_width(hint_lbl, 440);
    lv_obj_set_style_text_align(hint_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(hint_lbl, 20, 230);

    breath_lbl = mk_label(scr, &lv_font_montserrat_14, COL_TEXT, "");
    lv_obj_set_pos(breath_lbl, 20, 440);
    src_lbl = mk_label(scr, &lv_font_montserrat_12, COL_MUTED, "");
    lv_obj_set_pos(src_lbl, 20, 460);

    have_creds = radio_creds_present();
    if (!radio_wifi_start(have_creds)) {
        lv_label_set_text(hint_lbl, "Wi-Fi radio failed to start");
        return;
    }
    ESP_LOGI(TAG, "free heap %u", (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT));
    xTaskCreate(ghost_task, "ghost", 6 * 1024, NULL, 3, NULL);
    lv_timer_create(ui_tick, 150, NULL);
}
