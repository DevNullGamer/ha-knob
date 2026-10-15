"""Background serial logger for tools/http_check.py (--serial-log). Never resets the board.

  python serial_log.py COM14 out.log 3600

Appends "<epoch seconds> <line>" for every serial line for N seconds. Needs pyserial (the
PlatformIO Python has it). DTR/RTS are kept low so opening the port does not reset the ESP32-S3.
Close any serial monitor first.
"""
import sys
import time

import serial


def main():
    port, out, secs = sys.argv[1], sys.argv[2], float(sys.argv[3])
    s = serial.Serial()
    s.port = port
    s.baudrate = 115200
    s.timeout = 0.2
    s.dtr = False
    s.rts = False
    s.open()
    end = time.time() + secs
    buf = b""
    with open(out, "a", encoding="utf-8", errors="replace") as f:
        while time.time() < end:
            d = s.read(4096)
            if not d:
                continue
            buf += d
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                f.write(f"{time.time():.3f} " + line.decode("utf-8", "replace").rstrip("\r") + "\n")
                f.flush()


if __name__ == "__main__":
    main()
