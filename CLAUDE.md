# CLAUDE.md

## Project

HA Knob: Home Assistant controller firmware for the LilyGo T-Encoder Pro (ESP32-S3R8,
16 MB flash, 8 MB OPI PSRAM, 390x390 round AMOLED, touch, rotary encoder, buzzer).
PlatformIO + Arduino framework. Current version 0.8.0.

Approved spec and change log (outside the repo):
`~/.claude/workspace/chgreq/LilyGo T-encoderPro - HA Controller/chg001/`.
One spec step at a time.

## Commands

```
pio run -e t-encoder-pro                 # build
pio run -e t-encoder-pro -t upload       # flash (COM14)
pio run -e t-encoder-pro-test            # test build: + -DHAKNOB_TEST_INPUT ("TIN" serial input)
pio run -e t-encoder-pro -t uploadfs     # flash LittleFS web assets (data/)
pio run -e t-encoder-pro -t erase        # wipe flash incl. NVS config + secrets + ap_pw; upload again after
pio device monitor                       # serial monitor
pio test -e native                       # host unit tests (needs MSYS2 ucrt64 gcc on PATH)
%USERPROFILE%\.platformio\penv\Scripts\python.exe tools/serial_check.py --port COM14 --expect "<regex>" --timeout 20
```

`serial_check.py` never resets or flashes; it waits for each `--expect` regex and exits 0/1.
USB-CDC drops output before the host enumerates: assert on the repeating `HB` line, not
only on `BOOT`.

## Layout and rules

- `include/board_pins.h`: the only place GPIO numbers are defined.
- `include/version.h`: `FW_VERSION`; bump it with releases.
- `include/lv_conf.h`: LVGL 9.2.2 template with project changes listed at the top.
- `lib/core/`: **pure C++17**. No Arduino, ESP-IDF or FreeRTOS includes; only the STL and
  ArduinoJson. Everything here must be testable with `pio test -e native`.
  Hardware glue lives in `src/`.
- `test/native/test_*/`: Unity host tests. `test/embedded/` is reserved for on-device tests.
- `src/main.cpp` is wiring only: it polls the HAL modules, logs, and forwards to `ui::`
  and `src/app/*` (`app::control`, `app::idle`, `app::reset`).
- `src/hal/*`: one namespace per device (`hal::display`, `hal::touch`, `hal::encoder`,
  `hal::button`, `hal::buzzer`); single hardware instance each, state is file-local.
- `src/ui/*`: `ui::` screen manager plus `ui/screens/*`. All LVGL calls happen on the main
  loop only. Use the project fonts `ui_font_20/28/56` (declared once, in `include/lv_conf.h`
  via `LV_FONT_CUSTOM_DECLARE`, so `lvgl.h` is enough), never `lv_font_montserrat_*`
  (disabled; ASCII only). Fonts come from `tools/gen_fonts.sh`; never hand-edit them.
  Pages (Step 6.8): ring of control pages (or NoControls) + Status (pure
  `core::ui::PageRing`); swipe left / right = next / previous, long press = Select list;
  control page rotation / short press -> `app::control`. Screen precedence:
  `ui::targetScreen()`. Views come from the pure `core::presenter::present`; `main`
  rebuilds the shown page only when its `ViewKey` changes. Widgets are never clickable.
- `lib/core/src/config/ConfigModel`: config schema; `fromJson` always returns a valid
  config (stored or defaults + `LoadStatus`); `mergePatch` returns the merged config +
  `SecretChanges` (Keep/Set/Clear). JSON-building APIs take an optional
  `ArduinoJson::Allocator*`; on the device pass `PsramAllocator::instance()`.
- `lib/core/src/control/DomainActions` is the single source of truth for per-domain
  actions, presses, step ranges, supported actions, value math and service calls. Never
  duplicate that table (the web UI gets it via `/api/meta/actions`).
- `src/config/ConfigStore`: the only code that touches NVS namespace `haknob`. All public
  methods lock its mutex; never call LVGL or the HA socket while holding it.
- `src/net/WifiManager` (`net::wifi`): STA / setup AP / captive DNS / mDNS state machine,
  main loop only. `src/net/WebServer` (`net::web`): ESPAsyncWebServer routes.
- `lib/core/src/ha/`: `HaUrl` (the only URL parser; ConfigModel's `parseUrlEndpoint` is an
  alias of it), `HaProtocol` (message builders + classifier), `HaSession` (clock-injected
  state machine over a `Transport`; fake in native tests). `src/ha/HaClient` (`ha::`) wires
  it to `src/ha/WsTransport` (own WebSocket client on `WiFiClient`) and logs; main loop
  only, like all HA socket I/O. Never enable `coalesce_messages`. The token lives only in `ConfigStore` and the session.
- `lib/core/src/ha/EntityAttrs` is the only attribute model (typed fields for exactly
  `kAttributeKeys`; never parse other attributes). `EntityStore` applies
  `subscribe_entities` a/c/r events (max 64 entities). `parseMessage` always uses a filter
  and `NestingLimit(kMaxNesting = 20)`; use `kMaxNesting` for every HA parse.
  Long-lived core containers take an injected allocator (`util::StdAllocator` over
  `ArduinoJson::Allocator*`, nullptr = heap; build elements with it); the session passes
  its PSRAM allocator to its `EntityStore`.
- `util/JsonScan`, `ha/EntityCatalog` (6.3): the get_states result is scanned and
  built in budgeted slices (each `result` element parsed alone with a filter;
  `catalogToJson` is host only); never parse it whole with ArduinoJson.
- **Web callbacks run in the `async_tcp` task**: never call LVGL, `WiFi.*` or main-loop state
  from them. They read the snapshot published by `net::web::publishNetwork` (mutex), write
  config **only via `ConfigStore::applyPatch()`** (never `save()`, which bypasses the token
  and Wi-Fi guards), and request work (config re-read, scan, reboot, factory reset) with
  atomic flags the main loop takes (`net::web::take*`). Never touch HaClient there: the
  entity list is a published `shared_ptr` body, streamed (never copied; max 4 streams).
  New routes use `AsyncURIMatcher::exact` (a plain path also matches `<path>/...`).
- `lib/core/src/web/RequestGuard`: Host allow-list, captive-portal redirect, method,
  CSRF (`application/json` + `X-HAKnob: 1`), body limit (16 KB), Basic auth check; runs
  in the server middleware for every request. Never add CORS headers.
- Web UI (`data/`): vanilla JS, no build step, DOM text via `textContent` only (no
  `innerHTML`), no inline scripts or styles (CSP). Clients always send
  whole config sections (section replace is literal), `ssid` in every `wifi` section, `url`,
  `ca_pem` and `tls_insecure` in every `ha` section (absent = reset to `""`/`false`), and
  the Wi-Fi password together with the SSID (`null` = open).
- `lib/core/src/ws/` + `util/{Sha1,Base64}`: pure WebSocket codec (`plan-step5b.md`,
  DESIGN-NOTES "Step 5b.2"); `WsReceiver` limit 4 MB, oversize / binary -> `Dropped`.
- `lib/core/src/input/QuadratureDecoder.h` is header-only and `always_inline` because the
  encoder ISR calls it; keep anything it uses free of out-of-line calls and flash tables.
- All library versions are pinned exactly in `platformio.ini`; record any change and its
  reason in README "Library versions".

## Hardware facts and gotchas

- **IO3 (VCI_EN) must be HIGH** before the display is initialised.
- Display: Arduino_GFX `Arduino_ESP32QSPI` + `Arduino_CO5300(bus, 4, 0, false, 390, 390, 0,0,0,0)`,
  `begin(40000000)`. LVGL flush areas: even x1/y1 and even width/height
  (`LV_EVENT_INVALIDATE_AREA`). `flushCb` swaps in place
  (`lv_draw_sw_rgb565_swap` + `draw16bitBeRGBBitmap`): keep PARTIAL mode + sync flush.
- **IO0 is the encoder push button and the BOOT strapping pin**: holding the knob during
  reset enters download mode. No hold-at-boot gestures.
- **Never call `Serial.setDebugOutput(true)`, and never define `CONFIG_ASYNC_TCP_DEBUG` or
  `ASYNCWEBSERVER_LOG_DEBUG`**: they route library/ESP-IDF output into the USB serial
  without the `logPrintf` protection below (and blocking writes from the `async_tcp` task).
- **Serial: always log with `util::logPrintf()` (`src/util/Log.h`), never `Serial.print*`**
  (core 2.0.17's `HWCDC::write()` can busy-loop forever; DESIGN-NOTES "Serial logging").
  Keep `setTxTimeoutMs(1)` (never 0) and the 4 KB TX buffer set before `Serial.begin`.
  `logPrintf` drops lines that do not fit (`LOG dropped=N`), max 318 chars per line;
  header-only, not ISR-safe.
- **The screen is the knob cap**: pressing it also clicks the button (touch ~120 ms before
  `BTN down`). User decision: all press actions come from the button; touch gives swipes
  only (`core::SwipeClassifier`); taps must never trigger an action.
- **Encoder ISR is IRAM-safe** (`gpio_install_isr_service(ESP_INTR_FLAG_IRAM)` +
  `gpio_isr_handler_add`, inline `gpio_ll_get_level`). **Never call `attachInterrupt`
  anywhere**, nor `digitalRead` in the ISR (flash-resident on core 2.0.17; DESIGN-NOTES
  "Serial logging and encoder ISR"). Check after changes: `xtensa-esp32s3-elf-nm -C
  firmware.elf | grep encoder` (code at 0x4037xxxx/0x4038xxxx, data at 0x3fcxxxxx).
- Buzzer: core-2.x LEDC API (`ledcSetup` + `ledcAttachPin`), not `ledcAttach`.
- SensorLib drivers get `sda/scl = -1` (`Wire.begin(5, 6)` runs first).

## Invariants from Steps 1-6

Reasons and measurements: `docs/DESIGN-NOTES.md` (one section per step); read the one for your area.

- Input: CST816 touch; encoder 2 transitions per detent, clockwise = positive (confirmed on
  hardware); `QuadratureDecoder` counts on arrival at a different rest state (00/11).
  Swipe thresholds live in `SwipeClassifier::Config`.
- NVS `haknob` keys: `cfg` (JSON blob <= `kMaxConfigJsonBytes` = 12 KB), `wifi_pw`,
  `ha_token`, `web_pw`, `ap_pw` (generated on first boot, survives `factoryReset()`).
  Corrupt / invalid / newer-schema data gives defaults for that boot and is never
  overwritten automatically.
- Secrets in a patch: absent or `""` = keep, string = set, `null` = clear. Token guard:
  a `ha.url` host:port change, an https -> http downgrade, or weakened TLS trust on the same
  https host:port (insecure turned on, or a different non-empty CA) needs a new token in the
  same patch (`token_required`). Wi-Fi guard: a changed non-empty SSID needs
  `wifi.password` (string or `null`). An empty SSID / URL always clears its secret.
- `ConfigStore::save`: remove changed old secrets first, write `cfg`, then the new secrets;
  unchanged writes are skipped.
- Partitions (`partitions.csv`): otadata must stay at 0xE000 and app0 at 0x10000
  (PlatformIO writes fixed offsets; anything else boot-loops). NVS 64 KB at 0xFE0000.
  A table change needs `-t erase` + upload; verify with `-t envdump` against
  `gen_esp32part.py .pio/build/t-encoder-pro/partitions.bin`.
- Wi-Fi: `WiFi.persistent(false)`, `setAutoReconnect(false)`; WifiManager schedules
  retries, reads STA link state from ESP-IDF (not `WiFi.status()`) and scans with
  `esp_wifi_scan_start` (not `WiFi.scanNetworks`). Restart Wi-Fi only when the SSID /
  password in use differ.
- Web: the static handler serves only files listed from the LittleFS root at boot; HEAD is
  404; paths are compared with their full length (decoded NUL -> 404).
- HA session: states, backoff and timeouts in DESIGN-NOTES "Step 5". `auth_failed` never
  reconnects by itself (only a new URL/token or `POST /api/ha/retry`).
- HaSession (DESIGN-NOTES "Step 6.4"): an id-set change while Subscribing/Ready
  unsubscribes + resubscribes on the same connection (stays Ready); only events of the
  active or pending subscribe id reach `entities()`. The get_states result is taken over
  via `Transport::retainText` (never parsed whole) and built in `tick()` slices;
  `takeCatalog()` hands the `EntityCatalog` over once. Session log lines go through
  `setLogger` (no "HA " prefix; the logger must not call back into the session).
- `WsTransport`: one blocking step per poll; `WiFiClient::connect` timeout is in
  milliseconds; read only when `available() > 0`; detect a closed peer with
  `recv(MSG_PEEK | MSG_DONTWAIT)`. Messages > 256 KB are parsed envelope-only. Every read
  with data calls `onRxProgress`; mid-message silence for 15 s -> `rx_stalled`.
  Assembled in PSRAM; `retainText` hands over only the buffer being delivered (no copy).
- HaClient (Step 6.5): `main` passes the controls' unique entity ids on every
  `ha::configure`. get_states only via `ha::requestStates()`; the catalog JSON is built
  once into a `shared_ptr<const std::string>` (`ha::catalogJson()`, only while the fetch is
  Ready). `-DHAKNOB_STATES_ON_READY` (test builds only) fetches on every Ready.
- Loop task stack is 8 KB; HB `stack=` (high-water) must stay >= 2048. ArduinoJson parses
  recursively (~160 B per kept level, ~64 B per skipped level; `kMaxNesting` 20): lower
  `kMaxNesting` before raising the stack.
- TLS: exactly one mode per attempt, never a fallback (to another mode or to plaintext).
  Reset the client before every attempt; connect with the pre-resolved IP and the URL host;
  never `connect(host, ...)`; `setCACertBundle` only with `_binary_x509_crt_bundle_start`.
  Use `ha::SecureClient`, never a bare `WiFiClientSecure` (core `stop()` fd-0 bug).

## Control (Step 7; DESIGN-NOTES "Step 7.x")

- `core::control::ControlEngine` owns holds, throttling, the in-flight cap and notices;
  `src/app/Control` (`app::control`, main loop only) is its only glue; calls go out only
  via `ha::callService` (`millis()` inside).
- One `now` per loop pass for engine / SelectModel calls. `app::control::loop` runs after
  `ha::loop()` and drains every `ha::takeCallResults()` into `onResult` before `tick()`
  (a lost result leaks an in-flight slot).
- HaSession: use `clearPending()`, never `pending_.clear()` (calls must end as
  ConnectionLost).
- `device.encoder_reverse` flips the delta in `main`, never in the ISR; acceleration is
  per loop-pass batch (`Acceleration::onDetents`).
- HA-supplied text in logs only via `core::util::logSafe`. HB worst case 275 of 318
  chars (DESIGN-NOTES "Step 8.3"): recount before adding a field.

## Idle and reset (Step 8; DESIGN-NOTES "Step 8 (summary, 0.8.0)")

- Wake gate: all input passes `app::idle` first, before any routing (false = wake only: no
  action, no click; a button gesture is ignored up to its release). While
  `app::reset::captures()`, input goes only to `app::reset`.
- Factory-reset gesture (hold 10 s + short press) arms only after `hal::button::releaseSeen()`,
  never `isDown()` (IO0 note above).
- Panel off = brightness 0 + DISPOFF, never SLPIN. Beeps only via `hal::buzzer::beep` /
  `click`; a beep never shortens a running one.
- `-DHAKNOB_TEST_INPUT` (env `t-encoder-pro-test`) only: `TIN` input, `BUZ` log, all in
  `#if`; `xtensa-esp32s3-elf-strings firmware.elf | grep -c 'TIN '` / `'BUZ ms='`: 0 normally.

## Secrets

- Never commit Wi-Fi passwords, Home Assistant tokens or any credential; they live only in
  NVS on the device. Never log a token (log `token_set len=N` instead).
- Secrets are never part of `core::config::Config`; they flow only through
  `SecretChanges` and the `ConfigStore` getters, and appear in API output only as `*_set`.
- `.gitignore` excludes `.pio/`, `*.local.ini`, `secrets*`, `.env*`.
- Do not copy files from the LilyGo vendor repository (GPL-3.0); use hardware facts only.
