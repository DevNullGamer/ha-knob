# HA Knob

Firmware that turns the **LilyGo T-Encoder Pro** (ESP32-S3R8, 16 MB flash, 8 MB OPI PSRAM,
390x390 round AMOLED, capacitive touch, rotary encoder with push button, buzzer) into a
Home Assistant control knob.

**Status: v0.5.1, Step 5b.2 (own WebSocket client, large messages).** The firmware drives the hardware
(display, touch swipes, encoder, button, buzzer), loads its configuration from NVS, joins
Wi-Fi (or opens a setup access point), serves a setup web UI for Wi-Fi, Home Assistant and
device settings, and keeps an authenticated, self-healing WebSocket connection to Home
Assistant. Entity selection and control come in later steps.

## First-time setup

1. Flash the firmware and the web UI files once (`-t upload` and `-t uploadfs`, see below).
2. The device starts its setup access point. The screen shows its name (`HA-Knob-XXXX`) and
   its WPA2 password (random, generated on first boot).
3. Join that network with a phone or laptop. The captive portal opens the setup page; if it
   does not, open `http://192.168.4.1/`.
4. In the **Wi-Fi** tab, scan or type the network name, enter its password (or tick
   "Open network") and save. The device joins the network while the access point stays up.
   Once connected, the page shows the new address, and the access point shuts down 60 s later.
5. From then on the setup page is at `http://<device IP>/` or `http://ha-knob.local/` (the
   status screen shows both).
6. In the **Home Assistant** tab, enter the Home Assistant address (e.g.
   `http://homeassistant.local:8123` or `http://192.168.1.10:8123`) and a long-lived access
   token (Home Assistant: your user profile, Security, "Long-lived access tokens"; best for a
   dedicated non-admin user) and save. The tab and the status screen show the connection
   state ("connected" plus the Home Assistant version once it works).

If the device cannot join the configured network within 30 s, it opens the setup access
point again (and keeps retrying the network every 60 s).

## Prerequisites

- [PlatformIO Core](https://platformio.org/) 6.2 (`pio` on PATH). The `espressif32@6.12.0`
  platform (Arduino-ESP32 core 2.0.17) is downloaded automatically on the first build.
- For host unit tests on Windows: MSYS2 ucrt64 `gcc`/`g++` on PATH.
- The board connected over its native USB port (the default port in `platformio.ini` is
  `COM14`; change `upload_port`/`monitor_port` if yours differs).

The project path may contain spaces; this is verified to work.

Upload times (measured in Step 4, nothing to rebuild): `-t upload` about 53 s, of which
about 30 s are PlatformIO's pre-upload build check and about 16 s the flash write (1.5 MB
app, compressed to 0.86 MB, about 925 kbit/s); `-t uploadfs` about 45 s (the 3.4 MB LittleFS
partition is erased and written in full). Any change to `platformio.ini` (flags, libraries,
partitions) makes the next build a full rebuild of about 3-4 minutes, which is then part of
the upload command.

## Commands

| What | Command |
|---|---|
| Build firmware | `pio run -e t-encoder-pro` |
| Flash firmware | `pio run -e t-encoder-pro -t upload` |
| Flash web UI files (`data/` to LittleFS) | `pio run -e t-encoder-pro -t uploadfs` |
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
CFG saved controls=<n> bytes=<n> cfg_written=<0|1>   cfg_written=0: only secrets changed
CFG unchanged                            accepted patch that changes nothing (nothing written)
CFG secrets wifi_pw_set=<0|1> ha_token_set=<0|1> web_pw_set=<0|1> ap_pw_set=<0|1>[ token_set len=<n>]
CFG factory reset ok=<0|1>
ENC d=<+1|-1> count=<n>                  one line per detent, clockwise = +1
BTN down                                 debounced press (the buzzer clicks)
BTN up + BTN short                       release before 600 ms
BTN long                                 held 600 ms (fired while held; no "short" after it)
BTN verylong                             held 10 s
BTN up                                   release after long / verylong
TOUCH swipe=<left|right>                 horizontal swipe (taps are ignored)
HB up=<s> psram=<bytes> flash=<bytes> touch=<..> count=<n> ctl=<controls> cfg=<defaults|loaded> wifi=<off|ap|sta|ap_sta> ip=<ip> ha=<state> heap=<bytes> heap_min=<bytes> loopmax=<ms>   every 5 s
WIFI ap ssid=<HA-Knob-XXXX> ip=192.168.4.1   setup access point up
WIFI ap off                              setup access point shut down
WIFI sta connecting ssid=<ssid>
WIFI sta connected ip=<ip> rssi=<dBm>
WIFI sta lost
WIFI fallback ap_sta                     not connected within 30 s: setup AP up as well
WIFI restart reason=config               Wi-Fi credentials changed
WIFI mdns host=<hostname>.local ok=<0|1>
WIFI scan start | WIFI scan done networks=<n> failed=<0|1> | WIFI scan timeout | WIFI scan start failed err=<hex>
WIFI scan stop grace expired (no SCAN_DONE)   a stopped scan never reported SCAN_DONE within 3 s
WEB started port=80 fs=<0|1> files=<n>   fs=0: LittleFS not mounted (run uploadfs); files = web UI files found
LOG dropped=<n>                          n lines were dropped while nothing read the serial port
WEB config saved restart_wifi=<0|1>
WEB config rejected field=<field> error=<error>
WEB reboot requested | WEB factory reset requested | WEB ha retry requested
HA config url=<url> token_set len=<n>    HA settings applied (the token itself is never logged)
HA config none url_set=<0|1> token_set=<0|1>   Home Assistant not configured
HA connect attempt=<n> url=ws://<host>:<port><path>   one line per connection attempt
HA dns host=<host> ok=<0|1>[ ip=<ip>]    name resolution of the attempt
HA tcp ok=<0|1> ms=<n>                   TCP connect (blocks the main loop, at most 2 s)
HA socket open ms=<n>                    WebSocket handshake done (ms since the TCP connect started)
HA handshake failed error=<http NNN|bad_accept|bad_response|closed> | HA handshake timeout
HA rx len=<n> ms=<n> handle_ms=<n>       a message of 64 KB or more: receive time, processing time
HA frame dropped len=<n> ms=<n> reason=<too_large|no_memory|binary>   message discarded, connection stays up
HA socket closed by=server code=<n> reason=<text>   server sent a WebSocket Close
HA socket closed by=peer                 TCP connection closed or reset
HA protocol error reason=<r>             invalid WebSocket data from the server (connection closed)
HA state=<state>[ ha_version=<v>][ error=<error>][ retry_in_ms=<ms>]   every state change
HA retry                                 web UI Retry accepted
SYS reboot | SYS factory reset           performed by the main loop just after the response
```

In `HB`, `ip` is the station IP when connected, else `192.168.4.1` while the setup AP is up,
else `0.0.0.0`; `ha` is the Home Assistant connection state, `heap` / `heap_min` the free
internal heap now / the lowest since boot, and `loopmax` the longest main-loop pass (ms,
without the loop's own 5 ms delay) since the previous `HB`. SSIDs are logged, passwords and
tokens never.

Serial output never blocks the firmware: when the board is plugged into a PC but no program
reads the port, the 4 KB transmit buffer fills up and further lines are dropped (counted in
`LOG dropped=`) until a monitor is opened. (Arduino-ESP32 2.0.17's USB serial would
otherwise hang the writing task in that situation.)

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

- **NVS is 64 KB** (20 KB in the default table): rewriting the `cfg` blob (at most 12 KB)
  needs space for the old and the new copy (about 25 KB) next to the secrets and the
  Wi-Fi/PHY data.
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
| `cfg` | blob | Non-secret config JSON (below), at most 12 KB (12288 bytes; 8 KB before Step 5b.3) |
| `wifi_pw` | string | Wi-Fi password (secret) |
| `ha_token` | string | Home Assistant long-lived token (secret, at most 512 characters) |
| `web_pw` | string | Optional web UI password (secret; none = no password, the default) |
| `ap_pw` | string | Random 10-character WPA2 password for the setup AP; generated on first boot, kept by factory reset |

On the very first boot (nothing stored) the defaults are used and saved, so the next boot
logs `CFG loaded`. A corrupt or invalid stored config, or one with a newer schema, gives
defaults for that boot and is left in place until the next save.

Downgrade note: firmware before Step 5b.3 (the v0.5.1 release and older) accepts at most
8 KB. After a downgrade a stored config larger than that (e.g. many controls plus a
custom CA) is
reported as `CFG defaults reason=corrupt`; the old firmware boots on defaults (setup AP)
and leaves the blob in place until the next save. A config without `ha.ca_pem` /
`ha.tls_insecure` (stored by v0.5.1 or older) loads unchanged: the keys take their defaults.

Non-secret config, schema 1 (defaults shown; `controls` example):

```json
{
  "schema": 1,
  "device":  { "hostname": "ha-knob", "brightness": 80, "dim_after_s": 30, "dim_level": 15,
               "off_after_s": 300, "buzzer": true, "encoder_reverse": false, "accel": true,
               "send_interval_ms": 250 },
  "wifi":    { "ssid": "" },
  "ha":      { "url": "", "ca_pem": "", "tls_insecure": false },
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
| `ha.url` | empty (not configured) or `http(s)://host[:port][/prefix]`, host up to 253 characters, port 1 to 65535, prefix of printable ASCII without spaces (the device only connects to `http` URLs without `?`/`#`, see Home Assistant) |
| `ha.ca_pem` | empty (verify against the built-in public CA bundle) or 1 to 3 PEM `CERTIFICATE` blocks and nothing else, at most 4096 bytes after normalisation (`length`). Normalised on input: CRLF/CR become LF, trailing spaces and tabs are removed from every line, surrounding whitespace is trimmed and one trailing LF added; whitespace only becomes empty. Any other content, e.g. a `PRIVATE KEY` block, text around the blocks, a missing END line, or a body whose outer SEQUENCE does not start with a nested SEQUENCE (the tbsCertificate; a private key relabelled as `CERTIFICATE` starts with an INTEGER), is `not_a_certificate`; more than 3 blocks `too_many`; a body that is not strict Base64 `invalid_base64`; a body that is not one DER SEQUENCE whose length matches `invalid_der`. Only allowed with an `https` URL (`requires_https`) |
| `ha.tls_insecure` | boolean, default `false` (skip certificate verification). Only allowed with an `https` URL (`requires_https`) and an empty `ha.ca_pem` (`conflicts_with_ca_pem`) |
| `ha.token` | up to 512 printable ASCII characters, no spaces |
| `web.password` | up to 64 characters, no control characters |
| `controls` | at most 24; the serialised config must fit in 12288 bytes (`controls` / `too_large`, also when a large `ha.ca_pem` pushes it over) |
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

Writes are skipped when nothing changes (flash wear): the `cfg` blob is not rewritten
when the serialised config equals the stored one, and a secret set to its stored value
is not rewritten, so e.g. `POST /api/config {}` writes nothing (`CFG unchanged`).

Patch semantics (`mergePatch`, used by `POST /api/config`): a section present in the patch
replaces that section **literally** (keys missing inside it take their defaults, so a
`wifi` section without `ssid` unconfigures Wi-Fi); secrets (`wifi.password`, `ha.token`,
`web.password`) are kept when absent or `""`, set when a string, cleared when `null`.
Clients must therefore always send whole sections, `ssid` in every `wifi` section, `url` in
every `ha` section together with `ca_pem` and `tls_insecure` (absent = `""` / `false`, so
leaving them out resets the TLS settings), and the Wi-Fi password together with the SSID
(`null` = open network). Unconfiguring Home Assistant therefore sends
`{"ha":{"url":"","ca_pem":"","tls_insecure":false,"token":null}}`; an empty URL with a CA or
`tls_insecure` set is rejected (`requires_https`).
`restartWifi` is reported when the SSID changes or the password is set or cleared for a
configured SSID (not for an SSID that was and stays empty); the web server additionally
compares the stored credentials before and after, so saving the same password again does
not restart Wi-Fi.

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
| Path-only change, or `http` to `https` (TLS upgrade) on the same host:port | Kept (whatever the TLS fields are) |
| `https` before and after on the same host:port, and `ha.tls_insecure` turns on (`false` to `true`) or `ha.ca_pem` becomes non-empty and differs from the stored one (after normalisation) | New `ha.token` required, else `token_required` (the stored token is never sent over a connection that is trusted less) |
| `https` on the same host:port, CA removed or `tls_insecure` turned off (tightening), or the same CA resent (also with other line endings) | Kept |

The TLS fields are validated before the guard runs, so an unusable CA reports its own
error (e.g. `ha.ca_pem` / `not_a_certificate`) rather than `token_required`.

Secrets are never returned: the redacted JSON only carries `wifi.password_set`,
`ha.token_set` and `web.password_set`.

Save order (`ConfigStore::save`): the old `ha_token` / `wifi_pw` are removed first when they
change (the save aborts if that fails), then `cfg` is written, then the new secret values.
An interrupted save can therefore leave "new URL, no token", never a new URL with the old
token.

## Wi-Fi

| Situation | Behaviour |
|---|---|
| No SSID configured | Setup AP `HA-Knob-XXXX` (last two MAC bytes) at `192.168.4.1`, WPA2 with the stored `ap_pw`; DNS answers every name with `192.168.4.1` (captive portal) |
| SSID configured | Station connects (one retry after 15 s). Not connected within 30 s: the setup AP comes up as well (`ap_sta`) and the station retries every 60 s |
| Station connects while the AP is up | AP stays up for 60 s (so the setup page can show the new IP), then shuts down |
| Connection lost | Reconnect at once; the 30 s fallback applies again |
| Credentials changed in the web UI | Wi-Fi restarts 1 s after the response; an AP that is up stays up during the attempt |
| Hostname changed | mDNS restarts with the new name; DHCP uses it from the next connection |

mDNS announces `<hostname>.local` with an `_http._tcp` service. The Wi-Fi driver's own
credential storage and auto-reconnect are disabled: the password lives only in the
`haknob` NVS namespace, and every retry is scheduled by `src/net/WifiManager`.

## Web server and API

ESPAsyncWebServer on port 80. Static files come from LittleFS (`data/`: `index.html`,
`app.js`, `style.css`; plain files, no build step) with `Cache-Control: no-cache`. If the
files were never flashed, `/` answers 503 with a hint to run `uploadfs`.

| Method / path | Response |
|---|---|
| GET `/api/status` | `{fw, uptime_s, heap_free, heap_min_free, psram_free, touch_variant, wifi:{mode, ssid, connected, ip, rssi, ap_active, ap_ssid, ap_ip, hostname}, ha:{state, ha_version, last_error}}` (see Home Assistant) |
| GET `/api/config` | Redacted config (including `ha.ca_pem` and `ha.tls_insecure`, which are not secret): secrets only as `wifi.password_set`, `ha.token_set`, `web.password_set` |
| POST `/api/config` | Patch (see Configuration). `200 {ok:true, restart_wifi}`, `400 {error, field}` (validation), `500 {error:"nvs_write_failed", field}` (storage failed; the config may be partly written and is re-read) |
| GET `/api/wifi/scan` | `{scanning, age_s, failed, networks:[{ssid, rssi, secure}]}`; starts a new scan when there is none yet, the last one failed or it is older than 10 s. Poll until `scanning` is false (a scan takes about 2-5 s; requests during a scan can take up to about 3 s because the radio briefly leaves its channel) |
| POST `/api/ha/retry` | `200 {ok:true}`; leaves `auth_failed` (or ends a backoff wait) and reconnects. A no-op in other states |
| POST `/api/reboot` | `200 {ok:true}`, reboots about 0.75 s later |
| POST `/api/factory-reset` | `200 {ok:true}`, then erases the config and secrets (not `ap_pw`) and reboots |

Request guards (`lib/core/src/web/RequestGuard`, applied to every request in this order):

| Check | Result |
|---|---|
| `Host` is not the station IP, `192.168.4.1`, `<hostname>` or `<hostname>.local` (any case, optional `:port` and trailing dot) | `403`; while the setup AP is up, a GET/HEAD of a non-`/api` path gets `302` to `http://192.168.4.1/` (captive portal) |
| Method other than GET, HEAD, POST (e.g. `OPTIONS`) | `405` |
| POST without `Content-Type: application/json` or without `X-HAKnob: 1` | `403` |
| POST with a chunked body (no `Content-Length`) | `411` |
| Body larger than 16 KB | `413` |
| Web password set and no valid `Authorization: Basic` for user `admin` | `401` |

Only GET reaches the handlers and static files; HEAD is answered with 404. URL paths are
compared with all their bytes, so a decoded `%00` (e.g. `/index.html%00`) is 404. With a web
password set, a POST body is not even buffered without valid credentials.

No CORS headers are ever sent. Every response carries `X-Frame-Options: DENY`,
`X-Content-Type-Options: nosniff`, `Referrer-Policy: no-referrer` and a
`Content-Security-Policy` allowing scripts and styles only from the device. API responses
are `Cache-Control: no-store`.

Threading: request callbacks run in the `async_tcp` task. They never touch LVGL or Wi-Fi;
they read a snapshot the main loop publishes (under a mutex), write the config only via
`ConfigStore::applyPatch`, and hand Wi-Fi scans, config re-reads, reboot and factory reset
to the main loop through atomic flags.

## Home Assistant

The device keeps one WebSocket connection to `<HA URL>/api/websocket` (`ws://`) and
authenticates with the long-lived token. REST is not used. **`https` URLs are refused**:
they can be stored, but the device never connects to them (`no_config` with `last_error`
`tls_not_supported`), so the token is never sent unencrypted to a TLS port. Use the
`http://` address of Home Assistant on the local network.

| `ha.state` | Meaning |
|---|---|
| `no_config` | No address or no token stored; or the stored address is not used: `last_error` `tls_not_supported` (https) or `invalid_url` (e.g. a `?` or `#` in the address) |
| `connecting` | Resolving the name, connecting, waiting for Home Assistant's `auth_required` |
| `authenticating` | Token sent, waiting for the answer |
| `subscribing` | Subscribing to the selected entities (from Step 6; Step 5 subscribes to nothing) |
| `ready` | Connected and authenticated; `ha_version` is the Home Assistant version |
| `auth_failed` | Home Assistant rejected the token (`last_error` `auth_invalid: <message>`). **No automatic retry** (Home Assistant would ban the IP after repeated failures): save a new token, or press Retry |
| `backoff` | Not connected; the next attempt follows after 1, 2, 4, 8, 16, then every 30 s. `last_error` says why (`dns_failed`, `connect_failed`, `handshake_failed: http <code>` / `bad_accept` / `bad_response` / `closed` (the server closed the connection before answering the upgrade, e.g. a proxy refusing it), `handshake_timeout`, `connect_timeout`, `auth_timeout`, `pong_timeout`, `connection_lost`, `closed_by_server`, `protocol_error`, `send_failed`, `network_down`, `network_lost`) |

- Without Wi-Fi the client waits in `backoff` and connects as soon as the station is up.
- Liveness: an HA `ping` every 30 s; no `pong` within 10 s closes the connection and
  reconnects. No `auth_required` within 20 s of connecting, or no answer to the token
  within 10 s, also counts as a failed attempt.
- Message ids start at 1 on every connection.
- Name resolution (also `.local` names via mDNS) runs in the background. The TCP connect
  itself blocks the main loop for up to 2 s when the host does not answer at all. The
  WebSocket handshake that follows does not block; no `101` answer within 10 s is
  `handshake_timeout`.
- The WebSocket client is the project's own (`lib/core/src/ws` codec, `src/ha/WsTransport`
  adapter). It answers the server's WebSocket pings (Home Assistant's heartbeat) and reads
  at most 32 KB or 8 ms per main-loop pass, so even multi-MB messages never stall the
  loop. Messages are assembled in PSRAM, up to 4 MB; above 64 KB a message is only
  accepted while the largest free PSRAM block is at least 1.5 times its size. A larger,
  binary or unstorable message is skipped without buffering (`HA frame dropped`) and the
  connection stays up. Messages over 256 KB are parsed envelope-only (`type`, `id`,
  `success`, `error`), so they never need a JSON document of their own size.
- Changing only the path of the address, or `http` to `https` on the same host and port,
  keeps the token; any other address change needs the token again (see Configuration).

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
| `ESP32Async/AsyncTCP` | 3.5.0 | TCP layer of the web server; maintained fork, core 2.x compatible |
| `ESP32Async/ESPAsyncWebServer` | 3.12.1 | Web server (middleware, static files); maintained fork, core 2.x compatible. PlatformIO prints "Ignored `Hash` dependency": that dependency is for ESP8266 only |
| `throwtheswitch/Unity` | 2.6.1 | Host test framework (native env); pinned to the version PlatformIO resolved |

`links2004/WebSockets` 2.7.3 (Step 5) was removed in v0.5.1 (Step 5b.2): it read every frame
in one blocking loop into an unguarded 15 KB-limited buffer, would silently fall back to an
unverified TLS connection and could not shorten its TLS handshake timeout. The project's
own WebSocket client replaces it (no new library).

Licences:
- LVGL MIT, SensorLib MIT, ArduinoJson MIT, AsyncTCP LGPL-3.0, ESPAsyncWebServer LGPL-3.0.
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
src/util/         Log.h (non-blocking serial logging)
src/net/          WifiManager (STA, setup AP, captive DNS, mDNS), WebServer (REST API, static files)
src/ha/           HaClient (HaSession wiring, logging), WsTransport (WebSocket client on WiFiClient)
src/ui/           Ui (screen manager), Widgets, screens/ (Boot, Setup, Status), fonts/ (generated)
data/             web UI (index.html, app.js, style.css), flashed with uploadfs
lib/core/         pure C++17 logic, no Arduino/ESP headers, unit-tested on the host
  src/config/     ConfigModel (schema, JSON, validation, redaction, patch merge)
  src/control/    DomainActions (per-domain actions, presses and step ranges)
  src/ha/         HaUrl (URL -> WebSocket endpoint), HaProtocol (messages), HaSession (state machine)
  src/input/      QuadratureDecoder, ButtonFsm, Acceleration, SwipeClassifier
  src/ui/         Presenter (number formatting)
  src/web/        RequestGuard (Host allow-list, CSRF, body limit, Basic auth check)
  src/util/       MathUtil, Sha1, Base64
  src/ws/         WsHandshake, WsFrame, WsReceiver (WebSocket client codec, RFC 6455)
test/native/      host unit tests (Unity), run with `pio test -e native`
tools/            serial_check.py, gen_fonts.sh
```

## Security

- No secrets (Wi-Fi passwords, Home Assistant tokens) are ever committed. They live only
  in the device's NVS, entered through the device web UI. `.gitignore`
  excludes `*.local.ini`, `secrets*` and `.env*`.
- Secrets are stored separately from the config JSON, are never returned by the API (only
  `*_set` flags) and are never logged (at most `token_set len=N`).
- NVS is not encrypted: anyone with physical access can read the token. Use a dedicated
  non-admin Home Assistant user for the knob's token.
- The connection to Home Assistant is plain `ws://` on the LAN, so the token can be sniffed
  there (TLS is not planned). The token is only ever sent to the stored address; changing
  the host or port requires entering it again.
- The web UI is plain HTTP on the LAN. The optional web password (Device tab, user `admin`)
  is sent with HTTP Basic auth and can be sniffed on the network; there is no lockout
  after wrong passwords. The setup AP is always WPA2-protected.
- DNS rebinding and CSRF are blocked by the Host allow-list and the JSON + `X-HAKnob`
  header requirement (see Web server and API). The UI sets text with `textContent` only.
