# Design notes

Per-step implementation history, design decisions and measurements for HA Knob (chg001
Steps 1-5b.4), moved out of `CLAUDE.md` on 2026-10-16 so it stays short. The binding rules
from these steps are summarised in `CLAUDE.md` ("Invariants"); this file has the details,
reasons and numbers. Read the section for the area you are changing.

## Step 1 hardware results (2026-10-14)

- **Detected touch variant: CST816** (I2C 0x15), so the unit is the newer
  `TFD12MASBCTB4_V0_07` panel with the CO5300 controller.
- `BOOT`/`HB`: `psram=8386215 flash=16777216 touch=CST816`, free heap about 282 KB at boot.
- **Transitions per detent: 2, clockwise = positive. Confirmed by the user on hardware**
  (exactly ±1 per click, no missed steps). Rest positions are AB = 00 and 11; the sequence
  00 -> 10 -> 11 -> 01 -> 00 is clockwise.

## Serial logging and encoder ISR (moved from CLAUDE.md 2026-10-16)

- Serial: core 2.0.17's `HWCDC::write()` busy-loops when the USB host is attached but the
  port is not being read and the TX buffer is full; it counts retries down from the TX
  timeout, so `setTxTimeoutMs(0)` wraps around (~49 days), and 0 also made
  `Serial.flush()` discard the buffer. `setup()` therefore uses `setTxTimeoutMs(1)`. The
  hang froze the main loop after a replug (stale Wi-Fi snapshot -> `Host: <ip>` got 403,
  Wi-Fi scans never finished) until a monitor was opened. `util::logPrintf` writes a line
  only if it fits into the free part of the 4 KB TX buffer (set before `Serial.begin`) and
  counts the rest (`LOG dropped=N`); lines are cut to 255 characters.
- Encoder ISR: Arduino's `attachInterrupt` / `digitalRead` are in flash on core 2.0.17
  (`CONFIG_ARDUINO_ISR_IRAM` is off), so the ISR is registered with
  `gpio_install_isr_service(ESP_INTR_FLAG_IRAM)` + `gpio_isr_handler_add` and reads the
  pins with inline `gpio_ll_get_level`. A later `attachInterrupt` anywhere would reuse this
  IRAM-flagged ISR service, and its flash-resident dispatcher could then run with the
  cache disabled. Placement check: ISR and `readAb` at 0x4037xxxx/0x4038xxxx, data at
  0x3fcxxxxx (`xtensa-esp32s3-elf-nm -C firmware.elf | grep encoder`).

## Step 2 (2026-10-14)

- `QuadratureDecoder` counts detents on arrival at a rest state (00/11) that differs from the
  previous rest state, with the direction of the final transition; invalid jumps count
  nothing and a jump onto a rest state re-syncs the phase. (Replaces the Step 1 resetting
  accumulator.) The Step 1 `ENCRAW` ring buffer is gone; the encoder exposes only an atomic
  detent counter read as a delta by the main loop.
- Fonts: `ui_font_20/28/56` cover ASCII, Latin-1, Latin Extended-A and the bullet;
  20/28 also the `LV_SYMBOL_*` glyphs. Flash cost about 336 KB in total (56 px alone 208 KB).
  The status screen shows "Køkken Æble Ørn" as a font check.
- Swipe: >= 80 px horizontal, <= 700 ms, |dx| >= 2|dy| (`SwipeClassifier::Config`).
- Pending user confirmation: Danish string legible, `BTN long`/`verylong`, swipes.

## Step 3 (2026-10-14)

- NVS namespace `haknob`: `cfg` (blob, JSON, <= 8 KB; 12 KB from Step 5b.3), `wifi_pw`, `ha_token`, `web_pw`,
  `ap_pw` (10 chars, generated on first boot, survives `factoryReset()`).
- First boot (nothing stored) logs `CFG defaults reason=missing` and saves the defaults, so
  later boots log `CFG loaded controls=N`. Corrupt / invalid / newer-schema data gives
  defaults for that boot but is not overwritten until the next save.
- `BOOT ... cfg=<defaults|loaded>` and `HB ... ctl=<N> cfg=<defaults|loaded>` expose the load
  state (`cfg` = how this boot got its config).
  `device.brightness` is applied at boot.
- Duplicate `entity_id`s across controls are allowed (Q2). Domains are limited to light,
  climate, cover, fan, media_player, switch, input_boolean (Q5).
- Secrets in a patch: absent or `""` = keep, string = set, `null` = clear. Token guard: a
  `ha.url` change to a different host:port, or an https -> http downgrade, needs a new token
  in the same patch (`token_required`); clearing the URL clears the token; a path-only
  change or an http -> https upgrade on the same host:port keeps it.
- Wi-Fi guard (mirrors the token guard, against rogue APs): a `wifi.ssid` change (byte for
  byte) to a non-empty value needs `wifi.password` in the same patch, as a string or `null`
  (open network), else `wifi.password`/`password_required`. An empty SSID after the patch
  always clears `wifi_pw`; likewise an empty `ha.url` always clears `ha_token` (a secret is
  never stored without the setting it belongs to).
- `ConfigStore::applyPatch` sets `MergeResult::cfgWritten` when the cfg blob was written,
  even if a later secret write failed (`ok=false`, `nvs_write_failed`).
- `ConfigStore::save` order: remove the old `ha_token`/`wifi_pw` if they change (abort on
  failure), write `cfg`, write the new secrets. Worst case after a failure is "new cfg,
  secret missing". The in-memory config mirrors NVS once `cfg` is written.
- `ap_pw` is generated with `bootloader_random_enable()` around `esp_random()` (RF is not up
  at that point). A stored `cfg` that is empty, > `kMaxConfigJsonBytes` or unreadable is `Corrupt` (never
  overwritten automatically).
- **Custom `partitions.csv`, NVS 64 KB** (was 20 KB in `default_16MB.csv`) so the 8 KB (12 KB from 5b.3) cfg
  blob can be rewritten safely. otadata 0xE000, app0 0x10000 / app1 0x640000 (0x630000
  each), spiffs 0xC70000 (0x370000), **nvs 0xFE0000**, coredump 0xFF0000; 0x9000-0xE000
  unused. Changing the table requires `pio run -e t-encoder-pro -t erase` + upload.
- **PlatformIO offset pitfall:** upload addresses do NOT come from the partition table.
  The Arduino builder always writes `boot_app0.bin` to 0xE000 and PlatformIO writes the app
  to `upload.offset_address` (default 0x10000). otadata must stay at 0xE000 and app0 at
  0x10000, otherwise the board boot-loops (`rst:0x3 RTC_SW_SYS_RST`; happened in Step 3
  with app0 at 0x20000). NVS is found by label, so it may live anywhere. Check after any
  table change: `pio run -e t-encoder-pro -t envdump` (FLASH_EXTRA_IMAGES,
  ESP32_APP_OFFSET) against `gen_esp32part.py .pio/build/t-encoder-pro/partitions.bin`.
- Size: 24 controls with 48-byte labels and typical entity ids serialise to about 3.8-4 KB;
  extreme cases (255-char entity ids) are rejected by `validate()` as `controls/too_large`.
- `parseUrlEndpoint()` in ConfigModel is a minimal URL check; Step 5's `HaUrl` should take it
  over so URL parsing has one implementation.

## Step 4 (2026-10-15)

- Libraries: `ESP32Async/AsyncTCP@3.5.0`, `ESP32Async/ESPAsyncWebServer@3.12.1` (as
  planned, no deviation). The "Ignored `Hash` dependency" warning is ESP8266-only.
- `WiFi.persistent(false)` and `WiFi.setAutoReconnect(false)`: credentials exist only in
  `haknob`; retries are scheduled by WifiManager (15 s before the AP fallback, 60 s with
  the AP up). The setup-only state runs `WIFI_AP_STA` with an idle STA so scans work
  without a mode change.
- Wi-Fi restarts only when the SSID or password in use actually differ
  (`net::wifi::applyConfig` compares them), 1 s after the HTTP response. `mergePatch` no
  longer reports `restartWifi` for an SSID that was and stays `""`.
- After `applyPatch` with `cfgWritten` (also on `nvs_write_failed`) the handler sets
  `configDirty`; the main loop re-reads the config (brightness, Wi-Fi, hostname/mDNS).
- `HB` gained `wifi=<off|ap|sta|ap_sta> ip=<ip>`.
- Round 2 (2026-10-15):
  - STA link state is read from ESP-IDF every 100 ms (`esp_wifi_sta_get_ap_info` + STA
    netif address), not `WiFi.status()`. Retries never disconnect an association that is
    still waiting for DHCP, or a running scan.
  - Scans use `esp_wifi_scan_start` directly (active, 120 ms max dwell, own 15 s timeout,
    start retried for 6 s while the driver is busy). `WiFi.scanNetworks(true)` +
    `scanComplete()` report a scan as failed after `max_ms_per_chan * 20` (6 s) while the
    driver is still scanning, and the next start then fails too.
  - `ConfigStore` skips unchanged writes: cfg blob compared byte for byte with the stored
    one, a secret Set to its stored value or a Clear of an absent key becomes Keep.
    `MergeResult::secretsWritten` joins `cfgWritten`; the web handler re-reads the config
    if either is set.
  - The static handler only serves files listed from the LittleFS root at boot, so misses
    (`/api/x`, `/favicon.ico`) never reach LittleFS and its `vfs_api` error logging.
- AP SSID uses the last two bytes of the STA MAC (this unit: `HA-Knob-043C`).
- App size 1.52 MB (was about 0.94 MB): Wi-Fi stack, web server, mDNS, LittleFS.
- Upload timing: about 53 s with nothing to rebuild (30 s PlatformIO pre-upload checks,
  16 s write at about 925 kbit/s with compression and the stub). Not a config problem;
  slow uploads come from a full rebuild after any `platformio.ini` change (3-4 min).

## Step 5 (2026-10-15)

- Step 5 used links2004/WebSockets 2.7.3 (15 KB frame limit, blocking whole-frame reads);
  it was replaced by the own client in Step 5b.2 (see below).
- `HaSession` states: `no_config`, `connecting`, `authenticating`, `subscribing`, `ready`,
  `auth_failed`, `backoff`. Backoff 1, 2, 4, 8, 16, 30 s (consecutive failures; reset at
  Ready, new URL/token, Retry). `auth_failed` never reconnects by itself (not even after a
  Wi-Fi change); only a new URL/token or `POST /api/ha/retry` leave it. Retry also ends a
  backoff wait. `HaSession::send()` fails the connection only once even when the transport
  reports the close from inside `sendText()`. Timeouts: 20 s open -> auth_required, 10 s auth answer, ping every 30 s
  after auth_ok, 10 s pong, 15 s pending requests. Ids restart at 1 per connection.
- Step 5 passes no entity ids (no subscription; Ready right after auth_ok). The session
  already implements Subscribing for a non-empty list; Step 6 wires the controls' ids and
  live resubscription.
- `lastError` explains why the session is not Ready (`connect_failed`, `connection_lost`,
  `dns_failed`, `connect_timeout`, `auth_timeout`, `pong_timeout`, `send_failed`,
  `network_down`/`network_lost`, `auth_invalid: <HA message>`, `invalid_url`); cleared on
  Ready. `ha_version` is cleared by every new attempt.
- `ha.url` validation and the token guard accept exactly what v0.4.0 accepted. `HaUrl`
  flags a `?`/`#` (`hasQueryOrFragment`); the session refuses such a URL (`no_config`,
  `invalid_url`). Until Step 5b.4 the session refused https URLs (`no_config`,
  `tls_not_supported`, `Transport::open` never called; the adapter refused a TLS endpoint
  too) so the token never went to a TLS port in plaintext; since 5b.4 they connect over
  wss. `validate()` always accepted https, so no stored config became invalid.
- Transport: the host is resolved first with lwIP `dns_gethostbyname` run in the tcpip task
  (`tcpip_callback`), so an unresolvable name never blocks the main loop (kept in Step 5b.2).
- `HB` gained `ha=<state> heap=<bytes>`. Free heap: 167 KB at boot (Step 4: ~169 KB),
  ~165 KB while connected. App 1.69 MB (26.0 %).
- Web: HA tab (URL, write-only token, state, Retry, Remove). `/index.html%00` and other
  decoded NULs in a path are 404 (paths are compared with their full length). HEAD is 404
  everywhere (only GET reaches the handlers). Basic auth is checked before a POST body is
  buffered. WifiManager waits for a stopped scan's late `SCAN_DONE` (and frees it) before
  the next scan; mDNS is retried every 30 s while `MDNS.begin()` fails.

## Step 5b.2 (2026-10-15)

- Spec: `plan-step5b.md` (chg001). links2004 and `-DWEBSOCKETS_TCP_TIMEOUT` are gone; the
  HA socket is `src/ha/WsTransport` on Arduino `WiFiClient` + the `lib/core/src/ws` codec.
  App size 1.56 MB (24.1 %, was 1.69 MB with links2004).
- Phases: Resolving (async DNS) -> Connecting (`WiFiClient::connect(ip, port, 2000)`; the
  timeout is **milliseconds** on core 2.0.17 and also becomes SO_SNDTIMEO/SO_RCVTIMEO) ->
  Upgrading (non-blocking, 10 s `handshake_timeout`) -> Open. One blocking step per poll:
  the TCP connect runs on the poll after the DNS result.
- **Read only when `available() > 0`** (F7: `WiFiClientSecure::read()` drops the connection
  on WANT_READ); at most 32 KB or 8 ms per poll through a 2 KB internal buffer.
  `WiFiClient::connected()` reads a stale `errno` after a 0-byte `recv()`, so a closed peer
  is detected with `recv(fd, .., 1, MSG_PEEK | MSG_DONTWAIT)` (0 = FIN) when nothing is
  available.
- Messages are assembled in PSRAM (`PsramBufferProvider`, `heap_caps_realloc` SPIRAM); above
  64 KB the largest free PSRAM block must be >= 1.5 x the size. Limit 4 MB; larger, binary
  or unstorable messages are skipped without buffering (`HA frame dropped`,
  `HaSession::onMessageDropped`), the connection stays up. A stalled transfer is ended by
  the session's timers (connect 20 s, auth 10 s, HA ping 30 s + pong 10 s).
- `WsReceiver`: a failed doubled `grow()` for a fragmented message is retried once with the
  exact size before `no_memory`.
- Server PING -> PONG with the same payload. Server Close -> Close reply (echo, but 1000
  instead of 1005/1006/1015 or an unsendable code; empty if the server sent no code) ->
  `closed_by_server`. ProtocolError -> Close 1002 -> `protocol_error`. Session close of an
  open connection sends Close 1000. `close()`/`open()` called by the session inside
  `poll()` are deferred until `poll()` returns (the Text buffer stays valid during
  `onText`).
- `HaProtocol::parseMessage`: messages > 256 KB (`kEnvelopeOnlyBytes`) are parsed with an
  ArduinoJson filter (`type`, `id`, `success`, `ha_version`, `message`, `error.code`,
  `error.message`), `Message::envelopeOnly`; peak allocation for 1 MB about 8 KB.
- `HaSession::configure(url, token, TlsSettings, ids, now)` restarts on a TLS settings
  change too; `Transport::open(endpoint, tls)`. In 5b.2/5b.3 https was still refused
  (`tls_not_supported`, also by the adapter); lifted in Step 5b.4.
- `HB` gained `heap_min=` (lowest internal free heap since boot) and `loopmax=` (longest
  main-loop pass in ms since the previous HB, without the 5 ms delay); `/api/status` gained
  top-level `heap_min_free`.
- Core `log_e` (Arduino `log_printf` -> ROM `ets_printf`) **does reach COM14**: e.g.
  `[E][WiFiClient.cpp:275] connect(): socket error ... "Connection reset by peer"` on a
  refused connect. It does not use `HWCDC::write()` or `putc2` (HWCDC never installs
  itself): `esp_rom_install_uart_printf` sets the ROM flags `g_uart_print` and
  `g_usb_print` (`CONFIG_ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG=1`), so the ROM writes to
  UART0 and straight to the USB-Serial-JTAG FIFO. Measured: with COM14 closed for 2 min
  (buffers full) a refused connect plus its `[E]` line kept the main loop busy 38-78 ms,
  the same as with a reader (39-79 ms), so it does not block. These lines bypass
  `logPrintf`'s drop accounting and can interleave with its output.

## Step 5b.3 (2026-10-15)

- `HaConfig` gained `caPem` (`ha.ca_pem`, "" = built-in CA bundle) and `tlsInsecure`
  (`ha.tls_insecure`, false); not secret, in the cfg blob and `GET /api/config`; schema
  stays 1, a stored config without them loads unchanged (defaults).
- `normalizePem` runs in `parseHa` (stored JSON and patches): CRLF/CR -> LF, trailing
  spaces/tabs per line removed, trim, one trailing LF, whitespace-only -> "". A PEM resent with other line endings compares equal
  (no token requirement, no NVS write).
- `checkCaPem` (field `ha.ca_pem`): `length` (> 4096 after normalisation),
  `not_a_certificate` (anything but 1-3 `CERTIFICATE` blocks separated by blank space),
  `too_many`, `invalid_base64` (strict `core::util::base64Decode`), `invalid_der` (not one
  SEQUENCE whose definite outer length equals the decoded size); an outer SEQUENCE whose
  first element is not a SEQUENCE (relabelled PKCS#8/PKCS#1/SEC1 key) is
  `not_a_certificate`. `checkHaTls` adds
  `requires_https` (both fields; an empty URL counts as non-https) and
  `ha.tls_insecure`/`conflicts_with_ca_pem`. `mergePatch` runs `checkHaTls` before the
  token guard so a bad CA reports its own error.
- `kMaxConfigJsonBytes` 8192 -> 12288 (64 KB NVS holds old + new copy). Firmware before
  5b.3 reports a > 8 KB blob as `corrupt` and boots on defaults (README downgrade note).
- Token guard extension (`tlsTrustWeakened`): old and new URL both https on the same
  host:port, and insecure false -> true or a non-empty CA different from the stored one ->
  `ha.token`/`token_required` unless the patch sets a token. Tightening and http -> https
  keep the token. Insecure -> custom CA also asks for the token (spec rule taken literally).
- Until 5b.4, `ha::configure` passed default `TlsSettings` (the HaClient signature change
  came with 5b.4), https was refused with `tls_not_supported`, and the web UI did not send
  the TLS fields (a save reset them to defaults). All three were resolved in 5b.4.

## Step 5b.4 (2026-10-15)

- https URLs connect over wss (`tls_not_supported` is gone). `HaSession` opens them with
  `endpoint.tls` and the `TlsSettings` from the config (`ha::configure(url, token, tls, now)`;
  main.cpp builds them with `haTlsSettings(config)`). `core::ha::TlsMode`
  (`none|bundle|custom_ca|insecure`, `tlsModeFor()`, `tlsModeName()`; CA wins if both are
  set) is reported as `/api/status ha.tls` and in `HA config ... tls=`.
- **TLS rules (WsTransport):** exactly one mode per attempt, never a fallback (to another
  mode or to plaintext). The one `SecureClient` is reset before every attempt
  (`stop()`, `setCACert(nullptr)` clears CA + insecure, `setCACertBundle(nullptr)`), then
  `setTimeout(2)` (seconds; TCP connect + socket I/O), `setHandshakeTimeout(4)`, and one of
  `setInsecure()` / CA PEM passed to `connect(ip, port, host, caPem, nullptr, nullptr)` /
  `setCACertBundle(_binary_x509_crt_bundle_start)`. Always connect with the pre-resolved IP
  and the URL host (SNI + name check). Never call `setCACertBundle` with anything but the
  IDF bundle symbol, never use `connect(host, ...)` (blocking DNS).
- **Core bug worked around (`ha::SecureClient`):** `WiFiClientSecure::stop()` leaves
  `sslclient->socket` at 0 (stop_ssl_socket zeroes the context), so `fd()` reports 0 and
  the next `stop()` calls `close(0)`, closing whatever VFS fd 0 is (stdin, later possibly a
  LittleFS file of the web server). `SecureClient::stop()` resets it to -1. Use
  `SecureClient`, never a bare `WiFiClientSecure`.
- A custom CA is parsed with `mbedtls_x509_crt_parse` before connecting; any failure
  (also a partial one) is `tls_ca_invalid` with no connection.
- Error mapping: `-0x2700` (and `-0x3000` in bundle mode: the bundle's verify callback
  returns FATAL_ERROR for an unknown issuer) -> `tls_cert_rejected`; core `-1` ->
  `tls_timeout` if >= 4 s elapsed, else `connect_failed` (the core returns -1 for both);
  other negative codes -> `tls_handshake_failed`. Serial: `HA tls mode= host=`,
  `WARN HA tls verify=off` (every insecure connect), `HA tls ok ms= heap=`,
  `HA tls failed error= code=-0x.... ms= msg=<60 chars>`.
- R9 verified: `_binary_x509_crt_bundle_start` (136 CAs, 63.7 KB, flash rodata) links and
  `setCACertBundle` accepts it (`https://www.google.com` -> `tls ok ms=~900`, then
  `handshake_failed: http 404`, as expected for a non-HA host).
- R8: TLS failures add 1-3 core `[E]` lines per attempt (`ssl_client.cpp _handle_error`,
  `WiFiClientSecure.cpp connect(): start_ssl_client: <code>`, bundle: `esp_crt_bundle.c
  Failed to verify certificate`); a TLS peer reset/EOF while open adds one
  (`data_to_read(): (-80)`). Rate-limited by the backoff, no flood, no blocking.
- Measured against a LAN mock (RSA-2048, TLS 1.2 ECDHE-RSA-AES256-GCM): handshake
  0.41-0.47 s; internal heap ~119 KB right after connect, settling at ~115.5 KB over many
  reconnects (no leak over 67 close/reconnect cycles; the first ~15 cycles cost ~230 B
  each, then flat), `heap_min` ~105 KB; ~152-156 KB without a connection; 3 MB message
  over TLS in 6.9-7.3 s (~410-435 KB/s, `handle_ms` ~450 for the envelope-only parse);
  stalled handshake `tls_timeout` after 4.02 s (`loopmax` 4027). App 1.76 MB (+188 KB).
- Web UI: "Certificate (https)" fieldset (CA textarea, "Do not verify" checkbox with
  confirm() and warning), "Certificate check" row from `ha.tls`, explanations for the TLS
  `last_error` values (IP-address hint on `tls_cert_rejected`, "paste only the BEGIN/END
  CERTIFICATE blocks" on CA errors), a distinct `token_required` text when the trust was
  weakened on the same address. Remove sends
  `{url:"", ca_pem:"", tls_insecure:false, token:null}`.
- Status screen: "HA: connected (TLS unverified)" in insecure mode (the version is left
  out so the warning fits the 340 px label).

## Step 6.1 / 6.2 (2026-10-16)

Spec: `plan-step6.md` §3.3, §3.4. Core only; nothing on the device uses it yet (6.4/6.5).

- `core::ha::EntityAttrs` is the one typed attribute model (catalog, store, presenter).
  `kAttributeKeys` lists exactly the 18 JSON keys it reads. `applyAttrs` iterates the
  object's pairs, so only present keys matter: `null` resets (optional empty, name "",
  masks 0), wrong types are ignored, Replace starts from defaults. `"temperature": null`
  resets the value but sets `temperatureKey` (HA reports null for a climate in `off`, and
  the target_temp action must still be offered); `resetAttr("temperature")` (the `-.a`
  of a change) clears both. `hs_color` sets hue + saturation only from an array whose
  first two items are numbers (shorter arrays are ignored, `null` resets both).
- Color modes are a bitmask; HA's `"unknown"` sets no bit, any unrecognised mode sets
  `kColorOther`, which (like every mode but `onoff`) offers brightness.
- `availableActions` keeps table order and always includes `none`; feature bits are HA's
  `ClimateEntityFeature.TARGET_TEMPERATURE` (1), `CoverEntityFeature.SET_POSITION` (4),
  `FanEntityFeature.SET_SPEED` (1), `MediaPlayerEntityFeature.VOLUME_SET` (4). An action
  without a rule is never offered. `stepDefaultFor` ceils the fan's `percentage_step` with
  a 1e-9 tolerance (33.33 -> 34, 25.0000001 -> 25) and clamps to the action range;
  non-positive / non-finite steps take the table default. Fallbacks and clamps come from
  the table (no duplicated constants).
- `metaJson` takes `max_controls` / `max_label_bytes` from `config::kMaxControls` /
  `kMaxLabelBytes`; actions without a step have no `step` member.
- `EntityStore` applies `a`, then `c` (`-.a` resets before `+` is applied, so `+` wins
  for a key in both), then `r`. `version()` counts effective changes, not events: each
  insert / modify / remove whose result differs, `snapshotSeen()` turning true (so an
  empty `{"a":{}}` snapshot is visible too), and a `clear()` that drops entities or the
  snapshot flag. New entities beyond 64 are dropped; non-object `a` entries are ignored.
- `parseMessage` now always parses with a filter and `NestingLimit(20)`. Up to 256 KB the
  filter keeps the envelope, the whole `result`, and the subscribe_entities payload
  (`event.a.*.{s,a.<keys>}`, `event.c.*.{+.{s,a.<keys>},-.a}`, `event.r`); above 256 KB
  the envelope only, as before. Skipped strings are never stored, so a 70 000-byte
  `entity_picture` parses. ArduinoJson counts every container incl. the root and also
  applies the limit while skipping: an attribute value may nest 15 levels in an `a` event
  (5 levels above it) and 14 in a `c` event (6 above), 16 / 15 give `Error`. Before Step 6
  the unfiltered parse of small messages used ArduinoJson's default limit of 10.
- The filter document is built per call with the caller's allocator (~70 nodes, cheap
  next to the parse), like the old envelope filter.

## Step 6.3 (2026-10-16)

Spec: `plan-step6.md` §3.1 (points 3-5, 7), §3.5 (200 body). Core only; the session uses it
from 6.4.

- `util::json_scan` checks structure only: strings (a backslash skips the next byte),
  bracket kinds for the first 64 levels (a `uint64_t` bit stack; deeper levels are only
  counted), scalars as runs of `[A-Za-z0-9+-.]`. Content is validated by ArduinoJson when
  an element is parsed. No recursion, no allocation, every read bounds-checked.
- `EntityCatalogBuilder` units: `{`, one top-level member (key + whole value), `"result":[`,
  one element with its leading `,`, `]`, trailing whitespace + finish. `step(maxBytes)`
  runs whole units until >= maxBytes (0 = 1) were consumed in this call, so the state
  between steps is just phase + position and the output cannot depend on the budget.
  A big non-`result` member is one unit (not seen from HA).
- Envelope (any key order): `id` must equal the expected id (checked when seen, so in HA
  order a mismatch fails before the result is scanned), `type` must be `"result"` (raw
  compare), `success` a bool literal, `error.message` is parsed with a filter. Finish:
  missing id / type / success -> `states_unexpected`; `success:false` ->
  `states_failed: <message>` (`states_failed` without one); no `result` array ->
  `states_unexpected`. Truncated or structurally malformed text and trailing garbage are
  also `states_unexpected` (no separate error code, the §3.1 list has none).
  `NoMemory` from ArduinoJson or the buffers -> `states_no_memory`, except that
  ArduinoJson also reports a kept string > 65535 bytes as `NoMemory`: for a parsed text
  longer than that (`kMaxKeptStringBytes`) it is treated as that string (element skipped;
  an oversized `error.message` gives plain `states_failed`). Failed clears the list.
- Elements: > 256 KB skipped unparsed; parse errors (too deep, kept string > 65535, not an
  object), missing / empty / > 255-byte `entity_id` -> `skipped`. Unsupported domains are
  neither listed nor skipped. `total` counts every element. Beyond 1500 supported entities
  `truncated` is set (later elements are still parsed for `total` / `skipped`).
- Name: `friendly_name`, else the entity id, cut to 96 bytes; state cut to 255 bytes; both
  UTF-8 safe. `step_default` is `stepDefaultFor(actions[0])` (0 for `none`); actions are
  stored as a bitmask over the DomainActions table (no copy of the table).
- Storage: 16-byte records (x86-64 and xtensa; 1500 entries ~ 24 KB) and a NUL-separated text buffer, both `CatalogBuffer`s on the
  caller's allocator (min. 4 KB, doubling; on the device PSRAM), so 1500 entries never
  land in internal RAM as small `std::string` allocations. `CatalogEntry` is a view.
- `catalogToJson` writes the header by hand and serialises one entry at a time with a
  reused JsonDocument (no document for the whole list). The output `std::string` uses the
  default heap: PSRAM on the device only via the SPIRAM malloc threshold (>= 4 KB). Since
  the 6.5 R8 fix it is a one-shot wrapper around `CatalogJsonWriter` (see Step 6.5).
- Known limits: an element > 256 KB is skipped unparsed but still scanned in one step, so
  a multi-MB single entity makes one long main-loop step (may exceed the 6.5 `loopmax`
  gate; a resumable `skipValue` is the remedy if it is measured). A big non-`result`
  top-level member is likewise one unit (HA sends none).
- `peekEnvelope` reads top-level scalar members within the first 1 KB and stops at the
  first object / array value; `hasId` means conclusive. A member counts only if the `,` /
  `}` after its value is within the limit (`{"id":7` cut from `{"id":70..` is inconclusive,
  so is an unterminated `{"id":7`).
- Host measurement (MinGW, -O1): 1 MB synthetic result (7506 lights) in 32 KB slices,
  31 steps, ~23 ms total.

## Step 6.4 (2026-10-16)

Spec: `plan-step6.md` §3.1-3.3, §3.8. Core only; the device glue (WsTransport
`retainText` / `onRxProgress`, HaClient ids and logs) is Step 6.5.

- Subscription: the session tracks `subscriptionId_` (result success) and
  `pendingSubscribeId_` (sent, no result); at most one of them is non-zero. An id-set change
  (compared as a set, so reordering controls does not resubscribe) while Subscribing /
  Ready: drop the pending subscribe from the map, send `unsubscribe_events` for whichever
  id is set (tracked as Unsubscribe; a failure or expiry is only logged), clear the store,
  send `subscribe_entities` if ids are left. Ready stays Ready; Subscribing with no ids
  left becomes Ready. HA handles messages in order, so unsubscribing a still pending
  subscribe works; its late result is then an unknown id. In other states the ids are
  used at the next auth_ok. Only the current pending subscribe's expiry means Backoff
  (`subscribe_timeout`, also after a resubscribe while Ready).
- Events: applied only if `msg.id` equals the active or pending subscribe id (A2: the
  snapshot may come before the result). An envelope-only (> 256 KB) event of ours logs
  `event dropped error=too_large`; an unparsable message whose `peekEnvelope` shows
  `type:"event"` with our id logs `event dropped error=<ArduinoJson error>` (new
  `Message::parseError`). The snapshot is then lost until the next resubscribe /
  reconnect (no automatic recovery). Store cleared on connect(), an id change, a URL /
  token / TLS change and a failed subscribe result; kept during Backoff.
- get_states: `requestStates()` (Ready only; Fetching / Building coalesce) is not in the
  pending map. While Fetching, `onText` peeks first: peeked id == states id, or no id and
  > 64 KB -> `Transport::retainText(data, len)`; a small result whose id is not first is
  recognised after the normal parse and retained the same way. The builder
  (`EntityCatalogBuilder` on the retained text) runs one `step(statesBytesPerTick)` per
  `tick()` before the state switch, so it finishes in any session state.
  `Transport::retainText` defaults to "cannot" (empty) -> `states_no_memory`; the device
  transport overrides it in 6.5 (`WsReceiver::detachDelivered`), test fakes copy.
- Failures: transport drop while Fetching (`too_large` / `no_memory`; `binary` is not
  taken to be it), `states_timeout`, builder errors, `connection_lost` (fail() or connect()
  while Fetching; a failed get_states send too). Building survives a disconnect; a URL /
  token / TLS change aborts (None, catalog dropped). Failed drops an untaken catalog.
- Liveness: `lastRxMs_` (onRxProgress, reset at connect()). Pong timeout when
  `min(now - ping, now - lastRx) >= 10 s`, or `now - ping >= 90 s` regardless of rx
  (`kPongHardTimeoutMs`; extension of §3.2 approved in the 6.4 audit, so a peer that keeps
  streaming but never answers pings is still dropped); get_states expiry when
  `min(now - sent, now - lastRx) >= 15 s` or `now - sent >= 90 s`. Tests that never call
  onRxProgress keep the Step 5 behaviour.
- Late results: a fetch of the current connection that ends without a catalog (timeout,
  a drop taken to be it, a wrong retained message, a builder failure; not a config abort, which closes the transport first)
  leaves its id in `staleStatesId_` (cleared by connect() and once used). A later message
  > 64 KB whose peeked id matches is dropped unparsed (`states late result dropped len=`)
  instead of costing an envelope-only parse (~450 ms for 3 MB). Smaller ones go the
  normal way (unknown result id).
- Pending map: when it is full, a new subscribe evicts the oldest Unsubscribe entry so it
  keeps its expiry. `Purpose::GetStates` was removed (get_states has its own tracking).
  Ids -> empty always calls `becomeReady()`, which also clears an old
  `subscribe_failed: ..`. The logger must not call back into the session.
- Logs: `setLogger(std::function)` gets lines without the "HA " prefix (`subscribe id= ids=`,
  `unsubscribe id= subscription=`, `unsubscribe failed: ..`, `snapshot entities=`,
  `event dropped error=`, `states request id=`, `states retained len=`,
  `catalog failed error=`). `catalog done ... json=` is HaClient's (6.5), with
  `catalogBuildMs()`. `statesChangeCount()` is separate from `changeCount()` so catalog
  progress does not trigger the `HA state=` line.
- `WsReceiver::detachDelivered(len)` hands over the last Text buffer (NUL-terminated); the
  receiver forgets it and grows a fresh one for the next message.

## Step 6.5 (2026-10-16)

Spec: `plan-step6.md` §3.1-3.3, §3.8, §4 Step 6.5. Device glue for 6.1-6.4; v0.6.0.

- `WsTransport::retainText(data, len)` succeeds only for the Text event that
  `handleEvent()` is passing to `onText` right now (pointer and length compared with the
  event, at most once): `WsReceiver::detachDelivered` hands the PSRAM buffer over and the
  `RetainedText` frees it through `PsramBufferProvider::release` (context = the
  provider, which lives as long as the transport). The following `releaseDelivered()` is
  then a no-op. Anything else gets an empty `RetainedText` (session: `states_no_memory`).
  The buffer's capacity is the message size + 1 for an unfragmented message (the
  receiver grows fragmented ones geometrically), so the retained block is not much larger
  than the text.
- `onRxProgress(n, millis())` after every socket read with `n > 0` in Open (before the
  bytes are fed, so a message completing in that read already counts). `lastRxMs_` also
  starts at socket open.
- `rx_stalled`: checked when nothing is available and nothing was read in this poll:
  `WsReceiver::messageBytesSoFar() > 0` (payload bytes of a data message in progress, also
  of one being discarded) and 15 s (`kRxStallMs`) since the last read -> `HA rx stalled
  bytes= ms=` + `fail("rx_stalled")`. A frame header without payload bytes yet does not
  count (the session's pong rule covers it). Note: when a ping is outstanding at, or sent within 5 s of, the stall start, the
  session's pong rule fires `pong_timeout` 10 s after the last received byte, i.e. before
  `rx_stalled`; `rx_stalled` decides otherwise.
- EntityStore memory (audit carry-over: ~10 KB internal RAM for 24 entities, gate 4 KB):
  the map nodes (`EntityState` is a few hundred bytes, mostly `std::optional<double>`),
  ids, states and friendly names are allocated through `util::StdAllocator<T>`, a
  standard allocator over an injected `ArduinoJson::Allocator*` (lib/core stays pure; the
  session passes its allocator, on the device `PsramAllocator`). `EntityAttrs::friendlyName`
  and `EntityState::state` are `util::AllocString` (basic_string on that allocator; same
  `c_str()`, `==`, string_view conversion as before). The allocator is stateful and the
  propagate traits are false: `out = EntityAttrs()` in `applyAttrs` keeps `out`'s
  allocator, entries are built with `EntityState(allocator_)` before they are moved into
  the map, copies keep the source's allocator. Default-constructed objects (host tests,
  the catalog builder's per-element `EntityAttrs`) use the heap. Internal RAM left for the
  subscription: the session's `entityIds_` (~1.5 KB for 24 ids of > 15 chars).
- `ha::configure(url, token, tls, ids, now)`: `main` builds the unique ids in config order
  (`controlEntityIds`) on every config re-read; the session compares id sets, so saving
  unrelated settings never resubscribes. `ha::begin()` installs the session logger
  (`HA ` + line).
- Catalog: `refreshCatalog()` runs after every session call (`statesChangeCount()`) and
  takes a finished catalog; its JSON is then written in slices (below) into
  `shared_ptr<const std::string>`, which is published only when complete; the
  `EntityCatalog` (PSRAM) is freed right after. The JSON's characters reach PSRAM through
  the SPIRAM malloc threshold (`CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL` 4096 in the core's
  sdkconfig; the reserve is >= 4 KB from about 40 entities on, smaller catalogs are
  small). The JSON (published or pending) is dropped whenever the session's fetch state is
  not Ready (new fetch, failure, URL change), so 6.6 can publish it as is.
- R8 (catalog JSON time), measured: one-shot `catalogToJson` took `json_ms=162` for 1286
  entries (138537 bytes; ~0.12 ms per entry, per-entry JsonDocument on PSRAM) and pushed
  HB `loopmax` to 169 ms -> Tester FAIL (spec: split if > 25 ms). Fix:
  `core::ha::CatalogJsonWriter` (lib/core, pure): header in the constructor, then
  `step(maxEntries)` serialises entries with one reused JsonDocument (actions iterated from
  the mask, no `std::vector` per entry) and closes the JSON in the step that writes the
  last entry; output identical for every slice size (`catalogToJson` = loop of
  `step(SIZE_MAX)`). Memory: the constructor sums an exact lower bound of every entry
  (59 fixed characters + id + name + state + quoted action names + commas + 1
  step_default character) and reserves it + 4 bytes per entry once (`reserve()` on an
  empty `std::string` is exact, no doubling). An entry that does not fit (escapes,
  step_default > 5 characters) moves the text (usually at most once; escape-heavy names can need a few) into a string reserved for what is
  needed plus the remaining lower bound + 1 KB (`regrowths()`; never the doubling). HaClient
  shrinks only when > 8 KB is unused (only possible after a regrowth). Host check: 1500
  entries with `"0.25"` defaults and escaped names, 0 regrowths, 1.7 KB unused.
- HaClient drives the writer from `loop()` only (one slice per pass): `step(8)` repeatedly
  until 8 ms (`micros()`) are used, so a pass spends <= ~9 ms on it (plus a regrowth copy
  if one happens). While the writer runs `catalogState()` reports `building` (session is
  already Ready), `catalogJson()` is nullptr; `catalogChangeCount()` is bumped at take and
  at publish. `HA catalog done ... json=<bytes> json_ms=<n> json_passes=<n>
  json_slice_max_ms=<n> json_regrow=<n>` (not in §3.8): `json_ms` is now the wall time
  from take to publish (across passes), `json_slice_max_ms` the longest slice.
- HB gains `ent=<store size>/<unique ids> cat=<fetch state> stack=<loop task high-water,
  bytes (ESP-IDF counts StackType_t = bytes)> psram_free=<bytes>`. `psram_free` is not in
  §3.8; it is needed for the "PSRAM back within 64 KB after the build" gate (the old
  `psram=` is the PSRAM size). Worst-case HB line 242 characters (< 255).
- Stack (R2): `-fstack-usage` on the device build: ArduinoJson recursion costs
  `parseVariant` 48 + `parseObject` 112 (`parseArray` 64) bytes per kept level and
  `skipVariant` 32 + `skipObject`/`skipArray` 32 per skipped level. Deepest kept path is
  `result: true` of a small non-states message (up to 20 levels ~ 3.2 KB); the filtered
  event / catalog paths keep <= 6 levels and skip the rest (~64 B per level). Frames on
  top: `parseMessage` 400, `EntityCatalogBuilder::element` 480. Not measured on the
  device yet (Tester: HB `stack=` with a ~19-level message, gate >= 2048).
- Test hook: `-DHAKNOB_STATES_ON_READY` (test builds only, e.g. via
  `PLATFORMIO_BUILD_FLAGS`) makes HaClient call `requestStates()` on every transition to
  Ready, so the get_states path can be tested on the device before the web API (6.6).
- Build (v0.6.0): RAM 54472 bytes (16.6 %), flash 1789921 bytes (27.6 %); after the R8
  fix RAM 54672 bytes (16.7 %), flash ~1790849 bytes (27.6 %).
