# HA Knob

Firmware that turns the **LilyGo T-Encoder Pro** (ESP32-S3R8, 16 MB flash, 8 MB OPI PSRAM,
390x390 round AMOLED, capacitive touch, rotary encoder with push button, buzzer) into a
Home Assistant control knob.

**Status: v0.2.0, Step 2 (input core and UI shell).** The firmware has its final module
structure for the hardware (display, touch swipes, encoder, button, buzzer) and shows a boot
screen followed by a status screen ("Wi-Fi: not configured"). Wi-Fi, the web UI and the
Home Assistant connection come in later steps.

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
| Regenerate fonts | `sh tools/gen_fonts.sh` (see Fonts) |

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

### Serial log lines

```
BOOT fw=<version> psram=<bytes> flash=<bytes> touch=<CST816|CHSC5816|none> heap=<bytes>
ENC d=<+1|-1> count=<n>                  one line per detent, clockwise = +1
BTN down                                 debounced press (the buzzer clicks)
BTN up + BTN short                       release before 600 ms
BTN long                                 held 600 ms (fired while held; no "short" after it)
BTN verylong                             held 10 s
BTN up                                   release after long / verylong
TOUCH swipe=<left|right>                 horizontal swipe (taps are ignored)
HB up=<s> psram=<bytes> flash=<bytes> touch=<..> count=<n>   every 5 s
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
- **The screen is the knob cap.** Pressing the screen also clicks the push button (touch is
  seen about 120 ms before the click). So all press actions come from the button, and touch
  is used only for horizontal swipes; taps never trigger anything.
- **Encoder:** 2 valid quadrature transitions per detent (rest positions AB = 00 and 11),
  clockwise = positive. Confirmed on hardware.

## Input handling

| Input | Rule |
|---|---|
| Encoder | One count per detent, counted on arrival at a rest state (00/11); bounce, half-turns back and invalid jumps never count |
| Button | 30 ms debounce; short press < 600 ms (on release); long press at 600 ms (once, while held); very long press at 10 s |
| Swipe | Horizontal travel >= 80 px, finger down for <= 700 ms, and \|dx\| >= 2 x \|dy\| |
| Acceleration (used from Step 7) | Detents < 40 ms apart x4, < 80 ms x2, else x1 |

## Fonts

LVGL's built-in Montserrat fonts are ASCII only, so the project generates its own from
Montserrat Medium (SIL OFL 1.1) with `lv_font_conv` **1.5.3**. They are committed in
`src/ui/fonts/` and declared in `include/lv_conf.h` (default font `ui_font_20`); the
built-in Montserrat fonts are disabled.

| Font | Size | Glyphs | Flash |
|---|---|---|---|
| `ui_font_20` | 20 px | text ranges + LVGL symbols (380 glyphs) | 47.0 KB |
| `ui_font_28` | 28 px | text ranges + LVGL symbols (380 glyphs) | 80.8 KB |
| `ui_font_56` | 56 px | text ranges only (320 glyphs) | 207.8 KB |

Text ranges: `0x20-0x7E` (ASCII), `0xA0-0xFF` (Latin-1: æ ø å Æ Ø Å, °, ...),
`0x100-0x17F` (Latin Extended-A), `0x2022` (bullet). LVGL symbols: the `LV_SYMBOL_*`
glyphs of LVGL's built-in fonts (FontAwesome 5 Free, from LVGL 9.2.2's
`scripts/built_in_font/`). All fonts are 4 bpp, uncompressed.

To regenerate (needs Node.js; run from the repository root after
`pio pkg install -e t-encoder-pro`, which provides the source fonts):

```
sh tools/gen_fonts.sh
```

The script runs, for each size (shown for 20 px; 56 px omits the second `--font`):

```
npx -y lv_font_conv@1.5.3 --no-compress --no-prefilter --bpp 4 --format lvgl \
    --force-fast-kern-format --size 20 \
    --font .pio/libdeps/t-encoder-pro/lvgl/scripts/built_in_font/Montserrat-Medium.ttf \
    -r 0x20-0x7E,0xA0-0xFF,0x100-0x17F,0x2022 \
    --font .pio/libdeps/t-encoder-pro/lvgl/scripts/built_in_font/FontAwesome5-Solid+Brands+Regular.woff \
    -r <LV_SYMBOL code points, listed in the script> \
    -o src/ui/fonts/ui_font_20.c
```

The generated files also need `-DLV_LVGL_H_INCLUDE_SIMPLE` (set in `platformio.ini`).

## Library versions

All versions are pinned exactly in `platformio.ini`.

| Library | Version | Why this version |
|---|---|---|
| Platform `espressif32` | 6.12.0 (Arduino-ESP32 2.0.17) | Project decision; stable PlatformIO platform |
| `moononournation/GFX Library for Arduino` | 1.5.0 | Upstream `Arduino_CO5300` + `Arduino_ESP32QSPI`; same version as the vendor uses; builds on core 2.0.17 |
| `lvgl/lvgl` | 9.2.2 | Maintained v9 line; matches the vendor `lvgl_9` example |
| `lewisxhe/SensorLib` | 0.3.4 | CST816 and CHSC5816 touch drivers; latest 0.3.x release, builds on core 2.0.17 |
| `throwtheswitch/Unity` | 2.6.1 | Host test framework (native env); pinned to the version PlatformIO resolved |

Licences:
- LVGL MIT, SensorLib MIT.
- Fonts: the Montserrat and FontAwesome Free font files are SIL OFL 1.1; the FontAwesome
  Free icon designs are CC BY 4.0. The generated `src/ui/fonts/*.c` are derived fonts, so
  before any public release the OFL notices (and FontAwesome attribution) must ship with
  them.
- Arduino_GFX: licence unconfirmed / mixed (the 1.5.0 package ships no LICENSE file; much
  of it is derived from Adafruit-GFX, which is BSD); acceptable for this private project,
  to be rechecked before any public release.
- The LilyGo vendor repository is GPL-3.0; this project uses only hardware facts (pin
  numbers, panel quirks) from it and copies none of its files.

## Project layout

```
include/          board_pins.h (GPIO map), lv_conf.h (LVGL config), version.h
src/main.cpp      setup()/loop() wiring only
src/hal/          Display, Touch, Encoder, Button, Buzzer (hardware glue)
src/ui/           Ui (screen manager), Widgets, screens/, fonts/ (generated)
lib/core/         pure C++17 logic, no Arduino/ESP headers, unit-tested on the host
  src/input/      QuadratureDecoder, ButtonFsm, Acceleration, SwipeClassifier
  src/ui/         Presenter (number formatting)
  src/util/       MathUtil
test/native/      host unit tests (Unity), run with `pio test -e native`
tools/            serial_check.py, gen_fonts.sh
```

## Security

- No secrets (Wi-Fi passwords, Home Assistant tokens) are ever committed. They will live
  only in the device's NVS, entered through the device web UI. `.gitignore` excludes
  `*.local.ini`, `secrets*` and `.env*`.
