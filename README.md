# HA Knob

Firmware that turns the **LilyGo T-Encoder Pro** (ESP32-S3R8, 16 MB flash, 8 MB OPI PSRAM,
390x390 round AMOLED, capacitive touch, rotary encoder with push button, buzzer) into a
Home Assistant control knob.

**Status: v0.1.0, Step 1 (hardware smoke test).** The firmware currently only proves that
display, touch, encoder, button and buzzer work. Wi-Fi, the web UI and the Home Assistant
connection come in later steps.

## Prerequisites

- [PlatformIO Core](https://platformio.org/) 6.2 (`pio` on PATH). The `espressif32@6.12.0`
  platform (Arduino-ESP32 core 2.0.17) is downloaded automatically on the first build.
- For host unit tests on Windows: MSYS2 ucrt64 `gcc`/`g++` on PATH.
- The board connected over its native USB port (the default port in `platformio.ini` is
  `COM14`; change `upload_port`/`monitor_port` if yours differs).

The project path may contain spaces; this is verified to work.

## Commands

| What | Command |
|---|---|
| Build firmware | `pio run -e t-encoder-pro` |
| Flash firmware | `pio run -e t-encoder-pro -t upload` |
| Flash web UI files (from a later step) | `pio run -e t-encoder-pro -t uploadfs` |
| Serial monitor | `pio device monitor` |
| Host unit tests | `pio test -e native` |
| Serial assertions | see below |

### Serial check

`tools/serial_check.py` opens the port without resetting the board, prints every line, and
exits 0 once every `--expect` regex has matched a line (1 on timeout). It retries while the
port does not exist yet, so it can be started right after a flash. Run it with PlatformIO's
Python (it ships `pyserial`):

```
%USERPROFILE%\.platformio\penv\Scripts\python.exe tools/serial_check.py --port COM14 ^
    --expect "HB .*psram=[89]\d{6}" --expect "flash=16777216" ^
    --expect "touch=(CST816|CHSC5816)" --timeout 20
```

`--expect` is required (at least one). Exit codes:

| Code | Meaning |
|---|---|
| 0 | Every `--expect` regex matched a line |
| 1 | Timeout: at least one expectation did not match |
| 2 | Infrastructure failure: port could not be opened, usage error, invalid regex, or pyserial missing |

The ESP32-S3 USB serial drops everything printed before the PC has re-enumerated the port,
so the one-time `BOOT` line is often missed. The `HB` heartbeat line repeats the key facts
every 5 seconds.

### Smoke test serial lines

```
BOOT fw=<version> psram=<bytes> flash=<bytes> touch=<CST816|CHSC5816|none> heap=<bytes>
ENC d=<+1|-1> count=<n>
ENCRAW ab=<prev>-><cur> step=<+1|-1|0> dt=<us since previous transition>
BTN down / BTN up
TOUCH x=<x> y=<y>
HB up=<s> psram=<bytes> flash=<bytes> touch=<..> count=<n>
```

## Hardware notes

- **IO0 / download mode.** The encoder push button is on IO0, the ESP32-S3 BOOT strapping
  pin. Holding the knob pressed while the board resets puts it in download (flashing) mode.
  If an upload fails because the port is gone, hold the knob, tap reset (or re-plug USB),
  release, and upload again.
- **Panel variants.** The T-Encoder Pro ships with one of two 390x390 AMOLED panels:
  - `TFD12MASBCTB4_V0_07` (newer): CO5300 display controller, CST816 touch (I2C 0x15).
  - `DXQ120MYB2416A`: SH8601 display controller, CHSC5816 touch (I2C 0x2E).

  The firmware detects the variant at boot by probing the touch address and reports it as
  `touch=` on serial. The display is driven with Arduino_GFX `Arduino_CO5300` in both cases.
- The panel needs `IO3` (VCI_EN) HIGH before initialisation, and every flush window must
  start on even x/y and have an even width/height.

## Library versions

All versions are pinned exactly in `platformio.ini`.

| Library | Version | Why this version |
|---|---|---|
| Platform `espressif32` | 6.12.0 (Arduino-ESP32 2.0.17) | Project decision; stable PlatformIO platform |
| `moononournation/GFX Library for Arduino` | 1.5.0 | Upstream `Arduino_CO5300` + `Arduino_ESP32QSPI`; same version as the vendor uses; builds on core 2.0.17 |
| `lvgl/lvgl` | 9.2.2 | Maintained v9 line; matches the vendor `lvgl_9` example |
| `lewisxhe/SensorLib` | 0.3.4 | CST816 and CHSC5816 touch drivers; latest 0.3.x release, builds on core 2.0.17 |
| `throwtheswitch/Unity` | 2.6.1 | Host test framework (native env); pinned to the version PlatformIO resolved |

Licences: LVGL MIT, SensorLib MIT. Arduino_GFX: licence unconfirmed / mixed (the 1.5.0
package ships no LICENSE file; much of it is derived from Adafruit-GFX, which is BSD);
acceptable for this private project, to be rechecked before any public release. The LilyGo vendor repository is GPL-3.0; this project
uses only hardware facts (pin numbers, panel quirks) from it and copies none of its files.

## Project layout

```
include/        board_pins.h (GPIO map), lv_conf.h (LVGL config), version.h
src/main.cpp    firmware (currently the Step 1 smoke test)
lib/core/       pure C++17 logic, no Arduino/ESP headers, unit-tested on the host
test/native/    host unit tests (Unity), run with `pio test -e native`
tools/          serial_check.py
```

## Security

- No secrets (Wi-Fi passwords, Home Assistant tokens) are ever committed. They will live
  only in the device's NVS, entered through the device web UI. `.gitignore` excludes
  `*.local.ini`, `secrets*` and `.env*`.
