// The pixel world: mana sand, particles, the fairy wisp, the scrying orb and the HUD.
// Draws into fb[H][W] (palette indices); the .ino doubles it onto the panel.
#pragma once
#include <Arduino.h>
#include "pal.h"
#include "music.h"
#include "scry.h"

static uint8_t fb[H][W];

static uint32_t wrng = 0x9e3779b9;
static inline uint32_t wr() { wrng ^= wrng << 13; wrng ^= wrng >> 17; wrng ^= wrng << 5; return wrng; }
static inline float wf() { return (wr() & 0xffff) / 65536.0f; }

static inline void px(int x, int y, uint8_t c) { if ((unsigned)x < W && (unsigned)y < H) fb[y][x] = c; }
static void rect(int x, int y, int w, int h, uint8_t c) { for (int j = y; j < y + h; j++) for (int i = x; i < x + w; i++) px(i, j, c); }

static void text(int x, int y, const char* s, uint8_t c) {
  for (; *s; s++) {
    char ch = *s;
    if (ch >= 'a' && ch <= 'z') ch -= 32;
    if (ch == ' ') { x += 4; continue; }
    for (const Glyph& g : FONT) if (g.c == ch) {
      for (int r = 0; r < 5; r++) for (int k = 0; k < 3; k++) if (g.px[r * 3 + k] == '1') px(x + k, y + r, c);
      break;
    }
    x += 4;
  }
}
static int textW(const char* s) { int n = 0; for (; *s; s++) n++; return n * 4 - 1; }

static void sprite(int x, int y, const char** rows, int h, const char* keys, const uint8_t* cols, bool flip = false) {
  for (int j = 0; j < h; j++) {
    const char* r = rows[j]; int w = strlen(r);
    for (int i = 0; i < w; i++) {
      char ch = r[flip ? w - 1 - i : i];
      if (ch == '.') continue;
      for (int k = 0; keys[k]; k++) if (keys[k] == ch) { px(x + i, y + j, cols[k]); break; }
    }
  }
}

// ---------------------------------------------------------------- sky

struct Star { uint8_t x, y, phase, rate; };
static Star stars[40];
static int twinkleBoost = 0;
static void initSky() {
  for (auto& s : stars) { s.x = wr() % W; s.y = 9 + wr() % 40; s.phase = wr(); s.rate = 1 + wr() % 4; }
}
static void drawSky(uint32_t t) {
  for (int y = 0; y < H; y++) {
    uint8_t c = y < 26 ? C_SKY0 : y < 46 ? C_SKY1 : C_SKY2;
    // ordered dither between bands
    if ((y == 25 || y == 24) && ((y + 0) & 1)) c = C_SKY1;
    for (int x = 0; x < W; x++) {
      uint8_t cc = c;
      if ((y >= 22 && y < 26 && ((x + y) & 3) == 0)) cc = C_SKY1;
      if ((y >= 42 && y < 46 && ((x + y) & 3) == 0)) cc = C_SKY2;
      fb[y][x] = cc;
    }
  }
  for (auto& s : stars) {
    uint8_t v = (uint8_t)(s.phase + t * s.rate);
    bool bright = v < 30 || twinkleBoost > 0 && (s.x & 3) == 0;
    px(s.x, s.y, bright ? C_STAR : C_STAR_DIM);
    if (bright && v < 8) { px(s.x - 1, s.y, C_STAR_DIM); px(s.x + 1, s.y, C_STAR_DIM); px(s.x, s.y - 1, C_STAR_DIM); px(s.x, s.y + 1, C_STAR_DIM); }
  }
  if (twinkleBoost > 0) twinkleBoost--;
}

// ---------------------------------------------------------------- sand

static const int SX0 = 3, SX1 = W - 3, SY0 = 10, SY1 = H - 3;  // sand area [SX0,SX1) x [SY0,SY1)
static uint8_t sand[H][W];
static uint8_t moved[H][W];
static int grainCount = 0;
static const int GRAIN_CAP = 1500;
static int gdx = 0, gdy = 1;    // gravity direction on screen, one of 8
static int mossPulse = 0;

static inline bool inSand(int x, int y) { return x >= SX0 && x < SX1 && y >= SY0 && y < SY1; }
static inline bool freeCell(int x, int y) { return inSand(x, y) && sand[y][x] == 0; }

struct Particle { float x, y, vx, vy; uint8_t color, life, kind; };  // kind 0 sparkle, 1 grain in flight
static const int NPART = 240;
static Particle parts[NPART];
static int partNext = 0;
static void spawn(float x, float y, float vx, float vy, uint8_t c, uint8_t life, uint8_t kind) {
  for (int k = 0; k < NPART; k++) {
    Particle& p = parts[(partNext + k) % NPART];
    if (p.life == 0) { p = {x, y, vx, vy, c, life, kind}; partNext = (partNext + k + 1) % NPART; return; }
  }
}

static void setGravity(float gx, float gy) {
  // gx, gy: in-plane gravity in screen coordinates (g units). Flat on a desk: fall "down".
  float m = sqrtf(gx * gx + gy * gy);
  if (m < 0.5f) { gdx = 0; gdy = 1; return; }
  float a = atan2f(gy, gx);
  int oct = (int)lroundf(a / (PI / 4)) & 7;
  static const int8_t DX[8] = {1, 1, 0, -1, -1, -1, 0, 1};
  static const int8_t DY[8] = {0, 1, 1, 1, 0, -1, -1, -1};
  gdx = DX[oct]; gdy = DY[oct];
}

static void stepSand(uint8_t stamp) {
  // iterate so cells nearest the floor move first
  bool ydown = gdy > 0 || (gdy == 0 && false);
  int y0 = gdy > 0 ? SY1 - 1 : SY0, y1 = gdy > 0 ? SY0 - 1 : SY1, ys = gdy > 0 ? -1 : 1;
  bool xrev = gdx > 0 || (gdx == 0 && (stamp & 1));
  (void)ydown;
  // rotate gravity by +-45 degrees for the slide directions
  static const int8_t DX[8] = {1, 1, 0, -1, -1, -1, 0, 1};
  static const int8_t DY[8] = {0, 1, 1, 1, 0, -1, -1, -1};
  int oct = 0; for (int k = 0; k < 8; k++) if (DX[k] == gdx && DY[k] == gdy) oct = k;
  int ax = DX[(oct + 1) & 7], ay = DY[(oct + 1) & 7], bx = DX[(oct + 7) & 7], by = DY[(oct + 7) & 7];
  for (int y = y0; y != y1; y += ys) {
    for (int xi = SX0; xi < SX1; xi++) {
      int x = xrev ? (SX1 - 1 - (xi - SX0)) : xi;
      uint8_t g = sand[y][x];
      if (!g || moved[y][x] == stamp) continue;
      int nx = x + gdx, ny = y + gdy;
      if (!freeCell(nx, ny)) {
        bool first = wr() & 1;
        int cx = first ? ax : bx, cy = first ? ay : by;
        if (freeCell(x + cx, y + cy)) { nx = x + cx; ny = y + cy; }
        else if (freeCell(x + (first ? bx : ax), y + (first ? by : ay))) { nx = x + (first ? bx : ax); ny = y + (first ? by : ay); }
        else {
          // settled: bright grains slowly cool to their dim color
          if (g >= C_NOTE && g < C_NOTE + 7 && (wr() & 255) < 3) sand[y][x] = g + 7;
          continue;
        }
      }
      sand[ny][nx] = g; sand[y][x] = 0; moved[ny][nx] = stamp;
    }
  }
}

static void evaporate(int n) {
  // over the cap: lift grains off the surface as sparkles
  for (int tries = 0; tries < n * 30 && n > 0; tries++) {
    int x = SX0 + wr() % (SX1 - SX0), y = SY0 + wr() % (SY1 - SY0);
    if (!sand[y][x]) continue;
    if (!freeCell(x - gdx, y - gdy)) continue;
    uint8_t c = sand[y][x]; if (c >= C_NOTE_DIM) c -= 7;
    sand[y][x] = 0; grainCount--; n--;
    spawn(x, y, (wf() - 0.5f) * 0.3f, -0.3f - wf() * 0.3f, c, 30 + wr() % 20, 0);
  }
}

static void depositGrain(Particle& p) {
  int x = (int)p.x, y = (int)p.y;
  for (int k = 0; k < 6; k++) {
    if (freeCell(x, y)) { sand[y][x] = p.color; grainCount++; return; }
    x -= gdx; y -= gdy;
  }
}

static void stepParticles(float gxs, float gys) {
  for (auto& p : parts) {
    if (!p.life) continue;
    p.life--;
    if (p.kind == 1) {
      p.vx += gxs * 0.06f; p.vy += gys * 0.06f;
      p.vx *= 0.98f; p.vy *= 0.98f;
      float nx = p.x + p.vx, ny = p.y + p.vy;
      int ix = (int)nx, iy = (int)ny;
      if (!inSand(ix, iy) || sand[iy][ix] || p.life == 0) {
        if (!inSand(ix, iy)) { nx = constrain(nx, SX0, SX1 - 1); ny = constrain(ny, SY0, SY1 - 1); }
        p.x = nx; p.y = ny; depositGrain(p); p.life = 0; continue;
      }
      p.x = nx; p.y = ny;
    } else {
      p.x += p.vx; p.y += p.vy; p.vy *= 0.97f; p.vx *= 0.97f;
    }
  }
}

static void drawParticles() {
  for (auto& p : parts) {
    if (!p.life) continue;
    uint8_t c = p.color;
    if (p.kind == 0) c = p.life > 18 ? C_STAR : p.life > 8 ? p.color : C_STAR_DIM;
    px((int)p.x, (int)p.y, c);
  }
}

static void drawRidge() {
  // distant mountains and a lone tower, drawn behind the sand
  for (int x = 0; x < W; x++) {
    float h = 10 + 5 * sinf(x * 0.07f + 1.3f) + 3 * sinf(x * 0.19f) + 2 * sinf(x * 0.41f + 2);
    int top = H - 3 - (int)h;
    for (int y = top; y < H - 3; y++) fb[y][x] = (y == top && (x & 1)) ? C_STONE1 : C_STONE0;
  }
  const int tx = 88, ty = H - 3 - 30;
  rect(tx, ty + 6, 7, 24, C_STONE0);
  rect(tx - 1, ty + 4, 9, 3, C_STONE0);
  for (int i = 0; i < 9; i += 2) px(tx - 1 + i, ty + 3, C_STONE0);
  rect(tx + 2, ty - 2, 3, 5, C_STONE0); px(tx + 3, ty - 3, C_STONE0);
  px(tx + 3, ty + 10, C_GOLD2); px(tx + 3, ty + 16, C_GOLD1);  // lit windows
}

static void drawFrame(uint8_t mossBright) {
  // stone walls with moss along the floor
  for (int x = 0; x < W; x++) {
    for (int y = 8; y < 10; y++) fb[y][x] = ((x + y) % 7 == 0) ? C_STONE2 : C_STONE1;
    for (int y = H - 3; y < H; y++) fb[y][x] = ((x * 3 + y) % 5 == 0) ? C_STONE2 : C_STONE0;
    if (((x * 7) % 11) < 6) px(x, H - 3, mossBright ? C_MOSS1 : C_MOSS0);
    if (((x * 5) % 13) < 3) px(x, H - 4 + 0, (sand[H - 4][x] ? fb[H - 4][x] : (mossBright ? C_MOSS1 : C_MOSS0)));
  }
  for (int y = 8; y < H; y++) for (int x = 0; x < 3; x++) {
    fb[y][x] = ((y + x * 2) % 6 == 0) ? C_STONE2 : C_STONE1;
    fb[y][W - 1 - x] = ((y + x * 3) % 6 == 0) ? C_STONE2 : C_STONE1;
  }
}

static void drawSand() {
  for (int y = SY0; y < SY1; y++) for (int x = SX0; x < SX1; x++) if (sand[y][x]) fb[y][x] = sand[y][x];
}

// ---------------------------------------------------------------- wisp

static float wispX = 60, wispY = 30, wispTY = 30;
static int wispFlash = 0;
static void drawWisp(uint32_t t, bool eye, float level) {
  int x = (int)wispX, y = (int)wispY;
  if (eye) {
    static const char keys[] = "wpk";
    uint8_t cols[] = {C_STAR, (uint8_t)(level > 0.5f ? C_NOTE + 6 : C_NOTE + 5), C_BLACK};
    sprite(60 - 4, 12, EYE, 5, keys, cols);
    return;
  }
  bool wingUp = (t / 3) & 1;
  uint8_t glow = wispFlash > 0 ? C_STAR : C_FAIRY;
  for (int dy = -2; dy <= 2; dy++) for (int dx = -2; dx <= 2; dx++) {
    int d = abs(dx) + abs(dy);
    if (d <= 1) px(x + dx, y + dy, glow);
    else if (d == 2 && ((t + dx + dy) & 1)) px(x + dx, y + dy, C_NOTE + 4);
  }
  px(x - 3, y + (wingUp ? -2 : -1), C_STAR_DIM); px(x - 4, y + (wingUp ? -3 : -1), C_STAR_DIM);
  px(x + 3, y + (wingUp ? -2 : -1), C_STAR_DIM); px(x + 4, y + (wingUp ? -3 : -1), C_STAR_DIM);
  if (wispFlash > 0) wispFlash--;
}

// ---------------------------------------------------------------- orb scene

struct Mote { float a, r, w; uint8_t c; };
static Mote motes[48];
static float orbPulse = 0;
static int wizardCast = 0;
static void initOrb() {
  for (auto& m : motes) { m.a = wf() * 2 * PI; m.r = 3 + wf() * 12; m.w = 0.02f + wf() * 0.05f; m.c = C_NOTE + wr() % 7; }
}
static void drawOrbScene(uint32_t t, bool scrying) {
  const int cx = 70, cy = 34, R = 17;
  // floor bricks
  for (int y = H - 12; y < H; y++) for (int x = 0; x < W; x++) {
    bool mortar = (y - (H - 12)) % 4 == 3 || ((x + ((y - (H - 12)) / 4) * 4) % 9 == 0);
    fb[y][x] = mortar ? C_STONE0 : ((x + y) % 5 ? C_STONE1 : C_STONE2);
  }
  // pedestal (gold claw)
  rect(cx - 6, cy + R + 1, 13, 2, C_GOLD1);
  rect(cx - 4, cy + R + 3, 9, 5, C_GOLD0);
  rect(cx - 7, cy + R + 8, 15, 2, C_GOLD1);
  px(cx - 7, cy + R - 1, C_GOLD2); px(cx + 7, cy + R - 1, C_GOLD2); px(cx - 6, cy + R, C_GOLD1); px(cx + 6, cy + R, C_GOLD1);
  // glow ring on the beat
  if (orbPulse > 0.02f) {
    int rr = R + 2 + (int)((1 - orbPulse) * 10);
    for (int k = 0; k < 64; k++) {
      float a = k * 2 * PI / 64;
      if ((k + t) % 3 == 0) px(cx + (int)(cosf(a) * rr), cy + (int)(sinf(a) * rr * 0.95f), orbPulse > 0.5f ? C_FAIRY : C_STAR_DIM);
    }
    orbPulse *= 0.88f;
  }
  // glass
  for (int y = -R; y <= R; y++) for (int x = -R; x <= R; x++) {
    int d2 = x * x + y * y;
    if (d2 > R * R) continue;
    uint8_t c = d2 > (R - 2) * (R - 2) ? C_ORB1 : C_ORB0;
    if (d2 > (R - 1) * (R - 1) && x < 0 && y < 0) c = C_ORB_RIM;
    fb[cy + y][cx + x] = c;
  }
  // motes swirling inside
  float spin = mstate.bpm / 96.0f;
  for (auto& m : motes) {
    float rr = m.r;
    if (scrying) {
      int band = (int)(m.w * 300) % NBANDS;
      rr = 2 + min(1.0f, (float)scry.bands[band]) * (R - 4);
      m.a += m.w * (0.5f + scry.level * 3);
    } else m.a += m.w * spin;
    int x = cx + (int)(cosf(m.a) * rr), y = cy + (int)(sinf(m.a) * rr * 0.9f);
    if ((x - cx) * (x - cx) + (y - cy) * (y - cy) < (R - 1) * (R - 1)) px(x, y, m.c);
    if (m.r > 3) m.r -= 0.02f;
  }
  px(cx - 7, cy - 9, C_STAR); px(cx - 8, cy - 8, C_ORB_RIM); px(cx - 6, cy - 10, C_ORB_RIM);  // glint
  // wizard
  static const char wkeys[] = "hsfebrkt";
  uint8_t staffTip = wizardCast > 0 ? C_STAR : C_GOLD1;
  uint8_t wcols[] = {C_NOTE + 6, C_GOLD2, C_SKIN, C_BLACK, C_BEARD, C_NOTE + 5, C_STONE0, C_GOLD0};
  int wy = H - 12 - 16;
  sprite(18, wy, WIZARD, 16, wkeys, wcols);
  px(28, wy + 5, staffTip); px(27, wy + 5, wizardCast > 0 ? C_FAIRY : C_GOLD0); px(29, wy + 5, wizardCast > 0 ? C_FAIRY : C_GOLD0);
  if (wizardCast > 0) { px(28, wy + 4, C_FAIRY); wizardCast--; }
  // candles
  for (int side = 0; side < 2; side++) {
    int x = side ? 106 : 8, y = H - 12 - 7;
    rect(x, y, 3, 7, C_WAX);
    bool f = ((t / 4) + side) & 1;
    px(x + 1, y - 1, C_FLAME0); px(x + 1, y - 2, f ? C_FLAME1 : C_FLAME0); if (f) px(x + (side ? 2 : 0), y - 2, C_FLAME0);
    px(x + 1, y - 3, f ? C_FLAME0 : C_FLAME1);
  }
}

// ---------------------------------------------------------------- HUD

static void drawHUD(bool scrying, int battery, bool charging) {
  rect(0, 0, W, 8, C_VOID);
  for (int x = 0; x < W; x++) px(x, 7, C_STONE1);
  char buf[32];
  if (scrying) {
    if (scry.keyRoot >= 0) snprintf(buf, sizeof(buf), "%s %s %d", NOTE_NAMES[scry.keyRoot], scry.keyMinor ? "MINOR" : "MAJOR", (int)scry.bpm);
    else snprintf(buf, sizeof(buf), "SCRYING...");
    text(2, 1, buf, C_NOTE + 4);
  } else if (mstate.lullaby) {
    text(2, 1, "ZZZ", C_STAR_DIM);
  } else {
    snprintf(buf, sizeof(buf), "%s %s", NOTE_NAMES[mstate.root], MODES[mstate.mode].name);
    text(2, 1, buf, C_GOLD2);
    // intensity runes
    int rx = textW(buf) + 6;
    for (int k = 0; k < 4; k++) {
      uint8_t c = k <= mstate.intensity ? C_NOTE + 4 : C_STONE1;
      px(rx + k * 4 + 1, 1, c); px(rx + k * 4, 2, c); px(rx + k * 4 + 2, 2, c); px(rx + k * 4 + 1, 3, c);
      px(rx + k * 4 + 1, 4, c);
    }
  }
  // battery as hearts: 3 hearts, half-heart steps
  int halves = (battery + 8) / 17; if (halves > 6) halves = 6; if (halves < 0) halves = 0;
  for (int h = 0; h < 3; h++) {
    int x = W - 2 - (3 - h) * 8;
    int full = halves - h * 2;
    static const char keys[] = "r";
    uint8_t c[] = {(uint8_t)(full >= 2 ? C_HEART : C_NOTE_DIM)};
    sprite(x, 1, HEART, 6, keys, c);
    if (full == 1) for (int j = 0; j < 6; j++) for (int i = 0; i < 4; i++) if (HEART[j][i] == 'r') px(x + i, 1 + j, C_HEART);
  }
  if (charging) { px(W - 28, 3, C_GOLD2); px(W - 29, 4, C_GOLD2); px(W - 28, 4, C_GOLD2); px(W - 27, 4, C_GOLD2); px(W - 28, 5, C_GOLD2); }
}
