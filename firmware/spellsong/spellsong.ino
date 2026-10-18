// Spellsong: environmentally influenced chiptune music with fantasy pixel art, for the M5StickS3.
//
//   KEY1 (front) tap   switch scene: Mana Sand / Scrying Orb
//   KEY1 hold          reroll the realm (new key, mode, theme)
//   KEY2 (side) tap    toggle Scry: go quiet and listen to music in the room
//   KEY2 hold          cycle volume
//
// Motion: tilt steers the sand and the melody, movement adds layers and tempo, a hard shake casts
// a spell (flourish, key change, sand burst), face down plays a lullaby.
// Nearby Wi-Fi networks (passive scan, nothing joined or stored) seed the realm, so each room has
// its own theme; more networks add ornaments and echo. Low battery slows the music.
//
// Serial (USB): "shot" dumps the framebuffer, "state" prints JSON, "imu" prints raw accel.
#include <M5Unified.h>
#include <WiFi.h>
#include "pal.h"
#include "music.h"
#include "scry.h"
#include "world.h"

static M5Canvas canvas(&M5.Display);
static uint16_t lut[C_COUNT], lutFlash[C_COUNT];
static int spellFlash = 0;
static int16_t abuf[3][256];
static int16_t mbuf[3][HOP];
static volatile bool wantScry = false;   // main loop's request
static volatile bool scrying = false;    // audio task's state
static int scene = 0;
static uint8_t volumes[3] = {80, 130, 175};
static int volIdx = 1;
static uint32_t frame = 0;

// IMU axis mapping onto the screen (display rotation 1). Flip signs here if the sand falls the wrong way.
static const int GX_AXIS = 1, GY_AXIS = 0;
static const float GX_SIGN = -1, GY_SIGN = 1;  // measured: held upright, ax = +1 g

// ---------------------------------------------------------------- audio task

static void setVolume() {
  uint8_t v = volumes[volIdx];
  if (!M5.Power.isCharging() && v > 180) v = 180;  // stay under ~75% on battery
  M5.Speaker.setVolume(v);
}

static void audioTask(void*) {
  int bi = 0;
  bool micOn = false;
  int qi = 0, done = 0;
  for (;;) {
    if (wantScry != scrying) {
      if (wantScry) {
        while (M5.Speaker.isPlaying()) vTaskDelay(2);
        M5.Speaker.end();
        M5.Mic.begin();
        scryer.reset();
        M5.Mic.record(mbuf[0], HOP, MIC_SR); M5.Mic.record(mbuf[1], HOP, MIC_SR);
        qi = 2; done = 0; micOn = true;
      } else {
        while (M5.Mic.isRecording()) vTaskDelay(2);
        M5.Mic.end(); micOn = false;
        M5.Speaker.begin(); setVolume();
        // carry the heard key and tempo into the music
        if (scry.keyRoot >= 0) {
          infl.adoptRoot = scry.keyRoot; infl.adoptMinor = scry.keyMinor;
          infl.adoptBpm = scry.bpm; infl.adopt = true;
        }
      }
      scrying = wantScry;
    }
    if (micOn) {
      while (M5.Mic.isRecording() >= 2) vTaskDelay(1);
      M5.Mic.record(mbuf[qi % 3], HOP, MIC_SR); qi++;
      scryer.feed(mbuf[done % 3], HOP); done++;
      continue;
    }
    while (M5.Speaker.isPlaying(0) >= 2) vTaskDelay(1);
    composer.render(abuf[bi], 256);
    M5.Speaker.playRaw(abuf[bi], 256, SR, false, 1, 0, false);
    bi = (bi + 1) % 3;
  }
}

// ---------------------------------------------------------------- senses

static float gLP[3] = {0, 0, 1};
static float energy = 0;
static uint32_t faceDownSince = 0;
static uint32_t lastSpell = 0;
static uint32_t lastScan = 0;
static bool scanning = false;
static uint32_t candidateRealm = 0;

static void readImu() {
  float a[3];
  if (!M5.Imu.getAccel(&a[0], &a[1], &a[2])) return;
  float mag = sqrtf(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
  for (int i = 0; i < 3; i++) gLP[i] = gLP[i] * 0.85f + a[i] * 0.15f;
  float jolt = fabsf(mag - 1.0f);
  energy = energy * 0.97f + min(1.0f, jolt * 2.5f) * 0.03f;
  infl.energy = energy;
  float gx = GX_SIGN * gLP[GX_AXIS], gy = GY_SIGN * gLP[GY_AXIS];
  setGravity(gx, gy);
  infl.tilt = constrain(gx, -1.0f, 1.0f);
  uint32_t now = millis();
  if (mag > 2.3f && now - lastSpell > 1500) { lastSpell = now; infl.spell = true; }
  if (gLP[2] < -0.8f) { if (!faceDownSince) faceDownSince = now; }
  else faceDownSince = 0;
  infl.sleepy = faceDownSince && now - faceDownSince > 1500;
}

static uint32_t fnv(const uint8_t* b, int n) { uint32_t h = 2166136261u; for (int i = 0; i < n; i++) { h ^= b[i]; h *= 16777619u; } return h; }

static void pollWifi() {
  uint32_t now = millis();
  if (!scanning && (lastScan == 0 || now - lastScan > 90000)) {
    WiFi.scanNetworks(true, false, true, 110);  // async, passive: listen only
    scanning = true; lastScan = now;
    return;
  }
  if (!scanning) return;
  int n = WiFi.scanComplete();
  if (n == WIFI_SCAN_RUNNING) return;
  scanning = false;
  if (n < 0) return;
  uint32_t seed = 0; int strong = 0;
  for (int i = 0; i < n; i++) {
    if (WiFi.RSSI(i) < -78) continue;
    seed ^= fnv(WiFi.BSSID(i), 6);  // order-independent; BSSIDs are never stored or printed
    strong++;
  }
  WiFi.scanDelete();
  infl.spirits = n;
  if (strong == 0) return;
  // only move to a new realm when two scans in a row agree
  if (seed == candidateRealm && seed != infl.realm) infl.realm = seed;
  candidateRealm = seed;
  if (infl.realm == 1) infl.realm = seed;  // first scan sets the starting realm
}

// ---------------------------------------------------------------- visuals from music

static void onMusic() {
  MusicEv e;
  while (popEv(e)) {
    switch (e.kind) {
      case EV_LEAD: {
        wispTY = map(constrain(e.midi, 57, 93), 57, 93, 52, 14);
        wispFlash = 3; wizardCast = 6;
        uint8_t c = C_NOTE + e.degree % 7;
        if (scene == 0) {
          int n = 5 + e.vel / 40;
          for (int k = 0; k < n; k++) spawn(wispX, wispY + 2, (wf() - 0.5f) * 1.2f, 0.3f + wf() * 0.6f, c, 120, 1);
          for (int k = 0; k < 3; k++) spawn(wispX, wispY, (wf() - 0.5f) * 0.8f, (wf() - 0.5f) * 0.8f, c, 24, 0);
        } else {
          Mote& m = motes[wr() % 48]; m.c = c; m.r = 14;
        }
        break;
      }
      case EV_BASS: {
        mossPulse = 4;
        // mana springs from the top corners in the chord's color
        if (scene == 0) for (int k = 0; k < 3; k++) {
          bool left = wr() & 1;
          spawn(left ? SX0 + 2 : SX1 - 3, SY0 + 1, (left ? 0.5f : -0.5f) + (wf() - 0.5f) * 0.3f, 0.2f, C_NOTE + e.degree % 7, 140, 1);
        }
        break;
      }
      case EV_KICK: orbPulse = 1; break;
      case EV_SNARE: twinkleBoost = 3; break;
      case EV_SPELL: {
        for (int k = 0; k < 40; k++) { float a = wf() * 2 * PI, s = 0.6f + wf() * 1.6f; spawn(60, 36, cosf(a) * s, sinf(a) * s, C_NOTE + wr() % 7, 30 + wr() % 20, 0); }
        // lift a third of the sand into the air
        for (int y = SY0; y < SY1; y++) for (int x = SX0; x < SX1; x++)
          if (sand[y][x] && (wr() % 3) == 0) {
            uint8_t c = sand[y][x]; if (c >= C_NOTE_DIM) c -= 7;
            sand[y][x] = 0; grainCount--;
            spawn(x, y, (wf() - 0.5f) * 2.5f, -1.0f - wf() * 2.0f, c, 150, 1);
          }
        orbPulse = 1; twinkleBoost = 20; spellFlash = 7; wizardCast = 30;
        break;
      }
      default: break;
    }
  }
}

static void onScry() {
  if (scene == 0) {
    for (int k = 0; k < NBANDS; k++) {
      float v = scry.bands[k];
      if (v > 0.55f && (wr() & 3) == 0) {
        int x = SX0 + 3 + k * (SX1 - SX0 - 6) / NBANDS;
        spawn(x, SY0 + 1, (wf() - 0.5f) * 0.4f, 0.4f, C_NOTE + (k * 7 / NBANDS), 120, 1);
      }
    }
  }
  if (scry.onset) { scry.onset = false; orbPulse = 1; twinkleBoost = 2; }
}

// ---------------------------------------------------------------- frame

static void present(int shake) {
  uint16_t* buf = (uint16_t*)canvas.getBuffer();
  const uint16_t* L = (spellFlash > 0 && (spellFlash & 1)) ? lutFlash : lut;
  if (spellFlash > 0) spellFlash--;
  for (int y = 0; y < H; y++) {
    int sy = constrain(y - shake, 0, H - 1);
    uint16_t* r0 = buf + (y * 2) * 240;
    uint16_t* r1 = r0 + 240;
    for (int x = 0; x < W; x++) { uint16_t c = L[fb[sy][x]]; r0[x * 2] = c; r0[x * 2 + 1] = c; r1[x * 2] = c; r1[x * 2 + 1] = c; }
  }
  canvas.pushSprite(0, 0);
}

static void dumpShot() {
  Serial.printf("@@SHOT %d %d\n", W, H);
  for (int i = 0; i < C_COUNT; i++) Serial.printf("%06lx%c", (unsigned long)PAL_RGB[i], i == C_COUNT - 1 ? '\n' : ' ');
  for (int y = 0; y < H; y++) { for (int x = 0; x < W; x++) Serial.printf("%02x", fb[y][x]); Serial.print('\n'); }
  Serial.println("@@END");
}

static void serialCmd() {
  static char line[32]; static int n = 0;
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') {
      line[n] = 0; n = 0;
      if (!strcmp(line, "shot")) dumpShot();
      else if (!strcmp(line, "state"))
        Serial.printf("{\"key\":\"%s %s\",\"bpm\":%d,\"intensity\":%d,\"section\":%d,\"scene\":%d,\"scry\":%d,\"heard\":\"%s\",\"heardBpm\":%d,"
                      "\"energy\":%.2f,\"sleepy\":%d,\"spirits\":%d,\"grains\":%d,\"battery\":%d,\"heap\":%u,\"psram\":%u}\n",
                      NOTE_NAMES[mstate.root], MODES[mstate.mode].name, mstate.bpm, mstate.intensity, mstate.section, scene, (int)scrying,
                      scry.keyRoot >= 0 ? NOTE_NAMES[scry.keyRoot] : "-", (int)scry.bpm, energy, (int)infl.sleepy, infl.spirits, grainCount,
                      M5.Power.getBatteryLevel(), (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getFreePsram());
      else if (!strcmp(line, "imu")) Serial.printf("{\"ax\":%.2f,\"ay\":%.2f,\"az\":%.2f}\n", gLP[0], gLP[1], gLP[2]);
      else if (!strcmp(line, "spell")) infl.spell = true;
      else if (!strcmp(line, "scry")) wantScry = !wantScry;
      else if (!strcmp(line, "scene")) scene ^= 1;
    } else if (n < 31) line[n++] = c;
  }
}

void setup() {
  auto cfg = M5.config();
  cfg.internal_mic = true;
  cfg.internal_spk = true;
  M5.begin(cfg);
  Serial.begin(115200);
  M5.Display.setRotation(1);
  M5.Display.setBrightness(110);
  canvas.setColorDepth(16);
  canvas.setPsram(true);
  canvas.createSprite(240, 136);
  for (int i = 0; i < C_COUNT; i++) {
    uint32_t c = PAL_RGB[i];
    uint16_t v = lgfx::color565((c >> 16) & 255, (c >> 8) & 255, c & 255);
    lut[i] = (v >> 8) | (v << 8);  // sprite buffers hold big-endian RGB565
    uint16_t f = lgfx::color565((((c >> 16) & 255) + 255) / 2, (((c >> 8) & 255) + 255) / 2, ((c & 255) + 255) / 2);
    lutFlash[i] = (f >> 8) | (f << 8);
  }
  M5.BtnA.setHoldThresh(700);
  M5.BtnB.setHoldThresh(700);
  initSky(); initOrb(); scryer.begin();
  wrng ^= esp_random();
  infl.realm = 1;
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  M5.Speaker.begin();
  setVolume();
  composer.begin();
  xTaskCreatePinnedToCore(audioTask, "audio", 8192, nullptr, 10, nullptr, 1);
  Serial.println("@@APP spellsong");
}

void loop() {
  M5.update();
  serialCmd();
  if (M5.BtnA.wasClicked()) scene ^= 1;
  if (M5.BtnA.wasHold()) infl.realm = infl.realm * 1103515245u + 12345u;
  if (M5.BtnB.wasClicked()) wantScry = !wantScry;
  if (M5.BtnB.wasHold()) { volIdx = (volIdx + 1) % 3; setVolume(); }

  readImu();
  pollWifi();
  static uint32_t lastBat = 0;
  static int battery = 100; static bool charging = false;
  if (millis() - lastBat > 5000 || lastBat == 0) {
    lastBat = millis();
    battery = M5.Power.getBatteryLevel(); charging = M5.Power.isCharging();
    infl.weary = battery >= 0 && battery < 20 && !charging;
    setVolume();
  }

  frame++;
  if (scrying) onScry(); else onMusic();

  // physics
  float gx = gdx, gy = gdy;
  stepParticles(gx, gy);
  stepSand((uint8_t)frame);
  if (frame % 2 == 0) stepSand((uint8_t)(frame + 128));
  if (grainCount > GRAIN_CAP) evaporate(4);
  wispY += (wispTY - wispY) * 0.15f;
  wispX = 60 + sinf(frame * 0.021f) * 34 + sinf(frame * 0.053f) * 8;
  if (frame % 5 == 0 && !scrying) spawn(wispX, wispY, (wf() - 0.5f) * 0.3f, 0.15f, C_FAIRY, 14, 0);
  if (mossPulse > 0) mossPulse--;

  // draw
  drawSky(frame);
  if (scene == 0) {
    drawRidge();
    drawFrame(mossPulse > 0);
    drawSand();
    drawParticles();
    drawWisp(frame, scrying, scry.level);
  } else {
    drawOrbScene(frame, scrying);
    drawParticles();
  }
  drawHUD(scrying, battery, charging);
  present(orbPulse > 0.85f && scene == 0 ? 1 : 0);

  static uint32_t last = 0;
  uint32_t now = millis();
  int wait = 33 - (int)(now - last);
  if (wait > 0) delay(wait);
  last = millis();
}
