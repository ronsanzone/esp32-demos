// SOUND LENS — a live spectrogram waterfall with a chromatic tuner and a level meter.
// Mics run at 16 kHz. Every 512 samples (32 ms) the audio task computes a 512-point FFT for one
// waterfall row (log-frequency, 40 Hz to 8 kHz) and an autocorrelation pitch estimate for the tuner.
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include "audio.h"
#include "screens.h"

#define RATE 16000
#define NFFT 512
#define LOG2N 9
#define WF_W 240                         // waterfall canvas, shown 2x
#define WF_H 140
#define F_MIN 40.0f
#define F_MAX 8000.0f
#define ROWQ 8
#define LMIN 10                          // pitch lags at 8 kHz: 800 Hz ...
#define LMAX 200                         // ... down to 40 Hz

static int16_t hann[NFFT];
static int32_t tw_c[NFFT / 2], tw_s[NFFT / 2];
static uint16_t col_lo[WF_W], col_hi[WF_W];
static uint16_t cmap[256];

static uint16_t rows[ROWQ][WF_W];        // producer: audio task; consumer: LVGL timer
static volatile uint32_t row_w, row_r;
static volatile float pitch_hz, level_db = -90;

static lv_obj_t *img, *note_lbl, *hz_lbl, *cents_lbl, *needle, *level_bar, *db_lbl, *status_lbl;
static lv_draw_buf_t *wf;

// ---- DSP ----
static void dsp_init(void)
{
    for (int i = 0; i < NFFT; i++) hann[i] = (int16_t)(32767 * (0.5f - 0.5f * cosf(2 * (float)M_PI * i / (NFFT - 1))));
    for (int i = 0; i < NFFT / 2; i++) {
        tw_c[i] = (int32_t)lroundf(32767 * cosf(2 * (float)M_PI * i / NFFT));
        tw_s[i] = (int32_t)lroundf(32767 * sinf(2 * (float)M_PI * i / NFFT));
    }
    float ratio = F_MAX / F_MIN, bin_hz = (float)RATE / NFFT;
    for (int c = 0; c < WF_W; c++) {
        float f0 = F_MIN * powf(ratio, (float)c / WF_W), f1 = F_MIN * powf(ratio, (float)(c + 1) / WF_W);
        int lo = (int)(f0 / bin_hz), hi = (int)(f1 / bin_hz);
        if (lo < 1) lo = 1;
        if (hi <= lo) hi = lo + 1;
        if (hi > NFFT / 2) hi = NFFT / 2;
        col_lo[c] = lo; col_hi[c] = hi;
    }
    static const uint32_t stops[] = {0x000004, 0x3B0F70, 0x8C2981, 0xDE4968, 0xFE9F6D, 0xFCFDBF};
    for (int i = 0; i < 256; i++) {
        int seg = i * 5 / 256, t = (i * 5) % 256;
        uint32_t a = stops[seg], b = stops[seg + 1];
        int r = ((a >> 16 & 255) * (256 - t) + (b >> 16 & 255) * t) >> 8;
        int g = ((a >> 8 & 255) * (256 - t) + (b >> 8 & 255) * t) >> 8;
        int bl = ((a & 255) * (256 - t) + (b & 255) * t) >> 8;
        cmap[i] = (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (bl >> 3));
    }
}

// In-place radix-2 FFT on int32 (no per-stage scaling: 16-bit input grows at most 9 bits).
static void fft(int32_t *re, int32_t *im)
{
    for (int i = 1, j = 0; i < NFFT; i++) {
        int bit = NFFT >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) {
            int32_t t = re[i]; re[i] = re[j]; re[j] = t;
            t = im[i]; im[i] = im[j]; im[j] = t;
        }
    }
    for (int len = 2; len <= NFFT; len <<= 1) {
        int half = len / 2, stride = NFFT / len;
        for (int i = 0; i < NFFT; i += len)
            for (int k = 0; k < half; k++) {
                int32_t wr = tw_c[k * stride], wi = -tw_s[k * stride];
                int a = i + k, b = a + half;
                int32_t tr = (int32_t)(((int64_t)re[b] * wr - (int64_t)im[b] * wi) >> 15);
                int32_t ti = (int32_t)(((int64_t)re[b] * wi + (int64_t)im[b] * wr) >> 15);
                re[b] = re[a] - tr; im[b] = im[a] - ti;
                re[a] += tr; im[a] += ti;
            }
    }
}

// log2(x) in Q8 (x > 0)
static int log2q8(uint32_t x)
{
    if (!x) return 0;
    int e = 31 - __builtin_clz(x);
    uint32_t frac = e >= 8 ? (x >> (e - 8)) & 0xFF : (x << (8 - e)) & 0xFF;
    return e * 256 + (int)frac;
}

// Autocorrelation pitch on 1024 samples decimated to 8 kHz. Returns Hz, or 0 when nothing is periodic.
static float detect_pitch(const int16_t *x1024)
{
    static int16_t d[512];
    int32_t mean = 0;
    for (int i = 0; i < 512; i++) { d[i] = (int16_t)((x1024[2 * i] + x1024[2 * i + 1]) / 2); mean += d[i]; }
    mean /= 512;
    int64_t e0 = 0;
    for (int i = 0; i < 512; i++) { d[i] -= mean; e0 += (int32_t)d[i] * d[i]; }
    if (e0 < 512LL * 40 * 40) return 0;                     // too quiet to judge
    static float r[LMAX + 2];
    for (int l = LMIN - 1; l <= LMAX + 1; l++) {
        int64_t s = 0, e = 0;
        for (int i = 0; i + l < 512; i++) {
            s += (int32_t)d[i] * d[i + l];
            e += (int32_t)d[i] * d[i] + (int32_t)d[i + l] * d[i + l];
        }
        r[l] = e ? 2.0f * s / e : 0;                        // normalized square difference (McLeod)
    }
    float best = 0;
    for (int l = LMIN; l <= LMAX; l++) if (r[l] > best) best = r[l];
    if (best < 0.6f) return 0;
    for (int l = LMIN; l <= LMAX; l++)                      // first peak within 90% of the best
        if (r[l] > 0.9f * best && r[l] >= r[l - 1] && r[l] >= r[l + 1]) {
            float a = r[l - 1], b = r[l], c = r[l + 1], den = a - 2 * b + c;
            float off = den != 0 ? 0.5f * (a - c) / den : 0;
            return 8000.0f / (l + off);
        }
    return 0;
}

static void audio_task(void *arg)
{
    // Work buffers live on the heap so other apps don't pay for them in .bss.
    struct { int16_t stereo[NFFT * 2], ring[1024]; int32_t re[NFFT], im[NFFT]; int mag[NFFT / 2]; } *w0 =
        heap_caps_calloc(1, sizeof *w0, MALLOC_CAP_INTERNAL);
    if (!w0) { ESP_LOGE("lens", "no memory for audio buffers"); vTaskDelete(NULL); }
    int16_t *stereo = w0->stereo, *ring = w0->ring;
    int32_t *re = w0->re, *im = w0->im;
    int *mag = w0->mag;
    int peak_ema = 0, hop = 0;
    for (;;) {
        if (!audio_mic_read(stereo, NFFT)) { vTaskDelay(pdMS_TO_TICKS(50)); continue; }
        int64_t sq = 0;
        memmove(ring, ring + NFFT, NFFT * 2);
        for (int i = 0; i < NFFT; i++) {
            int16_t s = stereo[2 * i];
            ring[NFFT + i] = s;
            sq += (int32_t)s * s;
            re[i] = ((int32_t)s * hann[i]) >> 15;
            im[i] = 0;
        }
        float rms = sqrtf((float)sq / NFFT);
        level_db = rms > 0.5f ? 20 * log10f(rms / 32768.0f) : -90;

        fft(re, im);
        int row_peak = 0;
        for (int k = 1; k < NFFT / 2; k++) {
            uint32_t a = (uint32_t)abs(re[k]), b = (uint32_t)abs(im[k]);
            uint32_t m = a > b ? a + (b * 3 >> 3) : b + (a * 3 >> 3);
            mag[k] = log2q8(m + 1);
            if (mag[k] > row_peak) row_peak = mag[k];
        }
        // Auto-range: show the top ~54 dB under a slowly tracked peak, but keep a floor so silence stays dark.
        peak_ema = peak_ema ? (peak_ema * 31 + row_peak) / 32 : row_peak;
        int top = peak_ema > 16 * 256 ? peak_ema : 16 * 256;
        int bottom = top - 9 * 256;
        uint32_t w = row_w;
        if (w - row_r < ROWQ) {
            uint16_t *row = rows[w % ROWQ];
            for (int c = 0; c < WF_W; c++) {
                int m = 0;
                for (int k = col_lo[c]; k < col_hi[c]; k++) if (mag[k] > m) m = mag[k];
                int idx = (m - bottom) * 255 / (top - bottom);
                row[c] = cmap[idx < 0 ? 0 : idx > 255 ? 255 : idx];
            }
            row_w = w + 1;
        }
        if (++hop % 2 == 0) pitch_hz = detect_pitch(ring);
    }
}

// ---- UI ----
static const char *NOTE_NAMES[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};

static void ui_tick(lv_timer_t *t)
{
    bool moved = false;
    uint16_t *px = (uint16_t *)wf->data;
    while (row_r != row_w) {
        memmove(px + WF_W, px, (WF_H - 1) * WF_W * 2);
        memcpy(px, rows[row_r % ROWQ], WF_W * 2);
        row_r++;
        moved = true;
    }
    if (moved) {
        lv_obj_invalidate(img);
    }

    static uint32_t last_text;
    if (lv_tick_elaps(last_text) < 100) return;
    last_text = lv_tick_get();
    char b[32];
    float db = level_db;
    snprintf(b, sizeof b, "%d dBFS", (int)lroundf(db));
    lv_label_set_text(db_lbl, b);
    int w = (int)((db + 80) * 432 / 80);
    lv_obj_set_width(level_bar, w < 2 ? 2 : w > 432 ? 432 : w);
    lv_obj_set_style_bg_color(level_bar, lv_color_hex(db > -6 ? 0xFF6161 : db > -20 ? 0xF5C04A : 0x4FD17F), 0);

    float f = pitch_hz;
    if (f > 0) {
        float midi = 69 + 12 * log2f(f / 440.0f);
        int n = (int)lroundf(midi);
        int cents = (int)lroundf((midi - n) * 100);
        snprintf(b, sizeof b, "%s%d", NOTE_NAMES[((n % 12) + 12) % 12], n / 12 - 1);
        lv_label_set_text(note_lbl, b);
        snprintf(b, sizeof b, "%.1f Hz", (double)f);
        lv_label_set_text(hz_lbl, b);
        snprintf(b, sizeof b, "%+d cents", cents);
        lv_label_set_text(cents_lbl, b);
        uint32_t col = abs(cents) <= 5 ? 0x4FD17F : abs(cents) <= 20 ? 0xF5C04A : 0xFF6161;
        lv_obj_set_style_bg_color(needle, lv_color_hex(col), 0);
        lv_obj_set_style_text_color(cents_lbl, lv_color_hex(col), 0);
        lv_obj_set_x(needle, 338 + cents * 236 / 100 - 2);
        lv_obj_set_hidden(needle, false);
    } else {
        lv_label_set_text(note_lbl, "--");
        lv_label_set_text(hz_lbl, "sing, whistle or play");
        lv_label_set_text(cents_lbl, "");
        lv_obj_set_hidden(needle, true);
    }
}

// Tuner scale: ticks every 10 cents across x 220..456, with the in-tune zone marked.
static void draw_scale(lv_event_t *e)
{
    lv_layer_t *L = lv_event_get_layer(e);
    lv_draw_line_dsc_t d;
    lv_draw_line_dsc_init(&d);
    for (int i = -5; i <= 5; i++) {
        int x = 338 + i * 236 / 10;
        d.p1.x = d.p2.x = x;
        d.p1.y = i ? 104 : 96; d.p2.y = 128;
        d.width = i ? 1 : 2;
        d.color = lv_color_hex(i ? 0x3A3F47 : 0x4FD17F);
        lv_draw_line(L, &d);
    }
}

static lv_obj_t *box(lv_obj_t *parent, int x, int y, int w, int h, uint32_t col)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(o, lv_color_hex(col), 0);
    lv_obj_set_clickable(o, false);
    return o;
}

void lens_start(lv_obj_t *scr)
{
    dsp_init();
    lv_obj_t *title = mk_label(scr, &lv_font_montserrat_20, COL_TEXT, "SOUND LENS");
    lv_obj_set_style_text_letter_space(title, 3, 0);
    lv_obj_set_pos(title, 24, 20);
    status_lbl = mk_label(scr, &lv_font_montserrat_12, COL_MUTED, "");
    lv_obj_align(status_lbl, LV_ALIGN_TOP_RIGHT, -24, 26);

    note_lbl = mk_label(scr, &lv_font_montserrat_48, COL_TEXT, "--");
    lv_obj_set_pos(note_lbl, 24, 62);
    hz_lbl = mk_label(scr, &lv_font_montserrat_14, COL_MUTED, "");
    lv_obj_set_pos(hz_lbl, 26, 122);

    lv_obj_t *scale = lv_obj_create(scr);
    lv_obj_remove_style_all(scale);
    lv_obj_set_size(scale, 480, 140);
    lv_obj_set_pos(scale, 0, 0);
    lv_obj_set_clickable(scale, false);
    lv_obj_add_event_cb(scale, draw_scale, LV_EVENT_DRAW_MAIN, NULL);
    needle = box(scr, 336, 90, 4, 44, 0x4FD17F);
    lv_obj_set_style_radius(needle, 2, 0);
    cents_lbl = mk_label(scr, &lv_font_montserrat_14, COL_MUTED, "");
    lv_obj_set_width(cents_lbl, 236);
    lv_obj_set_style_text_align(cents_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(cents_lbl, 220, 66);

    box(scr, 24, 156, 432, 6, 0x15181E);
    level_bar = box(scr, 24, 156, 2, 6, 0x4FD17F);
    lv_obj_set_style_radius(level_bar, 3, 0);
    db_lbl = mk_label(scr, &lv_font_montserrat_12, COL_MUTED, "");
    lv_obj_set_pos(db_lbl, 24, 138);

    wf = lv_draw_buf_create(WF_W, WF_H, LV_COLOR_FORMAT_RGB565, 0);
    if (!wf) {
        lv_label_set_text(status_lbl, "out of memory");
        return;
    }
    memset(wf->data, 0, WF_W * WF_H * 2);
    img = lv_image_create(scr);
    lv_image_set_src(img, wf);
    lv_obj_set_size(img, WF_W * 2, WF_H * 2);
    lv_image_set_inner_align(img, LV_IMAGE_ALIGN_STRETCH);
    lv_image_set_antialias(img, false);
    lv_obj_set_pos(img, 0, 176);

    static const int ticks[] = {100, 250, 500, 1000, 2000, 4000};
    static const char *names[] = {"100", "250", "500", "1k", "2k", "4k"};
    for (int i = 0; i < 6; i++) {
        int x = (int)(2 * WF_W * logf(ticks[i] / F_MIN) / logf(F_MAX / F_MIN));
        lv_obj_t *l = mk_label(scr, &lv_font_montserrat_12, COL_TEXT, names[i]);
        lv_obj_set_style_text_opa(l, LV_OPA_70, 0);
        lv_obj_set_pos(l, x + 3, 458);
        box(scr, x, 452, 1, 26, 0x8A919C);
    }

    if (audio_mic_open(RATE, 30.0f)) {
        lv_label_set_text(status_lbl, "mics live   16 kHz");
        xTaskCreate(audio_task, "lens", 6 * 1024, NULL, 4, NULL);
    } else {
        lv_label_set_text(status_lbl, "mic init failed");
        lv_obj_set_style_text_color(status_lbl, lv_color_hex(0xFF6161), 0);
    }
    lv_timer_create(ui_tick, 33, NULL);
}
