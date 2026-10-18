// Scry mode: listen to music in the room through the mic.
//
// A 2048-point FFT at 16 kHz (hop 512, ~31 frames/s) feeds three things:
//  - 16 log-spaced band levels for the visuals,
//  - a slowly decaying chroma vector matched against Krumhansl-Kessler key profiles,
//  - a spectral-flux onset envelope whose autocorrelation gives the tempo.
#pragma once
#include <Arduino.h>
#include <math.h>

static const int MIC_SR = 16000;
static const int FFT_N = 2048;
static const int HOP = 512;
static const int NBANDS = 16;

struct ScryOut {
  volatile float bands[NBANDS];
  volatile float level = 0;     // 0..1 loudness after AGC
  volatile bool onset = false;  // consumed by visuals
  volatile int keyRoot = -1;    // -1 until confident
  volatile bool keyMinor = false;
  volatile float keyConf = 0;
  volatile float bpm = 0;
  volatile float chroma[12];
  volatile uint32_t frames = 0;
};
static ScryOut scry;

struct Scryer {
  float win[FFT_N];
  float ring[FFT_N];
  float re[FFT_N], im[FFT_N];
  float prevMag[FFT_N / 2];
  float chroma[12];
  int8_t binPc[FFT_N / 2];
  float binW[FFT_N / 2];
  int bandLo[NBANDS + 1];
  float agc = 0.01f;
  static const int ENV = 160;
  float env[ENV]; int envPos = 0;
  bool onsets[ENV];
  int ringFill = 0;

  void begin() {
    for (int i = 0; i < FFT_N; i++) win[i] = 0.5f - 0.5f * cosf(2 * PI * i / (FFT_N - 1));
    for (int b = 0; b < FFT_N / 2; b++) {
      float f = b * (float)MIC_SR / FFT_N;
      if (f < 90 || f > 2200) { binPc[b] = -1; binW[b] = 0; continue; }
      float midi = 69 + 12 * log2f(f / 440.0f);
      int m = (int)lroundf(midi);
      float off = fabsf(midi - m);  // 0 when the bin is centered on a semitone
      binPc[b] = ((m % 12) + 12) % 12;
      binW[b] = 1.0f - off * 1.6f;
      if (binW[b] < 0) binW[b] = 0;
    }
    for (int i = 0; i <= NBANDS; i++) {
      float f = 60.0f * powf(6000.0f / 60.0f, (float)i / NBANDS);
      bandLo[i] = max(1, (int)(f * FFT_N / MIC_SR));
    }
    reset();
  }
  void reset() {
    memset(ring, 0, sizeof(ring)); memset(prevMag, 0, sizeof(prevMag));
    memset(chroma, 0, sizeof(chroma)); memset(env, 0, sizeof(env)); memset(onsets, 0, sizeof(onsets));
    ringFill = 0; envPos = 0; agc = 0.01f;
    scry.keyRoot = -1; scry.keyConf = 0; scry.bpm = 0; scry.frames = 0;
  }

  static void fft(float* re, float* im, int n) {
    for (int i = 1, j = 0; i < n; i++) {
      int bit = n >> 1;
      for (; j & bit; bit >>= 1) j ^= bit;
      j ^= bit;
      if (i < j) { float t = re[i]; re[i] = re[j]; re[j] = t; t = im[i]; im[i] = im[j]; im[j] = t; }
    }
    for (int len = 2; len <= n; len <<= 1) {
      float ang = -2 * PI / len;
      float wr = cosf(ang), wi = sinf(ang);
      for (int i = 0; i < n; i += len) {
        float cr = 1, ci = 0;
        for (int k = 0; k < len / 2; k++) {
          float ur = re[i + k], ui = im[i + k];
          float vr = re[i + k + len / 2] * cr - im[i + k + len / 2] * ci;
          float vi = re[i + k + len / 2] * ci + im[i + k + len / 2] * cr;
          re[i + k] = ur + vr; im[i + k] = ui + vi;
          re[i + k + len / 2] = ur - vr; im[i + k + len / 2] = ui - vi;
          float t = cr * wr - ci * wi; ci = cr * wi + ci * wr; cr = t;
        }
      }
    }
  }

  void feed(const int16_t* x, int n) {
    // slide the analysis window
    memmove(ring, ring + n, (FFT_N - n) * sizeof(float));
    for (int i = 0; i < n; i++) ring[FFT_N - n + i] = x[i] / 32768.0f;
    ringFill += n;
    if (ringFill < FFT_N) return;
    for (int i = 0; i < FFT_N; i++) { re[i] = ring[i] * win[i]; im[i] = 0; }
    fft(re, im, FFT_N);

    float flux = 0, total = 0;
    for (int b = 1; b < FFT_N / 2; b++) {
      float mag = sqrtf(re[b] * re[b] + im[b] * im[b]);
      re[b] = mag;
      float d = mag - prevMag[b];
      if (d > 0 && b < 300) flux += d;
      prevMag[b] = mag;
      total += mag;
      if (binPc[b] >= 0) chroma[binPc[b]] += sqrtf(mag) * binW[b];
    }
    for (int i = 0; i < 12; i++) { chroma[i] *= 0.992f; scry.chroma[i] = chroma[i]; }

    // bands with automatic gain
    float peak = 0;
    float bl[NBANDS];
    for (int k = 0; k < NBANDS; k++) {
      float s = 0; int lo = bandLo[k], hi = max(lo + 1, bandLo[k + 1]);
      for (int b = lo; b < hi && b < FFT_N / 2; b++) s += re[b];
      bl[k] = s / (hi - lo);
      peak = max(peak, bl[k]);
    }
    agc = max(agc * 0.995f, peak);
    agc = max(agc, 0.02f);  // floor, so silence stays dark
    for (int k = 0; k < NBANDS; k++) {
      float v = bl[k] / agc;
      scry.bands[k] = scry.bands[k] * 0.5f + v * 0.5f;
    }
    scry.level = scry.level * 0.8f + min(1.0f, total / (agc * 400)) * 0.2f;

    // onset envelope and tempo
    env[envPos] = flux; envPos = (envPos + 1) % ENV;
    float mean = 0; for (int i = 0; i < ENV; i++) mean += env[i]; mean /= ENV;
    static float fluxAvg = 0;
    fluxAvg = fluxAvg * 0.9f + flux * 0.1f;
    bool on = flux > fluxAvg * 1.8f && flux > mean * 1.5f && flux > 0.5f && scry.level > 0.08f;
    if (on) scry.onset = true;
    onsets[(envPos + ENV - 1) % ENV] = on;
    scry.frames = scry.frames + 1;

    if (scry.frames % 16 == 0) { estimateKey(); estimateTempo(mean); }
  }

  void estimateKey() {
    static const float MAJ[12] = {6.35,2.23,3.48,2.33,4.38,4.09,2.52,5.19,2.39,3.66,2.29,2.88};
    static const float MIN[12] = {6.33,2.68,3.52,5.38,2.60,3.53,2.54,4.75,3.98,2.69,3.34,3.17};
    float best = -2, second = -2; int bestR = 0; bool bestMin = false;
    float cm = 0; for (int i = 0; i < 12; i++) cm += chroma[i]; cm /= 12;
    if (cm <= 1e-6f) return;
    for (int minor = 0; minor < 2; minor++) {
      const float* p = minor ? MIN : MAJ;
      float pm = 0; for (int i = 0; i < 12; i++) pm += p[i]; pm /= 12;
      for (int r = 0; r < 12; r++) {
        float num = 0, da = 0, db = 0;
        for (int i = 0; i < 12; i++) {
          float a = chroma[(i + r) % 12] - cm, b = p[i] - pm;
          num += a * b; da += a * a; db += b * b;
        }
        float c = num / sqrtf(da * db + 1e-9f);
        if (c > best) { second = best; best = c; bestR = r; bestMin = minor; }
        else if (c > second) second = c;
      }
    }
    scry.keyConf = best - second;
    if (best > 0.55f && scry.frames > 90) { scry.keyRoot = bestR; scry.keyMinor = bestMin; }
  }

  void estimateTempo(float mean) {
    // lags 10..32 frames = 187..58 BPM at 31.25 frames/s
    float bestV = 0; int bestL = 0;
    for (int L = 10; L <= 32; L++) {
      float s = 0;
      for (int i = 0; i < ENV - L; i++) {
        int a = (envPos + i) % ENV, b = (envPos + i + L) % ENV;
        s += (env[a] - mean) * (env[b] - mean);
      }
      if (s > bestV) { bestV = s; bestL = L; }
    }
    int count = 0; for (int i = 0; i < ENV; i++) count += onsets[i];
    if (count < 8) { scry.bpm = 0; return; }  // too few beats heard: no tempo, not a phantom one
    if (bestL == 0 || scry.frames < ENV) return;
    float fps = (float)MIC_SR / HOP;
    float bpm = 60.0f * fps / bestL;
    while (bpm < 72) bpm *= 2;
    while (bpm > 144) bpm /= 2;
    scry.bpm = bpm;
  }
};
static Scryer scryer;
