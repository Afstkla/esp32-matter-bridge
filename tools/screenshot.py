#!/usr/bin/env python3
"""Save what the panel is showing as a PNG, by way of the framebuffer.

    uv run --with pyserial,pillow python tools/screenshot.py shot.png pattern

Any arguments after the output path are run as console commands first, so the
usual use is to draw a screen and capture it in one go. The board is never
reset: the point is to photograph the state it is already in.
"""
import base64
import glob
import os
import sys
import time

import serial
from PIL import Image

BAUD = 115200


def find_port():
    if os.environ.get("MCTL_PORT"):
        return os.environ["MCTL_PORT"]
    ports = sorted(glob.glob("/dev/cu.usbmodem*"))
    if not ports:
        raise SystemExit("no /dev/cu.usbmodem* found")
    return ports[0]


def send(ser, line):
    ser.write((line + "\n").encode())
    ser.flush()


def read_dump(ser, timeout=60.0):
    deadline = time.time() + timeout
    payload = []
    size = None
    while time.time() < deadline:
        raw = ser.readline().decode("utf-8", "replace").strip()
        if not raw:
            continue
        if raw.startswith("SCREENDUMP END"):
            if size is None:
                raise SystemExit("dump ended before its header arrived")
            return size, base64.b64decode("".join(payload))
        if raw.startswith("SCREENDUMP "):
            parts = raw.split()
            size = (int(parts[1]), int(parts[2]))
            payload = []
        elif size is not None:
            payload.append(raw)
    raise SystemExit("timed out waiting for SCREENDUMP END")


def to_image(size, data):
    width, height = size
    expected = width * height * 2
    if len(data) != expected:
        raise SystemExit(f"got {len(data)} bytes, expected {expected}")
    # Big-endian RGB565, straight off the wire — see theme.h.
    image = Image.new("RGB", size)
    pixels = image.load()
    for index in range(width * height):
        value = (data[index * 2] << 8) | data[index * 2 + 1]
        red = (value >> 11) & 0x1F
        green = (value >> 5) & 0x3F
        blue = value & 0x1F
        pixels[index % width, index // width] = (red << 3 | red >> 2, green << 2 | green >> 4,
                                                 blue << 3 | blue >> 2)
    return image


def main():
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    out = sys.argv[1]
    port = find_port()
    print(f"  . {port}")
    with serial.Serial(port, BAUD, timeout=1.0) as ser:
        time.sleep(0.2)
        ser.reset_input_buffer()
        for command in sys.argv[2:]:
            print(f"  > {command}")
            send(ser, command)
            time.sleep(0.5)
        ser.reset_input_buffer()
        send(ser, "screendump")
        size, data = read_dump(ser)
    to_image(size, data).save(out)
    print(f"  < {out} {size[0]}x{size[1]}")


if __name__ == "__main__":
    main()
