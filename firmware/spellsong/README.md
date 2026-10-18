# Spellsong (M5StickS3)

Generative fantasy chiptune with pixel art. The music is always in key, and motion, the mic,
nearby Wi-Fi and the battery shape it. See the header of `spellsong.ino` for controls.

| File | What it holds |
|---|---|
| `spellsong.ino` | setup, audio task (speaker or mic), senses, scene loop, serial commands |
| `music.h` | four-voice NES-style synth and the composer (modes, progressions, motifs) |
| `scry.h` | mic analysis: FFT bands, key detection (Krumhansl-Kessler), tempo |
| `world.h` | 120x68 indexed framebuffer: sand, particles, wisp, orb scene, HUD |
| `pal.h` | palette, 3x5 font, sprites |

## Build

Arduino core 3.3.x for ESP32, with M5Unified and M5GFX installed as libraries:

```bash
arduino-cli compile \
  --fqbn "espressif:esp32:esp32s3:CDCOnBoot=cdc,USBMode=hwcdc,FlashSize=8M,PSRAM=opi,PartitionScheme=default_8MB" \
  --output-dir build firmware/spellsong
esptool --chip esp32s3 -p /dev/cu.usbmodem* --after watchdog-reset write-flash 0x0 build/spellsong.ino.merged.bin
```

The StickS3 module (ESP32-S3-PICO-1-N8R8) has octal PSRAM, so `PSRAM=opi`; the quad setting fails at boot with "wrong PSRAM line mode". To go back to the factory firmware,
flash UiFlow2 from M5Burner.

`tools/stick.py shot|state|imu` reads the board over USB.
