#!/usr/bin/env python3
"""Drive the Matter probe over USB serial.

    uv run --with pyserial python tools/mctl.py state 'vol 200' 'click 1'

Opening the port resets the board, so all commands run over one connection.
"""
import glob
import os
import sys
import time

import serial

BAUD = 115200


def find_port():
    if os.environ.get("MCTL_PORT"):
        return os.environ["MCTL_PORT"]
    ports = sorted(glob.glob("/dev/cu.usbmodem*"))
    if not ports:
        raise SystemExit("no /dev/cu.usbmodem* found")
    if len(ports) > 1:
        print(f"  ! several boards: {ports} — using {ports[0]}, set MCTL_PORT to pick")
    return ports[0]


def open_port(timeout=25.0, reset=True):
    port = find_port()
    print(f"  . {port}")
    ser = serial.Serial(port, BAUD, timeout=0.3)
    # Skips the deliberate reset pulse and the banner wait. DTR is left asserted
    # on purpose: this chip's USB-Serial-JTAG gates its output on it, so clearing
    # the line does not avoid a reset, it just goes silent.
    if not reset:
        time.sleep(0.2)
        ser.reset_input_buffer()
        return ser, []
    # IO0 must stay high or the chip drops into download mode; pulse RTS to reset
    # so we see the boot banner from the start rather than joining mid-run.
    ser.dtr = False
    ser.rts = True
    time.sleep(0.1)
    ser.rts = False
    deadline = time.time() + timeout
    banner = []
    while time.time() < deadline:
        line = ser.readline().decode("utf-8", "replace").strip()
        if line:
            banner.append(line)
            print(f"  | {line}")
            if line == "ready":
                return ser, banner
    # A running board that ignored the reset pulse still answers commands, so
    # carry on rather than giving up on a missing banner.
    print("  ! no boot banner; continuing anyway")
    return ser, banner


def send(ser, cmd, settle=1.0):
    print(f"> {cmd}")
    ser.reset_input_buffer()
    ser.write((cmd + "\n").encode())
    ser.flush()
    deadline = time.time() + settle
    while time.time() < deadline:
        line = ser.readline().decode("utf-8", "replace").strip()
        if line:
            print(f"  < {line}")


def main():
    args = sys.argv[1:]
    reset = "--no-reset" not in args
    cmds = [a for a in args if a != "--no-reset"] or ["state"]
    ser, _ = open_port(reset=reset)
    for cmd in cmds:
        if cmd.startswith("sleep "):
            time.sleep(float(cmd.split()[1]))
            continue
        if cmd.startswith("listen "):
            secs = float(cmd.split()[1])
            print(f"* listening {secs}s")
            deadline = time.time() + secs
            while time.time() < deadline:
                line = ser.readline().decode("utf-8", "replace").strip()
                if line:
                    print(f"  < {line}")
            continue
        send(ser, cmd)
    ser.close()


if __name__ == "__main__":
    main()
