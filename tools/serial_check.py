#!/usr/bin/env python3
"""Reset-agnostic serial capture with regex assertions.

Opens a serial port (retrying while it does not exist yet, e.g. right after a
flash/reset of the ESP32-S3 native USB port), prints every received line, and
exits 0 as soon as every --expect regex has matched at least one line, or 1 on
timeout. It never flashes and never resets the board (DTR/RTS are left low).

Run with PlatformIO's Python, which ships pyserial:
    %USERPROFILE%\\.platformio\\penv\\Scripts\\python.exe tools/serial_check.py \\
        --port COM14 --expect "HB .*psram=[89]\\d{6}" --expect "touch=(CST816|CHSC5816)" --timeout 20
"""

import argparse
import re
import sys
import time

try:
    import serial  # pyserial
except ImportError:
    sys.exit("pyserial not found: run this script with PlatformIO's penv Python")


def open_port(port, baud, deadline):
    """Opens the port, retrying until the deadline. Returns None on timeout."""
    while time.monotonic() < deadline:
        ser = serial.Serial()
        ser.port = port
        ser.baudrate = baud
        ser.timeout = 0.2
        # Setting these before open() keeps the ESP32-S3 USB-Serial/JTAG from resetting.
        ser.dtr = False
        ser.rts = False
        try:
            ser.open()
            return ser
        except serial.SerialException:
            time.sleep(0.5)
    return None


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--port", required=True, help="serial port, e.g. COM14")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--expect", action="append", default=[],
                        help="regex that must match a received line (repeatable)")
    parser.add_argument("--timeout", type=float, default=20.0,
                        help="seconds to wait in total, including opening the port")
    args = parser.parse_args()

    patterns = [re.compile(p) for p in args.expect]
    pending = list(patterns)
    deadline = time.monotonic() + args.timeout

    ser = open_port(args.port, args.baud, deadline)
    if ser is None:
        print(f"FAIL could not open {args.port} within {args.timeout:.0f} s")
        return 1

    buffer = b""
    try:
        while pending and time.monotonic() < deadline:
            try:
                chunk = ser.read(256)
            except serial.SerialException:
                # Port vanished (board reset); reopen and keep waiting.
                ser.close()
                ser = open_port(args.port, args.baud, deadline)
                if ser is None:
                    break
                continue
            buffer += chunk
            while b"\n" in buffer:
                raw, buffer = buffer.split(b"\n", 1)
                line = raw.decode("utf-8", errors="replace").rstrip("\r")
                print(line)
                pending = [p for p in pending if not p.search(line)]
    finally:
        if ser is not None:
            ser.close()

    if pending:
        for p in pending:
            print(f"FAIL no line matched: {p.pattern}")
        return 1
    print(f"OK all {len(patterns)} expectation(s) matched")
    return 0


if __name__ == "__main__":
    sys.exit(main())
