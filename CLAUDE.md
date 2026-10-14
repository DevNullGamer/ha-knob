# CLAUDE.md

Guidance for AI assistants working in this repository.

## Project

HA Knob: Home Assistant controller firmware for the LilyGo T-Encoder Pro (ESP32-S3R8,
16 MB flash, 8 MB OPI PSRAM, 390x390 round AMOLED, touch, rotary encoder, buzzer).
PlatformIO + Arduino framework. Current version 0.1.0 (Step 1: hardware smoke test).

The approved specification and change log live outside the repo in
`~/.claude/workspace/chgreq/LilyGo T-encoderPro - HA Controller/chg001/`.
Implement one specification step at a time.

## Commands

```
pio run -e t-encoder-pro                 # build
pio run -e t-encoder-pro -t upload       # flash (COM14)
pio run -e t-encoder-pro -t uploadfs     # flash LittleFS web assets (from Step 4)
pio device monitor                       # serial monitor
pio test -e native                       # host unit tests (needs MSYS2 ucrt64 gcc on PATH)
%USERPROFILE%\.platformio\penv\Scripts\python.exe tools/serial_check.py --port COM14 --expect "<regex>" --timeout 20
```

`serial_check.py` never resets or flashes; it waits for each `--expect` regex and exits 0/1.
USB-CDC drops output printed before the host enumerates, so assert on the repeating `HB`
line, not only on `BOOT`.

## Layout and rules

- `include/board_pins.h`: the only place GPIO numbers are defined.
- `include/version.h`: `FW_VERSION`; bump it with releases.
- `include/lv_conf.h`: LVGL 9.2.2 template with project changes listed at the top.
- `lib/core/`: **pure C++17**. No Arduino, ESP-IDF or FreeRTOS includes; only the STL (and
  ArduinoJson once added). Everything here must be testable with `pio test -e native`.
  Hardware glue lives in `src/`.
- `test/native/test_*/`: Unity host tests. `test/embedded/` is reserved for on-device tests.
- Keep `src/main.cpp` as wiring only once the module structure exists (Step 2).
- All library versions are pinned exactly in `platformio.ini`; record any change and its
  reason in README "Library versions".

## Hardware facts and gotchas

- **IO3 (VCI_EN) must be HIGH** before the display is initialised.
- Display: Arduino_GFX `Arduino_ESP32QSPI` + `Arduino_CO5300(bus, 4, 0, false, 390, 390, 0,0,0,0)`,
  `begin(40000000)`. LVGL flush areas must be rounded to even x1/y1 and even width/height
  (done in an `LV_EVENT_INVALIDATE_AREA` handler).
- **IO0 is the encoder push button and the BOOT strapping pin**: holding the knob during
  reset enters download mode. Never use a hold-at-boot gesture for features.
- Serial: `Serial.setTxTimeoutMs(0)` so writes never block without a USB host.
- Buzzer uses the core-2.x LEDC API (`ledcSetup` + `ledcAttachPin`), not core-3 `ledcAttach`.
- SensorLib drivers are given `sda/scl = -1` because `Wire.begin(5, 6)` is called first
  (otherwise core 2 logs "bus already initialized").

## Step 1 hardware results (2026-10-14)

- **Detected touch variant: CST816** (I2C 0x15), so the unit is the newer
  `TFD12MASBCTB4_V0_07` panel with the CO5300 controller.
- `BOOT`/`HB`: `psram=8386215 flash=16777216 touch=CST816`, free heap about 282 KB at boot.
- **Transitions per detent: provisionally 2** (`kTransitionsPerDetent` in `src/main.cpp`).
  A first manual spin logged one `ENC` per two valid quadrature transitions (e.g.
  `01->00`, `00->10`), consistent with the vendor decoder (detents at both 00 and 11).
  **Needs user confirmation**: each click must change `count` by exactly 1, clockwise
  positive. The `ENCRAW ... dt=` lines show the gaps where the knob rested, which identify
  the detent positions. Step 2's `QuadratureDecoder` takes N from this.
- Screen, button/buzzer and touch coordinate checks are pending user confirmation.

## Library versions (final for Step 1)

- espressif32 6.12.0 (Arduino-ESP32 2.0.17)
- moononournation/GFX Library for Arduino 1.5.0 (no fallback to 1.4.9 needed)
- lvgl/lvgl 9.2.2
- lewisxhe/SensorLib 0.3.4 (builds on core 2.0.17)
- throwtheswitch/Unity 2.6.1 (native env test framework, pinned in `lib_deps`)
- Deviation from spec §3.2: the `native` env has no `lib_deps` yet; ArduinoJson 7.4.3 is
  added in Step 3 when the core first uses it.

## Step 2 notes (from the Step 1 audit)

- The smoke-test decoder resets its accumulator at every detent, so its phase can drift
  after a missed/invalid transition. The real `QuadratureDecoder` must count detents at the
  **rest states 00/11** (absolute position), not by accumulating N transitions.
- The `ENCRAW` ISR ring buffer (no overflow-safe handoff, entries may be overwritten while
  read) is debug-only. **Do not reuse that pattern for real data**; the encoder path should
  expose only an atomic detent counter.

## Upcoming requirements

- **Danish characters (æ ø å Æ Ø Å)** must render on the device (Step 2): the built-in LVGL
  Montserrat fonts are ASCII only, so Latin-1 + Latin Extended-A fonts are generated with
  `lv_font_conv` and committed.
- Home Assistant is reached over plain `http`/`ws` on the LAN; TLS is not planned.

## Secrets

- Never commit Wi-Fi passwords, Home Assistant tokens or any credential; they live only in
  NVS on the device. Never log a token (log `token_set len=N` instead).
- `.gitignore` excludes `.pio/`, `*.local.ini`, `secrets*`, `.env*`.
- Do not copy files from the LilyGo vendor repository (GPL-3.0); use hardware facts only.
