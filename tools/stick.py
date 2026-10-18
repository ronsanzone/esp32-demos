#!/usr/bin/env python3
"""Talk to Spellsong on the M5StickS3 over USB.

  stick.py shot [out.png]   save the current frame (4x scale)
  stick.py state            print the music and sensor state as JSON
  stick.py imu              print the filtered accelerometer reading
  stick.py send <cmd>       send a raw command (spell, scry, scene)

The port defaults to the first /dev/cu.usbmodem*; pass --port to override.
"""
import argparse
import glob
import struct
import sys
import time
import zlib

import serial


def find_port():
    ports = sorted(glob.glob("/dev/cu.usbmodem*")) or sorted(glob.glob("/dev/ttyACM*"))
    if not ports:
        sys.exit("no USB serial port found")
    return ports[0]


def ask(ser, cmd, until, timeout=6.0):
    ser.reset_input_buffer()
    ser.write((cmd + "\n").encode())
    lines, end = [], time.time() + timeout
    while time.time() < end:
        raw = ser.readline()
        if not raw:
            continue
        line = raw.decode(errors="replace").strip()
        lines.append(line)
        if until(line, lines):
            return lines
    sys.exit(f"timed out waiting for reply to {cmd!r}")


def write_png(path, w, h, pal, rows, scale=4):
    raw = bytearray()
    for y in range(h * scale):
        raw.append(0)
        row = rows[y // scale]
        for x in range(w * scale):
            raw += pal[row[x // scale]]
    def chunk(tag, data):
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", struct.pack(">IIBBBBB", w * scale, h * scale, 8, 2, 0, 0, 0)))
        f.write(chunk(b"IDAT", zlib.compress(bytes(raw), 9)))
        f.write(chunk(b"IEND", b""))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("cmd")
    ap.add_argument("arg", nargs="?")
    ap.add_argument("--port")
    a = ap.parse_args()
    ser = serial.Serial(a.port or find_port(), 115200, timeout=0.5)
    time.sleep(0.2)
    if a.cmd == "shot":
        lines = ask(ser, "shot", lambda l, _: l == "@@END", timeout=10)
        start = next(i for i, l in enumerate(lines) if l.startswith("@@SHOT"))
        _, w, h = lines[start].split()
        w, h = int(w), int(h)
        pal = [bytes.fromhex(c) for c in lines[start + 1].split()]
        rows = [bytes.fromhex(l) for l in lines[start + 2:start + 2 + h]]
        out = a.arg or "stick-shot.png"
        write_png(out, w, h, pal, rows)
        print(out)
    elif a.cmd in ("state", "imu"):
        lines = ask(ser, a.cmd, lambda l, _: l.startswith("{"))
        print(lines[-1])
    elif a.cmd == "send":
        ser.write((a.arg + "\n").encode())
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
