#!/bin/sh
# Flash the prebuilt Spellsong in firmware/spellsong/dist onto the M5StickS3.
set -e
cd "$(dirname "$0")/../firmware/spellsong"
PORT=$(ls /dev/cu.usbmodem* | head -1)
~/.espressif/python_env/idf6.1_py3.14_env/bin/esptool --chip esp32s3 -p "$PORT" -b 921600 --after watchdog-reset \
  write-flash 0x0 dist/spellsong.ino.bootloader.bin 0x8000 dist/spellsong.ino.partitions.bin 0xe000 dist/boot_app0.bin 0x10000 dist/spellsong.ino.bin
