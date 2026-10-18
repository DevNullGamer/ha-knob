# Third-party notices

HA Knob itself is licensed under the MIT licence (see `LICENSE`). The firmware, the release
images and the web flasher contain the third-party components below. Versions are the ones
pinned in `platformio.ini`; licences were checked against the library sources downloaded by
PlatformIO (`.pio/libdeps/`), the platform packages (`~/.platformio/packages/`), the upstream
repositories at the exact tags or commits named below, and the linker map of the release
build (`.pio/build/t-encoder-pro/firmware.map`: only components with code or data in
`firmware.elf` are listed).

The full licence texts are in [`licenses/`](licenses/) (verbatim copies, except `FreeBSD-net80211-BSD-2-Clause.txt` (standard wording, see "Licence files"); the source of each
file is listed in "Licence files" at the end). Releases and the flasher site ship `LICENSE`,
this file and the whole `licenses/` directory.

## Files not covered by LICENSE

| Path | Licence | Why |
|---|---|---|
| `src/ui/fonts/*.c` | SIL OFL 1.1 (font data); Font Awesome icon designs CC BY 4.0 | generated from Montserrat and Font Awesome Free, see "Fonts" |
| `include/lv_conf.h` | MIT, Copyright (c) 2021 LVGL Kft (`licenses/LVGL-LICENCE.txt`) | derived from LVGL 9.2.2 `lv_conf_template.h` |
| `licenses/*` | each file's own terms | third-party licence texts |

## Summary: firmware and release images

| Component | Version | Licence | Licence text | Used for |
|---|---|---|---|---|
| LVGL | 9.2.2 | MIT | `LVGL-LICENCE.txt`, `lv_sprintf_builtin-notice.txt`, `lv_math-notice.txt` | UI toolkit |
| SensorLib (lewisxhe) | 0.3.4 | MIT | `SensorLib-LICENSE.txt` | CST816 touch driver |
| ArduinoJson | 7.4.3 | MIT | `ArduinoJson-LICENSE.txt` | JSON |
| GFX Library for Arduino (Arduino_GFX) | 1.5.0 | BSD (Adafruit-derived) | `Arduino_GFX-license.txt` | AMOLED display driver |
| AsyncTCP (ESP32Async) | 3.5.0 | LGPL-3.0-or-later | `LGPL-3.0.txt`, `GPL-3.0.txt` | web server transport |
| ESPAsyncWebServer (ESP32Async) | 3.12.1 | LGPL-3.0-or-later | `LGPL-3.0.txt`, `GPL-3.0.txt` | web server |
| arduino-esp32 core (PlatformIO `framework-arduinoespressif32`) | 2.0.17 (package 3.20017.241212) | LGPL-2.1-or-later as a whole; individual files Apache-2.0 / MIT | `LGPL-2.1.txt`, `Apache-2.0.txt`, `ESPmDNS-notice.txt` | Arduino framework, Wi-Fi, LittleFS, Preferences, mDNS, DNS server |
| ESP-IDF, as precompiled into the arduino-esp32 libraries, and the second-stage bootloader | v4.4.7 (38eeba2) | Apache-2.0, with bundled components below | `Apache-2.0.txt`, `ESP-IDF-COPYRIGHT.rst` | SDK, RTOS, TCP/IP, TLS, Wi-Fi, flash, bootloader |
| ESP-IDF bundled components (lwIP, FreeRTOS, mbedTLS, wpa_supplicant, TLSF, cJSON, littlefs, ...) | see below | BSD-3 / BSD-2 / MIT / Apache-2.0 / CC0 | see below | |
| Mozilla CA root store (TLS certificate bundle) | 2022-04-26 | MPL-2.0 | `MPL-2.0.txt` | `https` trust store |
| newlib C library (toolchain `libc.a`, `libm.a`) | toolchain 8.4.0+2021r2-patch5 | BSD-family, see file | `newlib-COPYING.NEWLIB.txt` | C library |
| Montserrat Medium (text glyphs of `src/ui/fonts/`) | 7.200 (as shipped in LVGL 9.2.2) | SIL OFL 1.1 | inline below | UI font |
| Font Awesome Free (symbol glyphs of `src/ui/fonts/`) | 5.9.0 (as shipped in LVGL 9.2.2) | SIL OFL 1.1 (font); CC BY 4.0 (icon designs) | inline below | UI symbols |

## Summary: web flasher (GitHub Pages only)

| Component | Version | Licence | Licence text (`licenses/esp-web-tools/`) |
|---|---|---|---|
| ESP Web Tools (esphome) | 10.4.0 | Apache-2.0 | `esp-web-tools-LICENSE.txt` |
| lit, lit-html, lit-element, @lit/reactive-element (Google) | 3.3.3, 3.3.0, 4.2.0, 2.1.0 | BSD-3-Clause | `lit-LICENSE.txt`, `lit-html-LICENSE.txt`, `lit-element-LICENSE.txt`, `lit-reactive-element-LICENSE.txt` |
| @material/web (Google) | 2.4.1 | Apache-2.0 | `material-web-LICENSE.txt` |
| esptool-js (Espressif) | 0.6.0 | Apache-2.0 | `esptool-js-LICENSE.txt` |
| esptool flasher stub images (inside esptool-js) | esptool-legacy-flasher-stub | GPL-2.0-or-later | `../GPL-2.0.txt` |
| pako | 2.2.0 | MIT and Zlib | `pako-LICENSE.txt`, `pako-zlib-notice.txt` |
| improv-wifi-serial-sdk | 2.8.0 | Apache-2.0 | `improv-wifi-serial-sdk-LICENSE.txt` |
| tslib (Microsoft) | 2.8.1 | 0BSD | `tslib-LICENSE.txt` |
| atob-lite | 2.0.0 | MIT | `atob-lite-LICENSE.md` |

Build-only and not part of any released file: the PlatformIO `espressif32` platform 6.12.0
(Apache-2.0), the compiler toolchain, esptool, Unity (host unit tests) and the host test tools.

## MIT components

LVGL, SensorLib and ArduinoJson are MIT. Their licence files are copied verbatim to
`licenses/` and carry these copyright lines:
- LVGL: Copyright (c) 2021 LVGL Kft
- SensorLib: Copyright (c) 2022 lewis he (the linked driver files also name 2023 and 2024)
- ArduinoJson: Copyright (c) 2014-2026, Benoit Blanchon

Two LVGL source files that are linked into the firmware carry their own MIT notice (reproduced
verbatim from the file headers):
- `src/stdlib/builtin/lv_sprintf_builtin.c` (linked because `LV_USE_STDLIB_SPRINTF =
  LV_STDLIB_BUILTIN`): (c) Marco Paland, 2014-2019 PALANDesign Hannover, Germany
  (https://github.com/mpaland/printf); `licenses/lv_sprintf_builtin-notice.txt`.
- `src/misc/lv_math.c` (cubic-bezier): Copyright (c) 2014 Gaëtan Renaudeau;
  `licenses/lv_math-notice.txt`.

## Arduino_GFX (BSD)

The 1.5.0 package downloaded by PlatformIO ships no licence file. The licence is the file
`license.txt` at tag `v1.5.0` (commit 62975c4) of https://github.com/moononournation/Arduino_GFX
(GitHub reports SPDX `NOASSERTION`): a BSD licence, "Copyright (c) 2012 Adafruit Industries"
(the library started as a rewrite of Adafruit_GFX). Verbatim copy:
`licenses/Arduino_GFX-license.txt`.

## LGPL components

**AsyncTCP 3.5.0 and ESPAsyncWebServer 3.12.1** are licensed under the GNU Lesser General
Public License v3.0 or later (`SPDX-License-Identifier: LGPL-3.0-or-later` in each linked
source file; "Copyright 2016-2026 Hristo Gochkov, Mathieu Carbou, Emil Muratov, Will Miles").
LGPL v3 text: `licenses/LGPL-3.0.txt`; it supplements the GNU GPL v3:
`licenses/GPL-3.0.txt`.

**arduino-esp32 2.0.17** is licensed under the GNU Lesser General Public License v2.1 or
later as a whole (`licenses/LGPL-2.1.txt`). Individual files carry other licences; of the
files linked into the firmware:
- Apache-2.0 (Copyright Espressif Systems (Shanghai) Co. Ltd.): `cores/esp32/HWCDC.cpp`,
  the `cores/esp32/esp32-hal-*.c` files, `libraries/LittleFS/src/LittleFS.cpp`,
  `libraries/Preferences/src/Preferences.cpp`, `libraries/FS/src/vfs_api.cpp`,
  `libraries/WiFiClientSecure/src/esp_crt_bundle.c`, and
  `libraries/WiFiClientSecure/src/ssl_client.cpp` (also "Copyright (C) 2006-2015, ARM
  Limited"). Text: `licenses/Apache-2.0.txt`.
- MIT: `libraries/ESPmDNS/src/ESPmDNS.cpp`, "Copyright (c) 2013 Tony DiCola" (with
  ESP8266 port (c) 2015 Ivan Grokhotkov, MDNS-SD support 2015 Hristo Gochkov, extended
  MDNS-SD support 2016 Lars Englund). Its notice is reproduced verbatim in
  `licenses/ESPmDNS-notice.txt`.
- All other linked files (e.g. `WString.cpp`, `Print.cpp`, `Stream.cpp`, `IPAddress.cpp`,
  `cbuf.cpp`, `MD5Builder.cpp`, the `WiFi`, `FS`, `Wire` and `WiFiClientSecure.cpp` sources)
  are LGPL-2.1-or-later.

The ESP-IDF libraries precompiled into the PlatformIO package (`tools/sdk/esp32s3/lib/*.a`) are
not LGPL; they are covered by the ESP-IDF section below.

### Source code

The corresponding source of the LGPL components, at the exact versions used:
- AsyncTCP 3.5.0: https://github.com/ESP32Async/AsyncTCP/tree/v3.5.0 (commit 5bd9e2d)
- ESPAsyncWebServer 3.12.1: https://github.com/ESP32Async/ESPAsyncWebServer/tree/v3.12.1
  (commit c47917d)
- arduino-esp32 2.0.17: https://github.com/espressif/arduino-esp32/tree/2.0.17 (commit
  5e19e08); PlatformIO package `framework-arduinoespressif32` 3.20017.241212
  (sha dcc1105b), which contains the same sources plus the precompiled ESP-IDF v4.4.7
  libraries.

The complete source of HA Knob (the rest of the combined work) is this repository; all
libraries are used unmodified.

### Rebuilding with a modified library

Anyone can rebuild the firmware with a modified version of an LGPL component and flash it;
no term of this project restricts that, and the device has no secure boot or flash
encryption that would prevent it.
1. Clone this repository at the release tag (`v<version>`).
2. Put the modified library anywhere on disk and point PlatformIO at it in `platformio.ini`:
   - AsyncTCP / ESPAsyncWebServer: replace the `lib_deps` entry (for example
     `ESP32Async/AsyncTCP@3.5.0`) with `symlink://<path to the modified copy>` (or
     `file://<path>`).
   - arduino-esp32 core: add
     `platform_packages = framework-arduinoespressif32 @ symlink://<path to the modified core>`
     (or `file://<path>`) to `[env:t-encoder-pro]`.
3. Build with `pio run -e t-encoder-pro` (and `pio run -e t-encoder-pro -t buildfs` for the
   web UI image) and flash with `pio run -e t-encoder-pro -t upload` (and `-t uploadfs`), as
   described in the README.

## ESP-IDF and its bundled components

The arduino-esp32 package links precompiled ESP-IDF v4.4.7 libraries (commit 38eeba2,
`tools/sdk/versions.txt`) and the precompiled ESP-IDF second-stage bootloader
(`tools/sdk/esp32s3/bin/bootloader_qio_80m.elf`, released as `bootloader.bin`). ESP-IDF's
original code is "Copyright (C) 2015-2022 Espressif Systems" and licensed under the Apache
License 2.0 (`licenses/Apache-2.0.txt`, the ESP-IDF v4.4.7 `LICENSE`). ESP-IDF's own
third-party summary is copied verbatim to `licenses/ESP-IDF-COPYRIGHT.rst`
(`docs/en/COPYRIGHT.rst` at v4.4.7). Sources: https://github.com/espressif/esp-idf/tree/v4.4.7.

Components with code in `firmware.elf` that need their own attribution:

| Component (archive in the map) | Copyright | Licence | Text in `licenses/` |
|---|---|---|---|
| lwIP (`liblwip.a`; espressif/esp-lwip a45be9e) | Copyright (c) 2001, 2002 Swedish Institute of Computer Science | BSD-3-Clause | `lwIP-COPYING.txt` |
| FreeRTOS kernel V10.4.3, ESP-IDF port (`libfreertos.a`) | Copyright (C) 2020 Amazon.com, Inc. or its affiliates | MIT | `FreeRTOS-LICENSE.md` (the copyright line is from `tasks.c`) |
| Mbed TLS 2.28.7 (`libmbedcrypto.a`, `libmbedtls_2.a`, `libmbedx509.a`; espressif/mbedtls 2b8e772) | Copyright (C) 2006-2018 ARM Limited; Copyright The Mbed TLS Contributors | Apache-2.0 (dual Apache-2.0 / GPL-2.0-or-later; used under Apache-2.0) | `Apache-2.0.txt` |
| wpa_supplicant, ESP-IDF fork (`libwpa_supplicant.a`) | Copyright (c) 2002-2019, Jouni Malinen and contributors | BSD-3-Clause (advertising clause removed) | `wpa_supplicant-README.txt` |
| fast PBKDF2 inside wpa_supplicant (`fastpbkdf2.c`) | Copyright (c) 2015 Joseph Birr-Pixton | CC0 1.0 (public domain dedication) | none required |
| TLSF heap allocator (`libheap.a`, `heap_tlsf.c`) | Copyright (c) 2006-2016, Matthew Conte | BSD-3-Clause | `heap_tlsf-notice.txt` |
| cJSON (`libjson.a`; only `cJSON_Delete` is linked; DaveGamble/cJSON 87d8f09) | Copyright (c) 2009-2017 Dave Gamble and cJSON contributors | MIT | `cJSON-LICENSE.txt` |
| UBSAN runtime (`libesp_system.a`, `ubsan.c`; only the `__ubsan_include` stub is linked) | Copyright (c) 2016, Linaro Limited; modified by Jiří Zárevúcky | BSD-2-Clause | `ubsan-notice.txt` |
| Xtensa HAL and headers (`libxt_hal.a`, `libxtensa.a`, `libfreertos.a` vectors) | Copyright (c) 2003, 2006, 2010 Tensilica Inc; Copyright (c) 2006-2015 Cadence Design Systems Inc | MIT | `xtensa-notice.txt`, `ESP-IDF-COPYRIGHT.rst` ("Xtensa libhal MIT License") |
| esp_littlefs (`libesp_littlefs.a`; joltwallet/esp_littlefs 41873c2) | Copyright 2020 Brian Pugh | MIT | `esp_littlefs-LICENSE.txt` |
| littlefs (`lfs.c` in `libesp_littlefs.a`; littlefs-project/littlefs f53a0cc) | Copyright (c) 2022, The littlefs authors; Copyright (c) 2017, Arm Limited | BSD-3-Clause | `littlefs-LICENSE.md` |
| Wi-Fi, PHY and coexistence binary libraries (`libnet80211.a`, `libpp.a`, `libcore.a` from the SDK, `libsmartconfig.a`, `libcoexist.a`, `libphy.a`; espressif/esp32-wifi-lib f2aae4d, espressif/esp-phy-lib dcfdccf) | Espressif Systems | Apache-2.0 | `Apache-2.0.txt` |
| FreeBSD net80211 code inside `libnet80211.a` (per ESP-IDF COPYRIGHT.rst) | Copyright (c) 2004-2008 Sam Leffler, Errno Consulting | BSD (ESP-IDF COPYRIGHT.rst); BSD-2-Clause per the FreeBSD sys/net80211 sources | `ESP-IDF-COPYRIGHT.rst`; standard FreeBSD `sys/net80211` BSD-2-Clause wording in `FreeBSD-net80211-BSD-2-Clause.txt` (not a verbatim copy of a source header: none is published with the binary) |
| esp_diagnostics (`libesp_diagnostics.a`, from espressif/esp-insights) | Espressif Systems | Apache-2.0 | `Apache-2.0.txt` |
| mDNS, NVS, spi_flash, esp_netif, Wi-Fi provisioning / protocomm glue and the other Espressif components | Espressif Systems | Apache-2.0 | `Apache-2.0.txt` |

Checked and **not** linked (no code or data in `firmware.elf`): protobuf-c (an archive member
is pulled in by `wifi_provisioning`, but all its sections are discarded by `--gc-sections`;
no `protobuf_c` symbol in the ELF), the Bluetooth controller and host stacks (`libbtdm_app.a`,
NimBLE, BLE Mesh; only the 375-byte `bt.c` glue, Apache-2.0, is linked), ESP-MESH, JSMN,
argtable3, linenoise, libcoap, expat, FatFS, libsodium, micro-ecc, nghttp2, SPIFFS,
TinyCBOR, the SD/MMC driver, asio, ESP-MQTT, cryptoauthlib, qrcode, OpenThread and
freemodbus. The ROM code of the ESP32-S3 is part of the chip, not of the released files.

### TLS certificate bundle (Mozilla CA list)

The firmware embeds ESP-IDF's full certificate bundle (`x509_crt_bundle`, about 62 KB, in
`libmbedtls.a`; `CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_DEFAULT_FULL`), built from
`components/mbedtls/esp_crt_bundle/cacrt_all.pem` of ESP-IDF v4.4.7: "Certificate data from
Mozilla as of: Tue Apr 26 03:12:05 2022 GMT", extracted from Mozilla's `certdata.txt`. That
data is made available under the Mozilla Public License 2.0 (`licenses/MPL-2.0.txt`). Source
Code Form: https://github.com/espressif/esp-idf/blob/v4.4.7/components/mbedtls/esp_crt_bundle/cacrt_all.pem
and Mozilla's `certdata.txt` in the NSS source tree
(as cited by that file: https://hg.mozilla.org/releases/mozilla-release/raw-file/default/security/nss/lib/ckfw/builtins/certdata.txt).

### Toolchain libraries

The C library comes from newlib in the Xtensa toolchain (`toolchain-xtensa-esp32s3`
8.4.0+2021r2-patch5, `libc.a` and `libm.a`, plus ESP-IDF's `newlib` glue). Newlib consists
of files under several BSD-style and similar licences; the toolchain's `COPYING.NEWLIB` is
copied verbatim to `licenses/newlib-COPYING.NEWLIB.txt`. `libgcc` and `libstdc++` (GCC 8.4.0)
are covered by the GCC Runtime Library Exception, which places no conditions on compiled
programs.

## Fonts

The generated files `src/ui/fonts/ui_font_20.c`, `ui_font_28.c` and `ui_font_56.c` are
bitmap subsets produced by `tools/gen_fonts.sh` (lv_font_conv) from two font files that ship
with LVGL 9.2.2 in `scripts/built_in_font/`:

- `Montserrat-Medium.ttf`, Version 7.200: "Copyright 2011 The Montserrat Project Authors
  (https://github.com/JulietaUla/Montserrat)", licensed under the SIL Open Font License 1.1.
- `FontAwesome5-Solid+Brands+Regular.woff`, Font Awesome Free 5.9.0: "Copyright (c) Font
  Awesome" (https://fontawesome.com). The font files are licensed under the SIL Open Font
  License 1.1; the icon designs under Creative Commons Attribution 4.0 International
  (https://creativecommons.org/licenses/by/4.0/). Attribution: icons by Font Awesome
  (https://fontawesome.com), CC BY 4.0; no endorsement implied; the icons were converted
  to bitmap glyphs. The brand icons used (USB U+F287, Bluetooth U+F293) are trademarks of
  their respective owners.

The generated C files are Modified Versions of these fonts under the OFL and are distributed
under the OFL 1.1 as to the font data. Neither font declares a Reserved Font Name (checked:
the copyright statements in the font files' name tables, quoted above, name none), so the
OFL's Reserved Font Name condition does not restrict the generated files. The OFL text
follows.

```
-----------------------------------------------------------
SIL OPEN FONT LICENSE Version 1.1 - 26 February 2007
-----------------------------------------------------------

PREAMBLE
The goals of the Open Font License (OFL) are to stimulate worldwide
development of collaborative font projects, to support the font creation
efforts of academic and linguistic communities, and to provide a free and
open framework in which fonts may be shared and improved in partnership
with others.

The OFL allows the licensed fonts to be used, studied, modified and
redistributed freely as long as they are not sold by themselves. The
fonts, including any derivative works, can be bundled, embedded, 
redistributed and/or sold with any software provided that any reserved
names are not used by derivative works. The fonts and derivatives,
however, cannot be released under any other type of license. The
requirement for fonts to remain under this license does not apply
to any document created using the fonts or their derivatives.

DEFINITIONS
"Font Software" refers to the set of files released by the Copyright
Holder(s) under this license and clearly marked as such. This may
include source files, build scripts and documentation.

"Reserved Font Name" refers to any names specified as such after the
copyright statement(s).

"Original Version" refers to the collection of Font Software components as
distributed by the Copyright Holder(s).

"Modified Version" refers to any derivative made by adding to, deleting,
or substituting -- in part or in whole -- any of the components of the
Original Version, by changing formats or by porting the Font Software to a
new environment.

"Author" refers to any designer, engineer, programmer, technical
writer or other person who contributed to the Font Software.

PERMISSION & CONDITIONS
Permission is hereby granted, free of charge, to any person obtaining
a copy of the Font Software, to use, study, copy, merge, embed, modify,
redistribute, and sell modified and unmodified copies of the Font
Software, subject to the following conditions:

1) Neither the Font Software nor any of its individual components,
in Original or Modified Versions, may be sold by itself.

2) Original or Modified Versions of the Font Software may be bundled,
redistributed and/or sold with any software, provided that each copy
contains the above copyright notice and this license. These can be
included either as stand-alone text files, human-readable headers or
in the appropriate machine-readable metadata fields within text or
binary files as long as those fields can be easily viewed by the user.

3) No Modified Version of the Font Software may use the Reserved Font
Name(s) unless explicit written permission is granted by the corresponding
Copyright Holder. This restriction only applies to the primary font name as
presented to the users.

4) The name(s) of the Copyright Holder(s) or the Author(s) of the Font
Software shall not be used to promote, endorse or advertise any
Modified Version, except to acknowledge the contribution(s) of the
Copyright Holder(s) and the Author(s) or with their explicit written
permission.

5) The Font Software, modified or unmodified, in part or in whole,
must be distributed entirely under this license, and must not be
distributed under any other license. The requirement for fonts to
remain under this license does not apply to any document created
using the Font Software.

TERMINATION
This license becomes null and void if any of the above conditions are
not met.

DISCLAIMER
THE FONT SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO ANY WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT
OF COPYRIGHT, PATENT, TRADEMARK, OR OTHER RIGHT. IN NO EVENT SHALL THE
COPYRIGHT HOLDER BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
INCLUDING ANY GENERAL, SPECIAL, INDIRECT, INCIDENTAL, OR CONSEQUENTIAL
DAMAGES, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
FROM, OUT OF THE USE OR INABILITY TO USE THE FONT SOFTWARE OR FROM
OTHER DEALINGS IN THE FONT SOFTWARE.
```

## Web flasher (ESP Web Tools)

The GitHub Pages flasher serves ESP Web Tools 10.4.0 (https://github.com/esphome/esp-web-tools,
tag 10.4.0, commit fab2ba4) unmodified: the `dist/web` directory of the npm package
`esp-web-tools-10.4.0.tgz` (sha512 `3pwkeFFm5Fj7UQo8SJNYK5RXrtNCpq6X9QoI6bMT4GBZWgrJqjn0YvM9ihG74BtMoSFYXfmDtkehuxe50PTMPQ==`).
It is Apache-2.0, "ESPHome maintainers". Its minified bundle contains the packages listed in
the second summary table; the versions are those of the `package-lock.json` at the 10.4.0
tag, confirmed against the version strings in the bundle (lit-html 3.3.0, lit-element 4.2.0,
reactive-element 2.1.0) and the code it contains. Copyright lines:
- lit, lit-html, lit-element, @lit/reactive-element: Copyright (c) 2017 Google LLC
- @material/web: Copyright 2018-2023 Google, Inc.
- esptool-js: Copyright 2019 Espressif Systems (Shanghai) CO LTD
- pako: Copyright (C) 2014-2017 by Vitaly Puzrin and Andrei Tuputcyn; zlib-derived code
  (C) 1995-2013 Jean-loup Gailly and Mark Adler, (C) 2014-2017 Vitaly Puzrin and Andrey
  Tupitsin (zlib licence, `pako-zlib-notice.txt`)
- tslib: Copyright (c) Microsoft Corporation
- atob-lite: MIT (no copyright line in its licence file)

`@lit-labs/ssr-dom-shim` (a dependency of `@lit/reactive-element` for server-side rendering
only) and `@types/trusted-types` (type declarations) are not in the browser bundle.

The flasher stub images (`stub_flasher_*.js`, from esptool-js `lib/targets/stub_flasher/`)
are binaries of the esptool flasher stub: "Copyright 2016 Cesanta Software Limited,
2016-2022 Espressif Systems (Shanghai) Co. Ltd.", licensed under the GNU General Public
License v2.0 or later (`licenses/GPL-2.0.txt`). They are identical to the images in
esptool's `esptool/targets/stub_flasher/1/` (checked against esptool v4.8.1, v4.10.0 and
v4.11.0 for 11 of the 12 images, including ESP32-S3; the ESP32-P4 image was not matched), which esptool documents
as released from https://github.com/espressif/esptool-legacy-flasher-stub (releases v1.3.0 to
v1.11.1), where their source code is available. They are downloaded to the chip's RAM while
flashing and are not part of the HA Knob firmware.

## LilyGo vendor repository

The LilyGo T-Encoder-Pro repository (https://github.com/Xinyuan-LilyGO/T-Encoder-Pro) is
GPL-3.0. This project uses only hardware facts from it (pin numbers, panel quirks). No
files or code were copied from it.

## Licence files

Every file in `licenses/` is a verbatim copy, except `FreeBSD-net80211-BSD-2-Clause.txt` (standard wording), (line endings normalised to LF by git). Sources:

| File | Source |
|---|---|
| `Apache-2.0.txt` | ESP-IDF v4.4.7 `LICENSE` (https://raw.githubusercontent.com/espressif/esp-idf/v4.4.7/LICENSE); identical to esp32-wifi-lib f2aae4d `LICENSE` |
| `GPL-2.0.txt` | toolchain-xtensa-esp32s3 8.4.0+2021r2-patch5 `share/licenses/newlib/COPYING` |
| `GPL-3.0.txt` | toolchain-xtensa-esp32s3 8.4.0+2021r2-patch5 `share/licenses/newlib/COPYING3` (same revision as the LGPL v3 text below) |
| `LGPL-2.1.txt` | toolchain-xtensa-esp32s3 8.4.0+2021r2-patch5 `share/licenses/newlib/COPYING.LIB` (the GNU LGPL v2.1 named by arduino-esp32 2.0.17 `LICENSE.md`) |
| `LGPL-3.0.txt` | AsyncTCP 3.5.0 `LICENSE` (`.pio/libdeps/t-encoder-pro/AsyncTCP/LICENSE`) |
| `MPL-2.0.txt` | https://www.mozilla.org/media/MPL/2.0/index.txt |
| `ESP-IDF-COPYRIGHT.rst` | ESP-IDF v4.4.7 `docs/en/COPYRIGHT.rst` |
| `FreeBSD-net80211-BSD-2-Clause.txt` | Standard FreeBSD `sys/net80211` BSD-2-Clause wording with the Sam Leffler copyright line (not copied from a published file) |
| `newlib-COPYING.NEWLIB.txt` | toolchain-xtensa-esp32s3 8.4.0+2021r2-patch5 `share/licenses/newlib/COPYING.NEWLIB` |
| `LVGL-LICENCE.txt` | LVGL 9.2.2 `LICENCE.txt` (`.pio/libdeps`) |
| `SensorLib-LICENSE.txt` | SensorLib 0.3.4 `LICENSE` (`.pio/libdeps`) |
| `ArduinoJson-LICENSE.txt` | ArduinoJson 7.4.3 `LICENSE.txt` (`.pio/libdeps`) |
| `Arduino_GFX-license.txt` | moononournation/Arduino_GFX tag v1.5.0 `license.txt` |
| `lv_sprintf_builtin-notice.txt` | LVGL 9.2.2 `src/stdlib/builtin/lv_sprintf_builtin.c`, lines 1-23 |
| `lv_math-notice.txt` | LVGL 9.2.2 `src/misc/lv_math.c`, lines 79-108 |
| `ESPmDNS-notice.txt` | framework-arduinoespressif32 3.20017.241212 `libraries/ESPmDNS/src/ESPmDNS.cpp`, lines 1-30 |
| `lwIP-COPYING.txt` | espressif/esp-lwip a45be9e (ESP-IDF v4.4.7 submodule) `COPYING` |
| `FreeRTOS-LICENSE.md` | ESP-IDF v4.4.7 `components/freertos/LICENSE.md` |
| `wpa_supplicant-README.txt` | ESP-IDF v4.4.7 `components/wpa_supplicant/README` |
| `heap_tlsf-notice.txt` | ESP-IDF v4.4.7 `components/heap/heap_tlsf.c`, lines 1-35 |
| `cJSON-LICENSE.txt` | DaveGamble/cJSON 87d8f09 (ESP-IDF v4.4.7 submodule) `LICENSE` |
| `ubsan-notice.txt` | ESP-IDF v4.4.7 `components/esp_system/ubsan.c`, lines 1-25 |
| `xtensa-notice.txt` | ESP-IDF v4.4.7 `components/xtensa/xtensa_intr_asm.S`, lines 1-22 |
| `esp_littlefs-LICENSE.txt` | joltwallet/esp_littlefs 41873c2 (arduino-esp32 `tools/sdk/versions.txt`) `LICENSE` |
| `littlefs-LICENSE.md` | littlefs-project/littlefs f53a0cc (esp_littlefs 41873c2 submodule) `LICENSE.md` |
| `esp-web-tools/esp-web-tools-LICENSE.txt` | npm `esp-web-tools-10.4.0.tgz` `package/LICENSE` |
| `esp-web-tools/lit-LICENSE.txt` | npm `lit-3.3.3.tgz` `package/LICENSE` |
| `esp-web-tools/lit-html-LICENSE.txt` | npm `lit-html-3.3.0.tgz` `package/LICENSE` |
| `esp-web-tools/lit-element-LICENSE.txt` | npm `lit-element-4.2.0.tgz` `package/LICENSE` |
| `esp-web-tools/lit-reactive-element-LICENSE.txt` | npm `@lit/reactive-element-2.1.0.tgz` `package/LICENSE` |
| `esp-web-tools/material-web-LICENSE.txt` | npm `@material/web-2.4.1.tgz` `package/LICENSE` |
| `esp-web-tools/esptool-js-LICENSE.txt` | npm `esptool-js-0.6.0.tgz` `package/LICENSE` |
| `esp-web-tools/improv-wifi-serial-sdk-LICENSE.txt` | npm `improv-wifi-serial-sdk-2.8.0.tgz` `package/LICENSE` |
| `esp-web-tools/tslib-LICENSE.txt` | npm `tslib-2.8.1.tgz` `package/LICENSE.txt` |
| `esp-web-tools/pako-LICENSE.txt` | npm `pako-2.2.0.tgz` `package/LICENSE` |
| `esp-web-tools/pako-zlib-notice.txt` | npm `pako-2.2.0.tgz` `package/lib/zlib/inflate.js`, lines 3-20 |
| `esp-web-tools/atob-lite-LICENSE.md` | npm `atob-lite-2.0.0.tgz` `package/LICENSE.md` |

All npm tarballs were checked against the sha512 integrity of the registry and of the
esp-web-tools 10.4.0 `package-lock.json`.
