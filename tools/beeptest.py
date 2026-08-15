#!/usr/bin/env python3
"""Prove the beep is audible by recording it with the device's own microphone.

    uv run --with pyserial python tools/beeptest.py

Runs `micdump` twice over the USB console — once with the beeper off and once
with it on — and reports the energy at the beep's own frequency in each 20 ms
window. A working speaker shows a burst of 2 kHz tone in the second capture and
nothing but noise floor in the first. The board is never reset.
"""
import base64
import glob
import math
import os
import struct
import sys
import time

import serial

BAUD = 115200
TONE_HZ = 2000
WINDOW_MS = 20


def find_port():
    if os.environ.get("MCTL_PORT"):
        return os.environ["MCTL_PORT"]
    ports = sorted(glob.glob("/dev/cu.usbmodem*"))
    if not ports:
        raise SystemExit("no /dev/cu.usbmodem* found")
    return ports[0]


def send(ser, cmd, settle=1.0):
    ser.reset_input_buffer()
    ser.write((cmd + "\n").encode())
    ser.flush()
    deadline = time.time() + settle
    out = []
    while time.time() < deadline:
        line = ser.readline().decode("utf-8", "replace").strip()
        if line:
            out.append(line)
    return out


def micdump(ser, ms):
    """Returns (sample_rate, channels, bytes) with holes zero-filled."""
    ser.reset_input_buffer()
    ser.write(f"micdump {ms}\n".encode())
    ser.flush()
    rate = channels = total = None
    data = None
    deadline = time.time() + ms / 1000.0 + 30
    while time.time() < deadline:
        line = ser.readline().decode("utf-8", "replace").strip()
        if not line:
            continue
        if line.startswith("MICDUMP ERR"):
            raise SystemExit(line)
        if line.startswith("MICDUMP END"):
            break
        if line.startswith("MICDUMP "):
            _, rate, channels, _bits, total = line.split()
            rate, channels, total = int(rate), int(channels), int(total)
            data = bytearray(total)
            continue
        if line.startswith("MIC ") and data is not None:
            _, offset, payload = line.split(maxsplit=2)
            try:
                chunk = base64.b64decode(payload, validate=True)
            except Exception:
                continue  # a line the console mangled; the offsets keep the rest aligned
            offset = int(offset)
            data[offset:offset + len(chunk)] = chunk
    if data is None:
        raise SystemExit("no MICDUMP header — is this firmware new enough?")
    return rate, channels, bytes(data)


def goertzel(samples, rate, freq):
    """Magnitude of one frequency bin, normalised to the window length."""
    k = 2 * math.cos(2 * math.pi * freq / rate)
    s1 = s2 = 0.0
    for x in samples:
        s0 = x + k * s1 - s2
        s2, s1 = s1, s0
    power = s1 * s1 + s2 * s2 - k * s1 * s2
    return math.sqrt(max(power, 0.0)) / len(samples)


def windows(data, rate, channels):
    """(start_ms, rms, tone) per WINDOW_MS of the left (microphone) channel."""
    frames = len(data) // (2 * channels)
    mono = struct.unpack_from(f"<{frames * channels}h", data)[0::channels]
    size = rate * WINDOW_MS // 1000
    for start in range(0, len(mono) - size, size):
        chunk = mono[start:start + size]
        rms = math.sqrt(sum(float(x) * x for x in chunk) / len(chunk))
        yield start * 1000 // rate, rms, goertzel(chunk, rate, TONE_HZ)


def report(label, data, rate, channels):
    rows = list(windows(data, rate, channels))
    if not rows:
        raise SystemExit(f"{label}: empty capture")
    peak = max(rows, key=lambda r: r[2])
    floor = sorted(r[2] for r in rows)[len(rows) // 2]
    print(f"\n{label}: {len(rows)} windows of {WINDOW_MS} ms")
    for at, rms, tone in rows:
        bar = "#" * min(60, int(tone / 40))
        print(f"  {at:5d} ms  rms {rms:8.1f}  {TONE_HZ} Hz {tone:8.1f}  {bar}")
    print(f"  peak {TONE_HZ} Hz {peak[2]:.1f} at {peak[0]} ms, median {floor:.1f}, "
          f"ratio {peak[2] / max(floor, 0.001):.1f}x")
    return peak[2], floor


def main():
    port = find_port()
    print(f"  . {port}")
    ser = serial.Serial(port, BAUD, timeout=0.5)
    time.sleep(0.2)

    send(ser, "beep off", 2.5)
    quiet_rate, quiet_ch, quiet = micdump(ser, 1000)
    quiet_peak, quiet_floor = report("beeper off", quiet, quiet_rate, quiet_ch)

    send(ser, "beep on", 0.3)
    rate, channels, loud = micdump(ser, 2500)
    loud_peak, loud_floor = report("beeper on", loud, rate, channels)
    send(ser, "beep off", 2.5)
    ser.close()

    verdict = "HEARD" if loud_peak > 8 * max(quiet_peak, 1.0) else "NOT HEARD"
    print(f"\n{verdict}: {TONE_HZ} Hz peaks at {loud_peak:.1f} while beeping, "
          f"{quiet_peak:.1f} with the beeper off")
    return 0 if verdict == "HEARD" else 1


if __name__ == "__main__":
    sys.exit(main())
