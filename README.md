# HA Knob

Firmware that turns the **LilyGo T-Encoder Pro** (ESP32-S3R8, 16 MB flash, 8 MB OPI PSRAM,
390x390 round AMOLED, capacitive touch, rotary encoder with push button, buzzer) into a
Home Assistant control knob.

**Status: v0.8.0 (knob and button control Home Assistant entities; idle dim / off; disconnected banner; factory-reset gesture).** The firmware
drives the hardware (display, touch swipes, encoder, button, buzzer), loads its configuration
from NVS, joins Wi-Fi (or opens a setup access point), serves a setup web UI for Wi-Fi, Home
Assistant, controls and device settings, keeps an authenticated, self-healing WebSocket
connection to Home Assistant, shows the configured controls' entities live on control
screens (see "Screens") and changes them with the knob and the button (see "Control").

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
   `http://homeassistant.local:8123`, `http://192.168.1.10:8123` or an `https://` address,
   see "TLS (https)" below) and a long-lived access
   token (Home Assistant: your user profile, Security, "Long-lived access tokens"; best for a
   dedicated non-admin user) and save. The tab and the status screen show the connection
   state ("connected" plus the Home Assistant version once it works).
7. In the **Controls** tab, the device loads the entity list from Home Assistant (a few
   seconds on large installations; **Refresh list** loads it again). Search or filter by
   domain, **Add** the entities you want (the same entity may be added several times, e.g.
   for brightness and colour temperature), choose per control what turning and pressing
   the knob do and the step, order them with **Up** / **Down**, then **Save controls**.
   Only the actions the entity reports are offered; an entity no longer in Home Assistant
   is marked "Not in Home Assistant".

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
| Test build (serial test input, never for normal use) | `pio run -e t-encoder-pro-test [-t upload]` (see Test input) |
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

### Test input

The env `t-encoder-pro-test` is the normal firmware built with `-DHAKNOB_TEST_INPUT`. It
reads lines from the serial port (non-blocking, at most 40 characters; longer lines are
dropped) and injects them like the real input, so device tests can be automated:

| Line | Same as |
|---|---|
| `TIN rot <+n\|-n>` | n encoder detents (1..100) in this loop pass's batch (`encoder_reverse` and acceleration apply) |
| `TIN short` / `TIN long` | a short / long button press (no `BTN down`, so no click) |
| `TIN down` / `TIN up` / `TIN verylong` | the button's press (`BTN down`, with the click) / release / very long press events |
| `TIN swipe left` / `TIN swipe right` | a swipe |

Each accepted line is logged as `TIN <cmd>`, anything else after `TIN ` as `TIN bad <cmd>`.
The test build also logs every buzzer request as `BUZ ms=<duration> started=<0|1>` (0: a
running beep already lasts longer, so nothing changed). The normal build contains none of it
(`xtensa-esp32s3-elf-strings firmware.elf | grep -c 'TIN '` and `grep -c 'BUZ ms='` are 0;
`grep -w BUZ` would miss it, `strings` may print it with a stray leading character).

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
ENC d=<+1|-1> count=<n>                  one line per detent, clockwise = +1 (-1 with encoder_reverse)
BTN down                                 debounced press (the buzzer clicks if device.buzzer, unless the press only wakes the screen)
BTN up + BTN short                       release before 600 ms
BTN long                                 held 600 ms (fired while held; no "short" after it)
BTN verylong                             held 10 s (opens the factory-reset confirmation)
BTN up                                   release after long / verylong
TOUCH swipe=<left|right>                 horizontal swipe (taps are ignored)
IDLE wake by=<encoder|swipe|button> consumed=1   an input woke the dimmed / dark screen and did nothing else
IDLE wake by=<config|toast|reset> consumed=0   a config save, a notice or the reset confirmation woke the screen
DISP level=<on|dim|off> pct=<n>          screen level / brightness applied (on a change, and once at boot)
UI page=<i>/<n> | no_controls | status   page shown after a swipe, the Select list, or a long press without controls
UI select open | choose=<i> | status     Select list opened / control page i or Status chosen
UI select cancel reason=<long|idle|config|setup|reset>   Select list closed without a change
UI view page=<i>/<n> value=<v><unit> state=<text> arc=<v>[<min>,<max>]|- dimmed=<0|1> unconfirmed=<0|1> title=<t> banner=<0|1>   the shown control page changed (unconfirmed=1: a turned value HA has not confirmed yet; banner=1: HA not connected, state = the reason)
UI toast text=<t>                        a control notice shown to the user for 3 s
UI render ms=<n> flush_ms=<f> px=<p>     a slow LVGL pass (ms = the whole lv_timer_handler() call, flush_ms / px = panel transfer time / pixels in it); test builds: every pass >= 5 ms; normal builds: >= 50 ms, at most once per 5 s
CTL press page=<i> press=<press>         short press on control page i
CTL notice=<kind> page=<i> text=<t>      ha_offline / not_found / unavailable / no_value / busy / failed / timeout
HA call id=<n> <domain>.<service> entity=<id> data=<json>   a service call sent
HA call ok id=<n> ms=<rtt> | HA call failed id=<n> error=<code>: <message> | HA call timeout id=<n>
HA calls dropped n=<k> reason=connection_lost   unanswered calls when the connection ended
TIN <cmd> | TIN bad <cmd>                test build only (see Test input)
BUZ ms=<n> started=<0|1>                 test build only (see Test input)
RST open                                 factory-reset confirmation shown (very long press)
RST cancel reason=<rotate|swipe|timeout> confirmation closed, nothing erased
RST ignored reason=not_armed             very long press before the button was first seen released
RST confirm                              confirmed by a short press (then SYS factory reset src=button)
HB up=<s> psram=<bytes> flash=<bytes> touch=<..> count=<n> ctl=<controls> cfg=<defaults|loaded> wifi=<off|ap|sta|ap_sta> ip=<ip> ha=<state> heap=<bytes> heap_min=<bytes> loopmax=<ms> ent=<n>/<ids> cat=<state> stack=<bytes> psram_free=<bytes> disp=<on|dim|off>   every 5 s
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
HA config url=<url> token_set len=<n> tls=<none|bundle|custom_ca|insecure>[ ca_len=<n>]   HA settings applied (token and CA never logged)
HA config none url_set=<0|1> token_set=<0|1>   Home Assistant not configured
HA connect attempt=<n> url=<ws|wss>://<host>:<port><path>   one line per connection attempt
HA dns host=<host> ok=<0|1>[ ip=<ip>]    name resolution of the attempt
HA tcp ok=<0|1> ms=<n>                   ws: TCP connect (blocks the main loop, at most 2 s)
HA tls mode=<bundle|custom_ca|insecure> host=<host>   wss: TLS connect starts (TCP + handshake block the main loop, at most about 6 s)
WARN HA tls verify=off                   wss without certificate verification (every connect)
HA tls ok ms=<n> heap=<bytes>            wss: TCP + TLS handshake done; free internal heap
HA tls failed error=<e> code=-0x<hex> ms=<n> msg=<mbedTLS text, max 60 chars>   wss connect failed (see TLS errors)
HA socket open ms=<n>                    WebSocket handshake done (ms since the TCP connect started)
HA handshake failed error=<http NNN|bad_accept|bad_response|closed> | HA handshake timeout
HA rx len=<n> ms=<n> handle_ms=<n>       a message of 64 KB or more: receive time, processing time
HA frame dropped len=<n> ms=<n> reason=<too_large|no_memory|binary>   message discarded, connection stays up
HA socket closed by=server code=<n> reason=<text>   server sent a WebSocket Close
HA socket closed by=peer                 TCP connection closed or reset
HA protocol error reason=<r>             invalid WebSocket data from the server (connection closed)
HA rx stalled bytes=<n> ms=<n>           a message stopped arriving for 15 s (connection closed, rx_stalled)
HA subscribe id=<n> ids=<k>              subscribe_entities for the k unique entity ids of the controls
HA unsubscribe id=<n> subscription=<id>  the controls changed: old subscription ended (same connection)
HA unsubscribe failed[: <message>]       Home Assistant refused it (or "timeout"); logged only; message max 60 bytes, control characters as ?
HA snapshot entities=<n>                 first state snapshot of a subscription
HA event dropped error=<e>               an entity update that could not be applied (too_large or a JSON error)
HA states request id=<n>                 entity list (get_states) requested
HA states retained len=<bytes>           its result arrived; the list is built from it in slices
HA states late result dropped len=<bytes>   the large result of an abandoned request
HA catalog done total=<n> supported=<n> skipped=<n> build_ms=<n> json=<bytes> json_ms=<n>   list ready
HA catalog failed error=<e>              states_too_large / states_no_memory / states_timeout / states_failed: .. / states_unexpected / connection_lost
HA state=<state>[ ha_version=<v>][ error=<error>][ retry_in_ms=<ms>]   every state change
HA retry                                 web UI Retry accepted
WEB entities refresh requested           POST /api/ha/entities/refresh accepted
SYS reboot | SYS factory reset src=<web|button>   performed by the main loop 750 ms after the request / confirmation
```

A failed TLS connect is also accompanied by one to three core lines such as
`[E][ssl_client.cpp:37] _handle_error(): ... (-9984) X509 - Certificate verification failed`
(Arduino's own logging; they do not block the firmware and are rate-limited by the
reconnect backoff).

In `HB`, `ip` is the station IP when connected, else `192.168.4.1` while the setup AP is up,
else `0.0.0.0`; `ha` is the Home Assistant connection state, `heap` / `heap_min` the free
internal heap now / the lowest since boot, and `loopmax` the longest main-loop pass (ms,
without the loop's own 5 ms delay) since the previous `HB`. `ent` is the number of live
entities / unique entity ids of the controls, `cat` the entity list fetch (`none`,
`fetching`, `building`, `ready`, `failed`), `stack` the loop task stack never used since
boot (bytes), `psram_free` the free PSRAM and `disp` the screen level. SSIDs are logged, passwords and tokens never.

A log line is at most 318 characters plus the newline (longer lines are cut); the longest possible `HB`
line is well below that.

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
               "off_after_s": 300, "buzzer": true, "detent_click": false,
               "encoder_reverse": false, "accel": true, "send_interval_ms": 250 },
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
| `device.dim_after_s`, `device.off_after_s` | 0 to 86400 (0 = never); seconds without input until the screen dims / turns off |
| `device.buzzer`, `device.detent_click` | booleans, default `true` / `false`: the click on press, the error beep / a 5 ms click on every knob step (only with `buzzer`) |
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
| GET `/api/status` | `{fw, uptime_s, heap_free, heap_min_free, psram_free, touch_variant, wifi:{mode, ssid, connected, ip, rssi, ap_active, ap_ssid, ap_ip, hostname}, ha:{state, ha_version, last_error, tls, subscribed, entities, catalog:{state, count, error}}}` (see Home Assistant; `tls` is `none`, `bundle`, `custom_ca` or `insecure`, the mode of the stored address; `subscribed` = unique entity ids of the controls, `entities` = entities with live state, `catalog.state` = `none`, `fetching`, `building`, `ready` or `failed`, `count` = entities in the list) |
| GET `/api/config` | Redacted config (including `ha.ca_pem` and `ha.tls_insecure`, which are not secret): secrets only as `wifi.password_set`, `ha.token_set`, `web.password_set` |
| POST `/api/config` | Patch (see Configuration). `200 {ok:true, restart_wifi}`, `400 {error, field}` (validation), `500 {error:"nvs_write_failed", field}` (storage failed; the config may be partly written and is re-read) |
| GET `/api/wifi/scan` | `{scanning, age_s, failed, networks:[{ssid, rssi, secure}]}`; starts a new scan when there is none yet, the last one failed or it is older than 10 s. Poll until `scanning` is false (a scan takes about 2-5 s; requests during a scan can take up to about 3 s because the radio briefly leaves its channel) |
| GET `/api/meta/actions` | `{max_controls, max_label_bytes, domains:[{domain, actions:[{name, step:{default, min, max}}], presses:[..]}]}`: per domain the encoder actions (default first, `none` last; actions without a step have no `step`) and the press actions (default first). The web UI takes all actions, presses and ranges from here |
| GET `/api/ha/entities` | Entity list for the picker; the first matching case wins: a fetch is running -> `202 {pending:true}`; a list exists -> `200 {fetched_at_s, total, skipped, truncated, entities:[{id, name, state, actions, step_default}]}` (supported domains only, Home Assistant's order; the default action is `actions[0]`, `step_default` belongs to it, 0 for `none`; presses come from `/api/meta/actions`); the last fetch failed -> `502 {error:"fetch_failed", detail}` (`detail` e.g. `states_too_large`); Home Assistant not `ready` -> `503 {error:"ha_not_connected"}`; otherwise `202` and a fetch starts. Poll while `202` (a fetch takes a few seconds on large installations). The list stays in RAM until a refresh, a failure or another Home Assistant address. At most 4 lists are sent at the same time; more parallel requests get `503 {error:"busy"}` with `Retry-After: 1` |
| POST `/api/ha/entities/refresh` | Home Assistant not `ready` -> `503 {error:"ha_not_connected"}`; else drops the list / error and fetches a new one (a fetch already running is kept) -> `202 {pending:true}` |
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
to the main loop through atomic flags (entity list requests through the snapshot). The
entity list is serialised once by the main loop and streamed from that shared copy.

## Home Assistant

The device keeps one WebSocket connection to `<HA URL>/api/websocket` (`ws://` for an
`http` address, `wss://` for an `https` address, see "TLS (https)") and authenticates with
the long-lived token. REST is not used.

| `ha.state` | Meaning |
|---|---|
| `no_config` | No address or no token stored; or the stored address is not used: `last_error` `invalid_url` (e.g. a `?` or `#` in the address) |
| `connecting` | Resolving the name, connecting, waiting for Home Assistant's `auth_required` |
| `authenticating` | Token sent, waiting for the answer |
| `subscribing` | Subscribing to the controls' entities (`subscribe_entities`); straight to `ready` when no control is configured |
| `ready` | Connected and authenticated; `ha_version` is the Home Assistant version |
| `auth_failed` | Home Assistant rejected the token (`last_error` `auth_invalid: <message>`). **No automatic retry** (Home Assistant would ban the IP after repeated failures): save a new token, or press Retry |
| `backoff` | Not connected; the next attempt follows after 1, 2, 4, 8, 16, then every 30 s. `last_error` says why (`dns_failed`, `connect_failed`, `handshake_failed: http <code>` / `bad_accept` / `bad_response` / `closed` (the server closed the connection before answering the upgrade, e.g. a proxy refusing it), `handshake_timeout`, `connect_timeout`, `auth_timeout`, `pong_timeout`, `connection_lost`, `closed_by_server`, `protocol_error`, `send_failed`, `network_down`, `network_lost`, and for `https` the TLS errors below) |

- Without Wi-Fi the client waits in `backoff` and connects as soon as the station is up.
- Liveness: an HA `ping` every 30 s; no `pong` within 10 s closes the connection and
  reconnects. Received bytes count as a sign of life (the pong may wait behind a large
  message), but 90 s after a ping without a pong the connection is closed anyway. A
  message that stops arriving in the middle for 15 s closes it as `rx_stalled`. No
  `auth_required` within 20 s of connecting, or no answer to the token within 10 s, also
  counts as a failed attempt.
- Entities: the unique entity ids of the configured controls are subscribed with
  `subscribe_entities`; their state and the attributes the device uses are kept live in
  PSRAM (`HB ent=`). Changing the controls unsubscribes and subscribes again on the same
  connection (no reconnect); events of an old subscription are ignored.
- Entity list: `get_states` (multi-MB on large installations) is taken over from the
  receive buffer without a copy and turned into a compact list of the supported entities
  in 32 KB slices (about 5 ms each), so the main loop never blocks on it. The list (at most
  1500 entities, names cut to 96 bytes) is kept in RAM only. The web UI gets it from
  `GET /api/ha/entities` (see Web server and API).
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

### TLS (https)

An `https://` address connects over `wss://` (Arduino `WiFiClientSecure`, mbedTLS 2.28).
Each attempt uses exactly one of three modes (`ha.tls` in `/api/status`), never a fallback
to another mode or to plaintext:

| Mode | Config | Verification |
|---|---|---|
| `bundle` (default) | `ca_pem` empty, `tls_insecure` false | Against the public CA list built into the firmware (the ESP-IDF certificate bundle: 136 root CAs, e.g. ISRG Root X1 for Let's Encrypt) |
| `custom_ca` | `ca_pem` = 1 to 3 CA certificates (PEM) | Against these certificates only (your own CA, or a self-signed server certificate itself) |
| `insecure` | `tls_insecure` true | None: the connection is encrypted, but anyone who can intercept it can pretend to be Home Assistant and capture the token. Logged as `WARN HA tls verify=off` on every connect; the status screen shows "HA: connected (TLS unverified)" |

The token is only sent after the TLS handshake (verified per mode) and the WebSocket
upgrade succeeded. Turning verification off or changing the CA on the same address needs
the token again (see Configuration).

Limitations (mbedTLS 2.28 in Arduino-ESP32 2.0.17):
- **TLS 1.2 only.** A server that only allows TLS 1.3 fails with `tls_handshake_failed`.
- **Certificate dates are not checked** (expired or not-yet-valid certificates are accepted;
  the prebuilt SDK has no time support in mbedTLS), and there is no revocation check.
- **IP addresses:** the host is matched against DNS names in the certificate's SAN only,
  never against IP-address SANs; a certificate without SAN is matched by its CN. An
  `https://<IP>` address therefore only verifies if the certificate's CN is exactly that IP
  and it has no SAN. Otherwise use the host name the certificate was issued for (it must
  resolve on the LAN), or the insecure mode.
- Client certificates (mTLS) are not supported.

TLS errors (`last_error`, state `backoff`; the serial line `HA tls failed` adds the mbedTLS
code and text):

| `last_error` | Cause |
|---|---|
| `tls_cert_rejected` | Certificate verification failed (mbedTLS `-0x2700`, or `-0x3000` when no CA in the built-in list issued it): wrong CA, name mismatch (see IP addresses), or a self-signed certificate in `bundle` mode |
| `tls_ca_invalid` | The stored CA PEM passed the config check but mbedTLS cannot parse it; no connection is made |
| `tls_handshake_failed` | Any other TLS error, e.g. the port does not speak TLS (`-0x7200`), TLS 1.3 only (`-0x7780`) |
| `tls_timeout` | The TLS handshake did not finish within 4 s |
| `connect_failed` | The TCP connection was refused or timed out (2 s) |

Cost and timing: the CA bundle and TLS code add about 188 KB of flash (app 1.76 MB). A
connection uses about 37 KB of internal heap (two 16 KB TLS record buffers plus state;
measured 115-119 KB free while connected, about 155 KB without a connection). The TCP
connect and the handshake block the main loop: about 0.4 s against an RSA-2048 server on
the LAN, about 0.9 s for a public host, at most about 6 s (2 s TCP + 4 s handshake).

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

## Screens

After the boot screen the device shows a ring of pages: one **control page** per configured
control (or a **No controls yet** page with the web UI address while there are none),
followed by the **Status page** (Wi-Fi, web UI addresses, HA state, encoder count, last
input). The **setup screen** replaces them while the setup access point is up and the
station is not connected.

| Input | Behaviour |
|---|---|
| Swipe left | Next page (wrapping) |
| Swipe right | Previous page (wrapping) |
| Long press | Opens the Select list; without controls: toggles the No controls / Status page |
| Rotation | Control page: changes the value (see Control); Status page: the encoder count |
| Short press | Control page: the control's press action (see Control) |
| Very long press (10 s) | Factory-reset confirmation (see Factory reset), on every screen |
| Tap | Never does anything |

**Select list** (long press): the control titles plus "Status", the current page selected.
Rotation moves one item per detent (no acceleration, stops at both ends), a short press
shows the selected page, a long press or 8 s without turning closes it without a change;
swipes are ignored. A config change and the setup screen close it. Calls of a control turned
just before keep being sent while the list is open. Screen precedence: boot, setup, Select
list, pages.

**Idle dim / off** (every screen): `device.dim_after_s` after the last input the screen dims
to `device.dim_level` (never brighter than `device.brightness`; 0 = black with the panel
on), `device.off_after_s` after it the panel turns off (brightness 0 and DISPOFF; the
content stays). 0 = never; with `off_after_s <= dim_after_s` it goes straight to off.
Changes are instant. While dimmed or off, the first turn, button press or swipe only wakes
the screen and does nothing else; for the button the whole gesture up to its release is
ignored (also a long press). A touch without a swipe does not wake (the cap press clicks
the button anyway). The screen never dims while the button is held. Saving the config in the
web UI and a notice (toast) wake it too; Home Assistant state changes do not.

A control page shows the title (label, else HA's `friendly_name`, else the entity
id), a 270° arc for the value, the value with its unit, a state line and the page
indicator `i/N` (of the controls). Values: brightness in % (0 % when the light is off),
hue in °, colour temperature in K, target temperature in the control's step (state line
"heat, now 20.3°"), cover position, fan percentage and volume in %; the action `none`
shows the entity state as the value and no arc (`unavailable` / `unknown`: `--`, the state
in the state line; long states in a smaller font). A value HA does not report shows `--`.
Dimmed: HA not connected, the entity `unavailable` or `unknown`, or the entity missing
("waiting..." until the subscription's first state snapshot arrives or for 3 s, then "not
found"). While HA is not connected the page keeps the last values dimmed and the state line
becomes a dark red banner with the reason: "No Wi-Fi" (station not connected), "HA not set
up" (no URL or token), "Connecting to HA..." (connecting, authenticating or subscribing),
"HA token rejected" or "HA offline" (waiting to reconnect). An `unavailable` entity is only
dimmed, without a banner. A page stays selected across a config
change while its number still exists, otherwise the first page is shown.

## Control

Turning the knob on a control page changes its value, a short press runs its press action,
both as Home Assistant `call_service` calls on the WebSocket (`plan-step7.md`):

| Action | Call per turn |
|---|---|
| brightness | `light.turn_on {brightness_pct}`; 0 % = `light.turn_off` |
| hue | `light.turn_on {hs_color: [hue, saturation or 100]}` (wraps at 360°) |
| color_temp | `light.turn_on {color_temp_kelvin}` (the light's min / max, else 2000-6500 K) |
| target_temp | `climate.set_temperature {temperature}` (min_temp / max_temp, else 7-35°) |
| position | `cover.set_cover_position {position}` |
| percentage | `fan.set_percentage {percentage}` (steps of HA's speed size) |
| volume | `media_player.volume_set {volume_level}` |

Presses: `toggle` = `<domain>.toggle` (climate: `turn_on` when off, else `turn_off`),
`stop` = `cover.stop_cover`, `play_pause` = `media_player.media_play_pause`, `mute` =
`media_player.volume_mute` (flips). Presses are not optimistic: the page waits for HA.

- Each detent moves one step (the control's `step`) on the step grid, clamped to the
  range; with `device.accel` detents less than 40 ms apart count x4, less than 80 ms x2
  (the detents of one loop pass are spread evenly over the time since the previous pass).
  `device.encoder_reverse` flips the direction.
- The value on screen follows the knob at once and stays (HA's own updates are not
  shown) while a value is unsent or unanswered and for 1.5 s after the last turn; then HA's
  value is shown again. A failed call ends this at once.
- Per control at most one call per `device.send_interval_ms` (the first at once, then the
  latest value; the final value is always sent, an unchanged value is not re-sent within one hold); at most
  8 unanswered calls overall. A call without an answer for 10 s counts as failed.
- HA not connected: turns and presses do nothing (notice "HA offline", at most every 2 s);
  values not yet sent are dropped, never sent after a reconnect.
- Notices (HA's error message, "No answer from HA", "Entity not found", "Unavailable",
  "No value to change", "Busy, try again") are logged as `CTL notice=` and shown for 3 s
  as a toast (white on dark red, below the value, at most 3 lines; a new one replaces the
  old one), with a 150 ms error beep (at most one per 2 s) when `device.buzzer` is on (the button click on press
  follows `device.buzzer` too). With `device.detent_click` (and `buzzer`) every turn that
  acts clicks 5 ms (one click per loop pass, at most one per 40 ms; not for a turn that only
  wakes the screen). A beep never cuts a longer running one short.
- Arc colour: amber; white while the shown value is a local one HA has not confirmed;
  grey when dimmed (also mid-turn); for a hue control always the colour of the shown hue.
- Some lights restore their previous level when turned on from 0 %; accepted.

## Input handling

| Input | Rule |
|---|---|
| Encoder | One count per detent, counted on arrival at a rest state (00/11); bounce, half-turns back and invalid jumps never count |
| Button | 30 ms debounce; short press < 600 ms (on release); long press at 600 ms (once, while held); very long press at 10 s (factory reset, only after the button was seen released once since boot) |
| Swipe | Horizontal travel >= 80 px, finger down for <= 700 ms, and \|dx\| >= 2 x \|dy\| |
| Wake (dimmed / off screen) | The first turn, press or swipe only wakes the screen (a button gesture is ignored up to its release); a bare touch does not wake |
| Acceleration (`device.accel`) | Detents < 40 ms apart x4, < 80 ms x2, else x1 (per loop-pass batch) |

## Factory reset

Hold the knob for 10 s **while the device is running**; holding it during power-on or reset
enters download mode instead (IO0 is the BOOT strapping pin, see Hardware notes). The Select
list appears at 0.6 s (it closes again), then the confirmation screen at 10 s with a 300 ms
beep (if `device.buzzer`): "Factory reset?", "Press to erase all settings", "Turn to cancel"
and a 10 s countdown. Releasing the knob does nothing; a short press confirms ("Resetting...",
then the reset and a reboot 750 ms later); turning, a swipe or the 10 s timeout cancel and
return to the previous screen. A long press while it is shown is ignored. The confirmation
works on every screen (also Setup, Status and Select) and keeps the screen awake; on a
dimmed or dark screen the hold only wakes it, so hold again.

The gesture only counts after the button has been seen released (30 ms up) once since boot,
so a knob held since reset never opens it. The reset is the same as `POST
/api/factory-reset`: the config and every secret are erased except the setup AP password
(`ap_pw`), and the device restarts in setup mode.

## Fonts

LVGL's built-in Montserrat fonts are ASCII only, so the project generates its own from
Montserrat Medium (SIL OFL 1.1) with `lv_font_conv` **1.5.3**. They are committed in
`src/ui/fonts/` and declared in `include/lv_conf.h` (default font `ui_font_20`); the
built-in Montserrat fonts are disabled.

| Font | Size | Glyphs | Flash |
|---|---|---|---|
| `ui_font_20` | 20 px | text ranges + LVGL symbols (388 glyphs) | 47.6 KB |
| `ui_font_28` | 28 px | text ranges + LVGL symbols (388 glyphs) | 81.6 KB |
| `ui_font_56` | 56 px | text ranges only (328 glyphs) | 209.8 KB |

Text ranges: `0x20-0x7E` (ASCII), `0xA0-0xFF` (Latin-1: æ ø å Æ Ø Å, °, ...),
`0x100-0x17F` (Latin Extended-A), `0x2013-0x2014` (en / em dash), `0x2018-0x2019`,
`0x201C-0x201D` (curly quotes), `0x2022` (bullet), `0x2026` (ellipsis), `0x20AC` (euro).
LVGL symbols: the `LV_SYMBOL_*` glyphs of LVGL's built-in fonts (FontAwesome 5 Free, from
LVGL 9.2.2's `scripts/built_in_font/`). All fonts are 4 bpp, uncompressed.

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
    -r 0x20-0x7E,0xA0-0xFF,0x100-0x17F,0x2013-0x2014,0x2018-0x2019,0x201C-0x201D,0x2022,0x2026,0x20AC \
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
| `lv_font_conv` (npx, font generation only) | 1.5.3 | See Fonts; source fonts from LVGL 9.2.2's `scripts/built_in_font/` |

Build flags: the `t-encoder-pro` env (and the test env that extends it) replaces the core's
default `-Os` with `-O2` (`build_unflags = -Os`). It applies to the project, the libraries
(LVGL, Arduino_GFX, ...) and the Arduino core sources (FrameworkArduino, incl. HWCDC, WiFi,
WiFiClientSecure, LittleFS); only the precompiled ESP-IDF libraries are not affected.
Step 8.8: LVGL renders about 20-25 % faster (the spinning loopmax gate of 60 ms needs it);
the app image grows by about 190 KB (1.85 -> 2.04 MB of the 6.2 MB app slot). After a flag
change re-run the encoder IRAM check (CLAUDE.md, `nm`).

`links2004/WebSockets` 2.7.3 (Step 5) was removed in v0.5.1 (Step 5b.2): it read every frame
in one blocking loop into an unguarded 15 KB-limited buffer, would silently fall back to an
unverified TLS connection and could not shorten its TLS handshake timeout. The project's
own WebSocket client replaces it (no new library).

Licences: HA Knob is MIT (`LICENSE`). Third-party licences, checked against the library
sources and the linker map, and the required notices are in
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md); the full licence texts (verbatim copies,
sources listed in the notices) are in `licenses/`. Releases and the flasher site ship
`LICENSE`, the notices and `licenses/`.
- Files not covered by `LICENSE`: `src/ui/fonts/*.c` (SIL OFL 1.1) and `include/lv_conf.h`
  (MIT, LVGL Kft, derived from `lv_conf_template.h`).
- LVGL, SensorLib and ArduinoJson are MIT; Arduino_GFX is BSD (Adafruit-derived; the 1.5.0
  package ships no licence file, so `license.txt` from tag v1.5.0 is copied to `licenses/`).
- AsyncTCP and ESPAsyncWebServer are LGPL-3.0-or-later. arduino-esp32 is LGPL-2.1-or-later
  as a whole; individual files are Apache-2.0 (e.g. LittleFS, Preferences, HWCDC,
  ssl_client) or MIT (ESPmDNS). The notices give the exact source tags and how to rebuild
  with a modified library or core.
- ESP-IDF v4.4.7 (precompiled into arduino-esp32, and the bootloader) is Apache-2.0 and
  bundles lwIP, littlefs, wpa_supplicant, TLSF (BSD), FreeRTOS, cJSON, esp_littlefs, the
  Xtensa HAL (MIT), mbedTLS (Apache-2.0) and the Mozilla CA list (MPL-2.0); newlib is
  BSD-family. The notices list each with its copyright line.
- Fonts: the generated `src/ui/fonts/*.c` derive from Montserrat 7.200 and Font Awesome
  Free 5.9.0 (SIL OFL 1.1, no Reserved Font Name; icon designs CC BY 4.0). The OFL text and
  attribution are in the notices.
- The web flasher (GitHub Pages only) serves ESP Web Tools 10.4.0 (Apache-2.0), which
  bundles lit (BSD-3), @material/web, esptool-js, improv-wifi-serial-sdk (Apache-2.0),
  pako (MIT/Zlib), tslib (0BSD), atob-lite (MIT) and the esptool flasher stubs
  (GPL-2.0-or-later); their texts are in `licenses/esp-web-tools/`.
- The LilyGo vendor repository is GPL-3.0; this project uses only hardware facts (pin
  numbers, panel quirks) from it and copies none of its files.

## Project layout

```
partitions.csv    flash layout (NVS 64 KB, OTA app0/app1, LittleFS, coredump)
include/          board_pins.h (GPIO map), lv_conf.h (LVGL config), version.h
src/main.cpp      setup()/loop() wiring only
src/app/          Control (turns / presses -> HA calls, notices), Idle (screen dim / off, wake gate), Reset (factory-reset gesture)
src/hal/          Display, Touch, Encoder, Button, Buzzer, PsramAllocator (hardware glue)
src/config/       ConfigStore (NVS persistence, mutex)
src/util/         Log.h (non-blocking serial logging)
src/net/          WifiManager (STA, setup AP, captive DNS, mDNS), WebServer (REST API, static files)
src/ha/           HaClient (HaSession wiring, logging), WsTransport (WebSocket client on WiFiClient / WiFiClientSecure)
src/ui/           Ui (screen manager, pages, toast), Widgets, screens/ (Boot, Setup, Status, Control, NoControls, Select, Reset), fonts/ (generated)
data/             web UI (index.html, app.js, style.css), flashed with uploadfs
lib/core/         pure C++17 logic, no Arduino/ESP headers, unit-tested on the host
  src/config/     ConfigModel (schema, JSON, validation, redaction, patch merge)
  src/control/    DomainActions (per-domain actions, presses and step ranges)
  src/ha/         HaUrl (URL -> WebSocket endpoint), HaProtocol (messages), HaSession (state machine),
                  EntityAttrs, EntityStore (live entities), EntityCatalog (entity list)
  src/input/      QuadratureDecoder, ButtonFsm, Acceleration, SwipeClassifier
  src/ui/         Presenter (number formatting, control views), PageRing (page navigation)
  src/web/        RequestGuard (Host allow-list, CSRF, body limit, Basic auth check)
  src/util/       MathUtil, Sha1, Base64, JsonScan, StdAllocator (STL allocator on an injected allocator)
  src/ws/         WsHandshake, WsFrame, WsReceiver (WebSocket client codec, RFC 6455)
test/native/      host unit tests (Unity), run with `pio test -e native`
tools/            serial_check.py, gen_fonts.sh
docs/             DESIGN-NOTES.md (per-step design decisions and measurements)
licenses/         third-party licence texts (verbatim; see THIRD_PARTY_NOTICES.md), esp-web-tools/ for the flasher
```

## Security

- No secrets (Wi-Fi passwords, Home Assistant tokens) are ever committed. They live only
  in the device's NVS, entered through the device web UI. `.gitignore`
  excludes `*.local.ini`, `secrets*` and `.env*`.
- Secrets are stored separately from the config JSON, are never returned by the API (only
  `*_set` flags) and are never logged (at most `token_set len=N`).
- NVS is not encrypted: anyone with physical access can read the token. Use a dedicated
  non-admin Home Assistant user for the knob's token.
- With an `http://` address the connection to Home Assistant is plain `ws://`, so the token
  can be sniffed on the LAN; use an `https://` address to encrypt it (see TLS). The token is
  only ever sent to the stored address; changing the host or port, turning certificate
  verification off or changing the CA requires entering it again.
- The web UI is plain HTTP on the LAN. The optional web password (Device tab, user `admin`)
  is sent with HTTP Basic auth and can be sniffed on the network; there is no lockout
  after wrong passwords. The setup AP is always WPA2-protected.
- DNS rebinding and CSRF are blocked by the Host allow-list and the JSON + `X-HAKnob`
  header requirement (see Web server and API). The UI sets text with `textContent` only.
