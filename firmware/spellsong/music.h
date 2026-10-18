// Chiptune synth and generative composer.
//
// Four NES-style voices (two pulse leads, triangle bass, noise/kick drums) render at 22.05 kHz.
// The composer works in scale degrees of the current mode, so every note it emits is in key:
// chord tones land on the beat, passing tones move by step, and motifs repeat in an A A' B A form.
#pragma once
#include <Arduino.h>
#include <math.h>

static const int SR = 22050;

// ---------------------------------------------------------------- shared state

// What the senses tell the music. Written by the main loop, read by the audio task.
struct Influence {
  volatile float tilt = 0;        // -1..1, left/right tilt: nudges melody contour
  volatile float energy = 0;      // 0..1, smoothed motion
  volatile bool spell = false;    // a shake happened; composer clears it
  volatile bool sleepy = false;   // face down
  volatile bool weary = false;    // battery low and not charging
  volatile int spirits = 0;       // nearby Wi-Fi networks
  volatile uint32_t realm = 1;    // seed for key, mode, progression
  volatile bool adopt = false;    // take key/mode/tempo heard in Scry
  volatile int adoptRoot = 0;
  volatile bool adoptMinor = false;
  volatile float adoptBpm = 0;
};
static Influence infl;

// What the music tells the visuals.
enum EvKind : uint8_t { EV_LEAD, EV_HARM, EV_BASS, EV_KICK, EV_SNARE, EV_HAT, EV_CHORD, EV_SPELL, EV_REALM };
struct MusicEv { uint8_t kind, midi, degree, vel; };
static const int EVQ = 64;
static MusicEv evq[EVQ];
static volatile uint32_t evHead = 0, evTail = 0;
static void pushEv(uint8_t k, uint8_t midi, uint8_t deg, uint8_t vel) {
  uint32_t h = evHead;
  if (h - evTail >= EVQ) return;  // visuals fell behind; drop
  evq[h % EVQ] = {k, midi, deg, vel};
  evHead = h + 1;
}
static bool popEv(MusicEv& e) {
  if (evTail == evHead) return false;
  e = evq[evTail % EVQ];
  evTail = evTail + 1;
  return true;
}

// Snapshot for the HUD.
struct MusicState {
  volatile int root = 9;      // pitch class of the key
  volatile int mode = 0;      // index into MODES
  volatile int bpm = 96;
  volatile int intensity = 0; // 0..3
  volatile int chordDeg = 0;  // scale degree of the current chord
  volatile int section = 0;   // 0 A, 1 A', 2 B, 3 A
  volatile bool lullaby = false;
};
static MusicState mstate;

struct Mode { const char* name; int8_t steps[7]; bool minor; const int8_t* progs; int nprogs; };
// Progressions are four chords, given as scale degrees (0 = tonic).
static const int8_t P_AEOLIAN[]    = {0,5,2,6,  0,6,5,6,  0,3,5,4};
static const int8_t P_DORIAN[]     = {0,3,0,6,  0,2,3,0,  0,6,3,4};
static const int8_t P_LYDIAN[]     = {0,1,0,1,  0,1,4,0,  0,6,1,4};
static const int8_t P_MIXOLYDIAN[] = {0,6,3,0,  0,3,6,0,  0,4,6,3};
static const int8_t P_IONIAN[]     = {0,4,5,3,  0,3,5,4,  0,5,3,4};
static const int8_t P_PHRYGIAN[]   = {0,1,0,6,  0,1,2,1,  0,5,1,0};
static const Mode MODES[] = {
  {"AEOLIAN",    {0,2,3,5,7,8,10}, true,  P_AEOLIAN, 3},
  {"DORIAN",     {0,2,3,5,7,9,10}, true,  P_DORIAN, 3},
  {"LYDIAN",     {0,2,4,6,7,9,11}, false, P_LYDIAN, 3},
  {"MIXOLYDIAN", {0,2,4,5,7,9,10}, false, P_MIXOLYDIAN, 3},
  {"IONIAN",     {0,2,4,5,7,9,11}, false, P_IONIAN, 3},
  {"PHRYGIAN",   {0,1,3,5,7,8,10}, true,  P_PHRYGIAN, 3},
};
static const int NMODES = sizeof(MODES) / sizeof(MODES[0]);
static const char* NOTE_NAMES[12] = {"C","C#","D","D#","E","F","F#","G","G#","A","A#","B"};

// ---------------------------------------------------------------- synth

static float midiHz(int m) { return 440.0f * powf(2.0f, (m - 69) / 12.0f); }

struct Tone {          // pulse or triangle voice
  uint32_t phase = 0, inc = 0;
  float duty = 0.5f;
  float env = 0, peak = 0, sustain = 0, decay = 0.9995f, release = 0.999f;
  int gate = 0;        // samples left before release
  bool tri = false;
  float vib = 0; int vibDelay = 0; uint32_t age = 0;
  void on(int midi, float vel, int lenSamples, float sus, float dec) {
    inc = (uint32_t)(midiHz(midi) * 4294967296.0f / SR);
    peak = vel; env = vel; sustain = vel * sus; decay = dec; gate = lenSamples; age = 0;
  }
  inline float next() {
    if (env < 0.0005f && gate <= 0) return 0;
    age++;
    uint32_t inc2 = inc;
    if (vib > 0 && age > (uint32_t)vibDelay) {
      float v = sinf(age * (2.0f * PI * 5.5f / SR)) * vib;
      inc2 = (uint32_t)(inc * (1.0f + v));
    }
    phase += inc2;
    float s;
    if (tri) {  // 4-bit stepped triangle, like the NES
      int p = phase >> 27;  // 0..31
      int lv = p < 16 ? p : 31 - p;
      s = (lv - 7.5f) / 7.5f;
    } else {
      s = (phase < (uint32_t)(duty * 4294967295.0f)) ? 1.0f : -1.0f;
    }
    if (gate > 0) { gate--; if (env > sustain) env *= decay; }
    else env *= release;
    return s * env;
  }
};

struct Drum {          // kick (pitch-dropping sine), snare and hat (LFSR noise)
  uint16_t lfsr = 1; int kind = 0; float env = 0, decay = 0.999f;
  float kphase = 0, kfreq = 0; int counter = 0, period = 1; float hold = 0;
  void hit(int k, float vel) {
    kind = k; env = vel;
    if (k == 0) { kfreq = 160; kphase = 0; decay = 0.99955f; }
    else if (k == 1) { period = 2; decay = 0.9993f; }
    else { period = 1; decay = 0.9965f; }
  }
  inline float next() {
    if (env < 0.0005f) return 0;
    float s;
    if (kind == 0) {
      kfreq = 45 + (kfreq - 45) * 0.9992f;
      kphase += kfreq / SR; if (kphase > 1) kphase -= 1;
      s = sinf(kphase * 2 * PI);
    } else {
      if (++counter >= period) {
        counter = 0;
        int bit = kind == 2 ? ((lfsr ^ (lfsr >> 6)) & 1) : ((lfsr ^ (lfsr >> 1)) & 1);
        lfsr = (lfsr >> 1) | (bit << 14);
        hold = (lfsr & 1) ? 1.0f : -1.0f;
      }
      s = hold;
    }
    env *= decay;
    return s * env;
  }
};

// ---------------------------------------------------------------- composer

struct Composer {
  Tone lead, harm, bass;
  Drum drum;
  static const int ECHO = SR / 2;
  int16_t echo[ECHO]; int echoPos = 0, echoLen = SR * 3 / 8;

  uint32_t rng = 12345;
  uint32_t rnd() { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }
  float frand() { return (rnd() & 0xffffff) / 16777216.0f; }

  int root = 9, mode = 0, prog = 0, baseBpm = 96;
  float bpm = 96; float leadDuty = 0.25f;
  int sub = 0;              // 32nd-note position in the bar, 0..31
  int bar = 0;
  int samplesToTick = 0;
  int intensity = 0;
  // motif: 32 sixteenths (two bars). value: offset in scale steps from chord root, REST, or HOLD.
  static const int8_t REST = -100, HOLD = -101;
  int8_t motifA[32], motifB[32], motif[32];
  int lastLeadDeg = 4;
  int flourish = 0, flourishBase = 0;
  int pendingKeyShift = 0;
  uint32_t pendingRealm = 0, realmSeen = 0;

  int degToMidi(int deg, int octaveBase) {
    int o = (deg >= 0) ? deg / 7 : -((-deg + 6) / 7);
    int d = deg - o * 7;
    return octaveBase + root + MODES[mode].steps[d] + 12 * o;
  }
  int chordDegAt(int b) { return MODES[mode].progs[prog * 4 + (b % 4)]; }

  void makeMotif(int8_t* m, float density) {
    // rhythm: choose onsets on a 16th grid, strong beats favored
    int cur = (rnd() % 3) * 2;  // start on root, third or fifth
    for (int i = 0; i < 32; i++) {
      bool beat = (i % 4) == 0, half = (i % 8) == 0;
      float p = half ? 0.9f : beat ? 0.6f * density + 0.2f : (i % 2 == 0 ? 0.35f * density : 0.12f * density);
      if (i == 30 || i == 31) p *= 0.3f;  // breathe at the phrase end
      if (frand() < p) {
        if (beat) {  // chord tone near the current note
          int best = 0, bd = 99;
          for (int ct = -7; ct <= 11; ct++) {
            int cm = ((ct % 7) + 7) % 7;
            if (cm != 0 && cm != 2 && cm != 4) continue;  // root, third, fifth (any octave)
            int dist = abs(ct - cur) + (frand() < 0.3f ? 1 : 0);
            if (dist < bd && dist > 0) { bd = dist; best = ct; }
          }
          cur = best;
        } else {
          cur += (frand() < 0.5f) ? 1 : -1;
          if (frand() < 0.12f) cur += (frand() < 0.5f) ? 2 : -2;
        }
        if (cur > 9) cur -= 2;
        if (cur < -3) cur += 2;
        m[i] = cur;
      } else {
        m[i] = (i > 0 && m[i - 1] != REST && frand() < 0.5f) ? HOLD : REST;
      }
    }
    m[0] = (m[0] == REST || m[0] == HOLD) ? 0 : m[0];
  }

  void applyRealm(uint32_t seed) {
    uint32_t s = seed * 2654435761u + 7;
    rng = s ? s : 1;
    root = rnd() % 12;
    mode = rnd() % NMODES;
    prog = rnd() % MODES[mode].nprogs;
    baseBpm = 84 + rnd() % 36;
    leadDuty = (rnd() & 1) ? 0.25f : 0.125f;
    makeMotif(motifA, 0.7f);
    makeMotif(motifB, 0.9f);
    memcpy(motif, motifA, 32);
    pushEv(EV_REALM, 0, 0, 0);
  }

  void begin() {
    lead.vib = 0.004f; lead.vibDelay = SR / 6;
    bass.tri = true; harm.duty = 0.125f;
    memset(echo, 0, sizeof(echo));
    realmSeen = infl.realm;
    applyRealm(infl.realm);
  }

  void phraseStart() {
    int sec = (bar / 4) % 4;
    mstate.section = sec;
    if (pendingRealm && pendingRealm != realmSeen) { realmSeen = pendingRealm; applyRealm(pendingRealm); pendingRealm = 0; }
    if (infl.adopt) {
      infl.adopt = false;
      root = infl.adoptRoot;
      int pick = rnd() % 2;
      mode = infl.adoptMinor ? (pick ? 1 : 0) : (pick ? 3 : 4);
      prog = rnd() % MODES[mode].nprogs;
      if (infl.adoptBpm > 0) baseBpm = (int)infl.adoptBpm;
      makeMotif(motifA, 0.7f); makeMotif(motifB, 0.9f);
      pushEv(EV_REALM, 0, 0, 0);
    }
    if (sec == 0) memcpy(motif, motifA, 32);
    else if (sec == 1) {  // A': vary the tail
      memcpy(motif, motifA, 32);
      for (int i = 24; i < 32; i++) if (motif[i] > REST + 10 && frand() < 0.5f) motif[i] += (frand() < 0.5f) ? 1 : -1;
    } else if (sec == 2) { makeMotif(motifB, 0.6f + 0.1f * intensity); memcpy(motif, motifB, 32); }
    else memcpy(motif, motifA, 32);
    if (sec == 0 && frand() < 0.25f) makeMotif(motifA, 0.7f);  // slowly evolve the theme
  }

  // one 32nd-note tick
  void tick() {
    bool lull = infl.sleepy;
    mstate.lullaby = lull;
    if (sub == 0) {
      if (pendingKeyShift) { root = (root + pendingKeyShift) % 12; pendingKeyShift = 0; }
      if (bar % 4 == 0) phraseStart();
      int cd = chordDegAt(bar);
      mstate.chordDeg = cd;
      pushEv(EV_CHORD, 0, cd, 0);
      // intensity: motion sets the floor, the form adds lift in the B section
      int motionI = infl.energy > 0.55f ? 3 : infl.energy > 0.25f ? 2 : infl.energy > 0.08f ? 1 : 0;
      int formI = (mstate.section == 2) ? 1 : 0;
      intensity = lull ? 0 : min(3, max(motionI, formI + (bar / 16) % 2));
      mstate.intensity = intensity;
    }
    if (infl.spell) {
      infl.spell = false;
      flourish = 16; flourishBase = chordDegAt(bar);
      pendingKeyShift = 7;  // up a fifth at the next bar
      pushEv(EV_SPELL, 0, 0, 0);
    }
    if (infl.realm != realmSeen && !pendingRealm) pendingRealm = infl.realm;

    int cd = chordDegAt(bar);
    int s16 = sub / 2;
    bool on16 = (sub % 2) == 0;
    float tiltShift = infl.tilt * 2.0f;
    int ornament = infl.spirits;
    float master = lull ? 0.5f : 1.0f;

    // flourish: fast rising arpeggio on 32nds
    if (flourish > 0) {
      int i = 16 - flourish;
      int deg = flourishBase + (i / 3) * 7 + (i % 3) * 2;
      int m = degToMidi(deg, 60);
      lead.duty = 0.125f;
      lead.on(m, 0.5f, SR / 30, 0.6f, 0.9997f);
      pushEv(EV_LEAD, m, ((deg % 7) + 7) % 7, 90);
      flourish--;
    } else if (on16) {
      int8_t v = motif[(bar % 2) * 16 + s16];
      if (v != REST && v != HOLD) {
        int deg = cd + v + (int)roundf(tiltShift);
        int len = 1;
        for (int k = 1; k < 8; k++) {
          int idx = (bar % 2) * 16 + s16 + k;
          if (idx >= 32 || motif[idx] != HOLD) break;
          len++;
        }
        int m = degToMidi(deg, lull ? 60 : 69);
        if (m > 93) m -= 12;
        if (m < 57) m += 12;
        int lenS = (int)(len * samplesPer16() * 0.92f);
        lead.duty = leadDuty;
        float vel = (s16 % 4 == 0 ? 0.42f : 0.34f) * master;
        lead.on(m, vel, lenS, 0.55f, 0.99985f);
        // ornament: a quick upper-neighbor grace on long notes, more with more spirits around
        if (len >= 3 && frand() < min(0.45f, 0.05f + ornament * 0.03f)) {
          graceMidi = degToMidi(deg + 1, lull ? 60 : 69); graceAt = (int)(samplesPer16() * 0.25f); graceBack = m;
        }
        lastLeadDeg = deg;
        pushEv(EV_LEAD, m, ((deg % 7) + 7) % 7, (uint8_t)(vel * 255));
      }
    }

    // harmony voice
    if (on16) {
      if (intensity == 0) {
        if (s16 == 0) {
          int m = degToMidi(cd + 2, 57);
          harm.duty = 0.5f;
          harm.on(m, 0.14f * master, (int)(samplesPer16() * 14), 0.7f, 0.99995f);
          pushEv(EV_HARM, m, ((cd + 2) % 7), 40);
        }
      } else {
        static const int8_t ARP[4] = {0, 2, 4, 7};
        int deg = cd + ARP[s16 % 4] + (intensity >= 3 && (s16 / 4) % 2 ? 7 : 0);
        int m = degToMidi(deg, 57);
        harm.duty = 0.125f;
        harm.on(m, 0.12f * master, (int)(samplesPer16() * 0.7f), 0.4f, 0.9996f);
        if (s16 % 4 == 0) pushEv(EV_HARM, m, deg % 7, 30);
      }
    }

    // bass
    if (on16) {
      int deg = -1;
      if (intensity == 0) { if (s16 == 0) deg = cd; }
      else if (intensity == 1) { if (s16 == 0) deg = cd; if (s16 == 8) deg = cd + 4; }
      else if (intensity == 2) { if (s16 == 0 || s16 == 6) deg = cd; if (s16 == 8) deg = cd + 4; if (s16 == 12) deg = cd + 7; }
      else { if (s16 % 2 == 0) deg = (s16 % 4 == 0) ? cd : cd + 7; }
      if (deg >= 0) {
        int m = degToMidi(deg, 33);
        int len = intensity == 0 ? 15 : intensity == 3 ? 1 : 3;
        bass.on(m, 0.55f * master, (int)(samplesPer16() * len), 0.8f, 0.99995f);
        pushEv(EV_BASS, m, deg % 7, 120);
      }
    }

    // drums
    if (on16 && !lull) {
      if (intensity >= 2) {
        if (s16 == 0 || s16 == 8 || (intensity == 3 && s16 == 10)) { drum.hit(0, 0.8f); pushEv(EV_KICK, 0, 0, 200); }
        else if (s16 == 4 || s16 == 12) { drum.hit(1, 0.32f); pushEv(EV_SNARE, 0, 0, 120); }
        else if ((intensity == 3 && s16 % 2 == 0) || s16 % 4 == 2) { drum.hit(2, 0.12f); pushEv(EV_HAT, 0, 0, 40); }
      } else if (intensity == 1 && (s16 == 4 || s16 == 12)) {
        drum.hit(2, 0.08f); pushEv(EV_HAT, 0, 0, 30);
      }
    }

    sub++;
    if (sub >= 32) { sub = 0; bar++; }
  }

  int graceAt = 0, graceMidi = 0, graceBack = 0;

  float samplesPer16() { return SR * 60.0f / (bpm * 4.0f); }

  void render(int16_t* out, int n) {
    float target = baseBpm * (1.0f + 0.22f * infl.energy);
    if (infl.weary) target *= 0.85f;
    if (infl.sleepy) target = 62;
    bpm += (target - bpm) * 0.02f;
    mstate.bpm = (int)bpm;
    mstate.root = root; mstate.mode = mode;
    float echoMix = 0.18f + min(0.22f, infl.spirits * 0.015f);
    for (int i = 0; i < n; i++) {
      if (--samplesToTick <= 0) { tick(); samplesToTick = (int)(samplesPer16() / 2); }
      if (graceAt > 0 && --graceAt == 0) {  // grace note: hop up, then back down
        lead.inc = (uint32_t)(midiHz(graceMidi) * 4294967296.0f / SR);
        graceAt = -(int)(samplesPer16() * 0.2f);
      } else if (graceAt < 0 && ++graceAt == 0) {
        lead.inc = (uint32_t)(midiHz(graceBack) * 4294967296.0f / SR);
      }
      float l = lead.next();
      float s = l * 0.9f + harm.next() * 0.8f + bass.next() * 0.9f + drum.next() * 0.7f;
      float e = echo[echoPos] / 32767.0f;
      echo[echoPos] = (int16_t)(constrain(l * 0.8f + e * 0.45f, -1.0f, 1.0f) * 32767);
      echoPos = (echoPos + 1) % echoLen;
      s += e * echoMix;
      s = s / (1.0f + fabsf(s) * 0.6f);  // soft clip
      out[i] = (int16_t)(s * 26000);
    }
  }
};
static Composer composer;
