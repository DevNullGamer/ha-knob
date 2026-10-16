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
  default heap: PSRAM on the device only via the SPIRAM malloc threshold (>= 4 KB), and
  growth past the initial reserve can need ~2x its size transiently.
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
