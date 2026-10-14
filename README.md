# HA Knob

Firmware that turns the **LilyGo T-Encoder Pro** (ESP32-S3R8, 16 MB flash, 8 MB OPI PSRAM,
390x390 round AMOLED, capacitive touch, rotary encoder with push button, buzzer) into a
Home Assistant control knob.

**Status: v0.3.0, Step 3 (config model and persistent store).** The firmware has its final
module structure for the hardware (display, touch swipes, encoder, button, buzzer), shows a
boot screen followed by a status screen ("Wi-Fi: not configured"), and loads its
configuration from NVS (applying the display brightness). Wi-Fi, the web UI and the Home
Assistant connection come in later steps.

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
| Erase the whole flash (resets the config and all secrets, incl. `ap_pw`; upload again afterwards) | `pio run -e t-encoder-pro -t erase` |
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
BOOT fw=<version> psram=<bytes> flash=<bytes> touch=<CST816|CHSC5816|none> heap=<bytes> cfg=<defaults|loaded>
CFG loaded controls=<n>                  config read from NVS
CFG defaults reason=<missing|corrupt|schema|invalid>[ field=.. error=..]
CFG ap_pw generated                      first boot only
CFG saved controls=<n> bytes=<n>
CFG secrets wifi_pw_set=<0|1> ha_token_set=<0|1> web_pw_set=<0|1> ap_pw_set=<0|1>[ token_set len=<n>]
CFG factory reset ok=<0|1>
ENC d=<+1|-1> count=<n>                  one line per detent, clockwise = +1
BTN down                                 debounced press (the buzzer clicks)
BTN up + BTN short                       release before 600 ms
BTN long                                 held 600 ms (fired while held; no "short" after it)
BTN verylong                             held 10 s
BTN up                                   release after long / verylong
TOUCH swipe=<left|right>                 horizontal swipe (taps are ignored)
HB up=<s> psram=<bytes> flash=<bytes> touch=<..> count=<n> ctl=<controls> cfg=<defaults|loaded>   every 5 s
```

The CFG lines are printed at boot, usually before the PC has attached, so `BOOT ... cfg=`
and `HB ... ctl= cfg=` are the reliable way to see the load state (`cfg` = how this boot
got its config).

## Configuration

The configuration is stored in NVS, namespace `haknob`. LittleFS only ever holds web
assets, so `uploadfs` never touches the config.

### Partition table

`partitions.csv` (16 MB flash), derived from Arduino-ESP32's `default_16MB.csv`:

| Name | Type | Offset | Size |
|---|---|---|---|
| (unused) | | 0x9000 | 0x5000 (20 KB) |
| otadata | data/ota | 0xE000 | 0x2000 (8 KB) |
| app0 | app/ota_0 | 0x10000 | 0x630000 (6.19 MB) |
| app1 | app/ota_1 | 0x640000 | 0x630000 (6.19 MB) |
| spiffs (LittleFS) | data/spiffs | 0xC70000 | 0x370000 (3.44 MB) |
| nvs | data/nvs | 0xFE0000 | 0x10000 (64 KB) |
| coredump | data/coredump | 0xFF0000 | 0x10000 (64 KB) |

Why it looks like this:

- **NVS is 64 KB** (20 KB in the default table): rewriting the 8 KB `cfg` blob needs space
  for the old and the new copy (about 16.5 KB) next to the secrets and the Wi-Fi/PHY data.
- **otadata must stay at 0xE000 and app0 at 0x10000.** PlatformIO does not take the upload
  addresses from the partition table: the Arduino builder always flashes `boot_app0.bin`
  to 0xE000, and PlatformIO flashes the app to the board's `upload.offset_address`
  (default 0x10000). A first version of this table moved app0 to 0x20000; the app then
  landed outside app0 and the board boot-looped (`rst:0x3 RTC_SW_SYS_RST`).
- So NVS cannot grow in place before 0xE000; it sits at the end of flash instead. ESP-IDF,
  `Preferences` and the PHY calibration find it by its label `nvs`, not by offset. The old
  NVS area 0x9000-0xE000 is left unused.

Upload writes against this table: `bootloader.bin` 0x0 (bootloader area), `partitions.bin`
0x8000 (table), `boot_app0.bin` 0xE000 (otadata), `firmware.bin` 0x10000 (app0);
`uploadfs` takes its offset from the table (0xC70000, spiffs).

**Changing the partition table requires `pio run -e t-encoder-pro -t erase` followed by an
upload**; this wipes the config, all secrets and `ap_pw`.

| NVS key | Type | Content |
|---|---|---|
| `cfg` | blob | Non-secret config JSON (below), at most 8 KB |
| `wifi_pw` | string | Wi-Fi password (secret) |
| `ha_token` | string | Home Assistant long-lived token (secret, at most 512 characters) |
| `web_pw` | string | Optional web UI password (secret; none = no password, the default) |
| `ap_pw` | string | Random 10-character WPA2 password for the setup AP; generated on first boot, kept by factory reset |

On the very first boot (nothing stored) the defaults are used and saved, so the next boot
logs `CFG loaded`. A corrupt or invalid stored config, or one with a newer schema, gives
defaults for that boot and is left in place until the next save.

Non-secret config, schema 1 (defaults shown; `controls` example):

```json
{
  "schema": 1,
  "device":  { "hostname": "ha-knob", "brightness": 80, "dim_after_s": 30, "dim_level": 15,
               "off_after_s": 300, "buzzer": true, "encoder_reverse": false, "accel": true,
               "send_interval_ms": 250 },
  "wifi":    { "ssid": "" },
  "ha":      { "url": "" },
  "controls": [
    { "entity_id": "light.kitchen", "label": "", "action": "brightness", "step": 5, "press": "toggle" }
  ]
}
```

Validation (`lib/core/src/config/ConfigModel.cpp`, first failure is reported as
`{field, message}`):

| Field | Rule |
|---|---|
| `device.hostname` | `[a-z0-9-]`, 1 to 32 characters |
| `device.brightness` | 5 to 100 |
| `device.dim_level` | 0 to 100 |
| `device.dim_after_s`, `device.off_after_s` | 0 to 86400 (0 = never) |
| `device.send_interval_ms` | 100 to 2000 |
| `wifi.ssid` | empty (not configured) or at most 32 bytes |
| `wifi.password` | 8 to 63 printable ASCII characters (none stored = open network) |
| `ha.url` | empty (not configured) or `http(s)://host[:port][/prefix]`, host up to 253 characters, port 1 to 65535 |
| `ha.token` | up to 512 printable ASCII characters, no spaces |
| `web.password` | up to 64 characters, no control characters |
| `controls` | at most 24; the serialised config must fit in 8 KB |
| `entity_id` | `[a-z_]+\.[a-z0-9_]+`, at most 255 characters, domain light, climate, cover, fan, media_player, switch or input_boolean |
| `label` | at most 48 bytes of UTF-8 (æ ø å etc. allowed), no control characters |
| `action`, `press`, `step` | must be offered by the domain (`lib/core/src/control/DomainActions.cpp`); `step` within the action's range |

The same `entity_id` may appear in several controls (e.g. one for brightness and one for
colour temperature). A control without `action`, `step` or `press` gets the domain's
defaults. Unknown keys are ignored; a missing section takes its defaults.

Encoder actions per domain (default first) and step default / range:

| Domain | Encoder actions | Press actions |
|---|---|---|
| light | `brightness` 5 (1-25), `hue` 10 (1-60), `color_temp` 100 (25-1000), `none` | `toggle`, `none` |
| climate | `target_temp` 0.5 (0.1-5), `none` | `none`, `toggle` |
| cover | `position` 10 (1-25), `none` | `toggle`, `stop`, `none` |
| fan | `percentage` 10 (1-50), `none` | `toggle`, `none` |
| media_player | `volume` 2 (1-20), `none` | `play_pause`, `toggle`, `mute`, `none` |
| switch, input_boolean | `none` | `toggle`, `none` |

Patch semantics (`mergePatch`, used by `POST /api/config` from Step 4): a section present
in the patch replaces that section; secrets (`wifi.password`, `ha.token`, `web.password`)
are kept when absent or `""`, set when a string, cleared when `null`.

Wi-Fi guard for `wifi.ssid` changes (compared byte for byte), so a stored WPA2 password is
never handed to a different (possibly rogue) access point:

| SSID change | Stored Wi-Fi password |
|---|---|
| To a different non-empty SSID | The same request must carry `wifi.password`: a string (new password, 8 to 63 characters) or `null` (open network), else `password_required` |
| To `""` (unconfigure), or an empty SSID in general | Cleared, even if the request carries a password |
| Unchanged | Normal secret rules (keep / set / clear) |

Token guard for `ha.url` changes:

| URL change | Stored token |
|---|---|
| Different host:port (host lower-cased, default port 80/443) | New `ha.token` required in the same request, else `token_required` |
| `https` to `http` on the same host:port (TLS downgrade) | New `ha.token` required, else `token_required` |
| URL `""` (unconfigured), cleared or already empty | Cleared, even if the request carries a token |
| Path-only change, or `http` to `https` (TLS upgrade) on the same host:port | Kept |

Secrets are never returned: the redacted JSON only carries `wifi.password_set`,
`ha.token_set` and `web.password_set`.

Save order (`ConfigStore::save`): the old `ha_token` / `wifi_pw` are removed first when they
change (the save aborts if that fails), then `cfg` is written, then the new secret values.
An interrupted save can therefore leave "new URL, no token", never a new URL with the old
token.

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
| `bblanchon/ArduinoJson` | 7.4.3 | Config JSON (both envs); current v7 release, custom allocator support for PSRAM |
| `throwtheswitch/Unity` | 2.6.1 | Host test framework (native env); pinned to the version PlatformIO resolved |

Licences:
- LVGL MIT, SensorLib MIT, ArduinoJson MIT.
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
partitions.csv    flash layout (NVS 64 KB, OTA app0/app1, LittleFS, coredump)
include/          board_pins.h (GPIO map), lv_conf.h (LVGL config), version.h
src/main.cpp      setup()/loop() wiring only
src/hal/          Display, Touch, Encoder, Button, Buzzer, PsramAllocator (hardware glue)
src/config/       ConfigStore (NVS persistence, mutex)
src/ui/           Ui (screen manager), Widgets, screens/, fonts/ (generated)
lib/core/         pure C++17 logic, no Arduino/ESP headers, unit-tested on the host
  src/config/     ConfigModel (schema, JSON, validation, redaction, patch merge)
  src/control/    DomainActions (per-domain actions, presses and step ranges)
  src/input/      QuadratureDecoder, ButtonFsm, Acceleration, SwipeClassifier
  src/ui/         Presenter (number formatting)
  src/util/       MathUtil
test/native/      host unit tests (Unity), run with `pio test -e native`
tools/            serial_check.py, gen_fonts.sh
```

## Security

- No secrets (Wi-Fi passwords, Home Assistant tokens) are ever committed. They live only
  in the device's NVS, entered through the device web UI (from Step 4). `.gitignore`
  excludes `*.local.ini`, `secrets*` and `.env*`.
- Secrets are stored separately from the config JSON, are never returned by the API (only
  `*_set` flags) and are never logged (at most `token_set len=N`).
- NVS is not encrypted: anyone with physical access can read the token. Use a dedicated
  non-admin Home Assistant user for the knob's token.
