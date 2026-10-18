// Palette, pixel font and sprites for Spellsong.
// Everything is drawn into a 120x68 indexed framebuffer and doubled to the 240x135 panel.
#pragma once
#include <stdint.h>

static const int W = 120;
static const int H = 68;

enum : uint8_t {
  C_VOID = 0, C_SKY0, C_SKY1, C_SKY2, C_STAR_DIM, C_STAR,
  C_STONE0, C_STONE1, C_STONE2, C_MOSS0, C_MOSS1,
  C_GOLD0, C_GOLD1, C_GOLD2, C_WHITE, C_BLACK,
  C_NOTE = 16,        // 16..22 bright note colors, one per scale degree
  C_NOTE_DIM = 23,    // 23..29 settled sand
  C_FAIRY = 30, C_HEART = 31,
  C_SKIN, C_BEARD, C_WAX, C_FLAME0, C_FLAME1, C_ORB0, C_ORB1, C_ORB_RIM,
  C_COUNT
};

static const uint32_t PAL_RGB[C_COUNT] = {
  0x0b0a1a, 0x141432, 0x1e1f4a, 0x2b2a63, 0x6d6aa8, 0xfff6d5,
  0x2a2438, 0x3f3752, 0x5c5274, 0x1f4d3a, 0x3f8f4f,
  0x8a5a1c, 0xe0a63a, 0xffe07a, 0xffffff, 0x000000,
  0xe8424f, 0xf08a2e, 0xf5d041, 0x47c96b, 0x3fd0d8, 0x4a7cf0, 0xb36bf0,
  0x7a1d33, 0x7a3d14, 0x7a6418, 0x1f6438, 0x1b5f6e, 0x22337a, 0x4f2a7a,
  0xc8fff4, 0xff3355,
  0xf0c8a0, 0xe8e8f0, 0xe8dcc0, 0xff9a2e, 0xffe680, 0x10204a, 0x1c3a7a, 0x9ad8ff,
};

// 3x5 font. Each glyph is five rows of three pixels, written as a 15-char string.
struct Glyph { char c; const char* px; };
static const Glyph FONT[] = {
  {'A',"010101111101101"},{'B',"110101110101110"},{'C',"011100100100011"},{'D',"110101101101110"},
  {'E',"111100110100111"},{'F',"111100110100100"},{'G',"011100101101011"},{'H',"101101111101101"},
  {'I',"111010010010111"},{'J',"001001001101010"},{'K',"101101110101101"},{'L',"100100100100111"},
  {'M',"101111111101101"},{'N',"110101101101101"},{'O',"010101101101010"},{'P',"110101110100100"},
  {'Q',"010101101111011"},{'R',"110101110101101"},{'S',"011100010001110"},{'T',"111010010010010"},
  {'U',"101101101101111"},{'V',"101101101101010"},{'W',"101101111111101"},{'X',"101101010101101"},
  {'Y',"101101010010010"},{'Z',"111001010100111"},
  {'0',"111101101101111"},{'1',"010110010010111"},{'2',"110001010100111"},{'3',"110001010001110"},
  {'4',"101101111001001"},{'5',"111100110001110"},{'6',"011100111101111"},{'7',"111001010010010"},
  {'8',"111101111101111"},{'9',"111101111001110"},
  {'#',"101111101111101"},{'.',"000000000000010"},{':',"000010000010000"},{'-',"000000111000000"},
  {'+',"000010111010000"},{'/',"001001010100100"},{'!',"010010010000010"},{'?',"110001010000010"},
};

// Sprites: one char per pixel, '.' is transparent. Keys map to palette entries in world.h.
static const char* HEART[6] = {
  ".rr.rr.",
  "rrrrrrr",
  "rrrrrrr",
  ".rrrrr.",
  "..rrr..",
  "...r...",
};

static const char* WIZARD[16] = {
  "....hhh.....",
  "...hhhhh....",
  "..hhhshhh...",
  ".hhhhhhhhh..",
  "hhhhhhhhhhh.",
  "...ffff.....",
  "...fefe...t.",
  "...bbbb...t.",
  "..rbbbbr..t.",
  ".rrbbbbrr.t.",
  ".rrrbbrrrrt.",
  "rrrrrrrrr.t.",
  "rrrrrrrrr.t.",
  ".rrrrrrr..t.",
  ".rrrrrrr..t.",
  "..kk..kk..t.",
};

static const char* EYE[5] = {
  "..wwwww..",
  ".wwpppww.",
  "wwppkppww",
  ".wwpppww.",
  "..wwwww..",
};
