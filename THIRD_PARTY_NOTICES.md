# Third-party notices

HA Knob itself is licensed under the MIT licence (see `LICENSE`). The firmware and the
release images link or embed the third-party components below. Versions are the ones pinned
in `platformio.ini`; licences were checked against the library sources downloaded by
PlatformIO (`.pio/libdeps/`) and the platform packages (`~/.platformio/`).

| Component | Version | Licence | Used for |
|---|---|---|---|
| LVGL | 9.2.2 | MIT (Copyright (c) 2021 LVGL Kft) | UI toolkit |
| SensorLib (lewisxhe) | 0.3.4 | MIT (Copyright (c) 2022 lewis he) | CST816 touch driver |
| ArduinoJson | 7.4.3 | MIT (Copyright (c) 2014-2026 Benoit Blanchon) | JSON |
| GFX Library for Arduino (Arduino_GFX) | 1.5.0 | BSD (Adafruit-derived, see below) | AMOLED display driver |
| AsyncTCP (ESP32Async) | 3.5.0 | LGPL-3.0 | web server transport |
| ESPAsyncWebServer (ESP32Async) | 3.12.1 | LGPL-3.0 | web server |
| arduino-esp32 core (PlatformIO `framework-arduinoespressif32`) | 3.20017.241212 (core 2.0.17) | LGPL-2.1-or-later | Arduino framework, Wi-Fi, LittleFS, mDNS libraries |
| ESP-IDF (with mbedTLS and the bootloader), as built into the arduino-esp32 libraries | v4.4.7 | Apache-2.0 | SDK, TLS, second-stage bootloader |
| PlatformIO `espressif32` platform (build scripts only, not shipped) | 6.12.0 | Apache-2.0 | build |
| Montserrat Medium (text glyphs of `src/ui/fonts/`) | as shipped in LVGL 9.2.2 | SIL OFL 1.1 | UI font |
| Font Awesome 5 Free (symbol glyphs of `src/ui/fonts/`) | as shipped in LVGL 9.2.2 | SIL OFL 1.1 (fonts); icon designs CC BY 4.0 | UI symbols |
| ESP Web Tools (web flasher, vendored on GitHub Pages only) | 10.4.0 | Apache-2.0 | browser flasher |

Unity (host unit tests only) and the host test tooling are not part of any released binary.

## MIT components

LVGL, SensorLib and ArduinoJson are distributed under the MIT licence. Each library's
licence text and copyright line are in its source package (`LICENCE.txt` / `LICENSE` /
`LICENSE.txt`) and read:

> Permission is hereby granted, free of charge, to any person obtaining a copy of this
> software and associated documentation files (the "Software"), to deal in the Software
> without restriction, including without limitation the rights to use, copy, modify, merge,
> publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons
> to whom the Software is furnished to do so, subject to the following conditions: the above
> copyright notice and this permission notice shall be included in all copies or substantial
> portions of the Software. THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
> EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
> FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
> COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
> OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR
> THE USE OR OTHER DEALINGS IN THE SOFTWARE.

Copyright lines:
- LVGL: Copyright (c) 2021 LVGL Kft
- SensorLib: Copyright (c) 2022 lewis he
- ArduinoJson: Copyright (c) 2014-2026, Benoit Blanchon

## Arduino_GFX (BSD)

The 1.5.0 package downloaded by PlatformIO ships no licence file. The upstream repository
(https://github.com/moononournation/Arduino_GFX) carries the following licence (the library
started as a rewrite of Adafruit_GFX); that text is reproduced here:

```
Software License Agreement (BSD License)

Copyright (c) 2012 Adafruit Industries.  All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

- Redistributions of source code must retain the above copyright notice,
  this list of conditions and the following disclaimer.
- Redistributions in binary form must reproduce the above copyright notice,
  this list of conditions and the following disclaimer in the documentation
  and/or other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
POSSIBILITY OF SUCH DAMAGE.

```

## LGPL components (AsyncTCP, ESPAsyncWebServer, arduino-esp32)

AsyncTCP and ESPAsyncWebServer are licensed under the GNU Lesser General Public License
v3.0 (full text: `LICENSE` in each library, https://www.gnu.org/licenses/lgpl-3.0.html).
The arduino-esp32 core and its bundled libraries are licensed under the GNU Lesser
General Public License v2.1 or later (https://www.gnu.org/licenses/old-licenses/lgpl-2.1.html).

Source and relinking: the complete corresponding source of HA Knob is this repository, and
the exact library versions are pinned in `platformio.ini`. The firmware is built from
unmodified copies of these libraries; anyone can rebuild and relink it with a modified
version of an LGPL library by editing the pinned version (or placing a modified copy in
`lib/`) and running `pio run -e t-encoder-pro`, then flash the result as described in the
README. No licence term of this project restricts that.

## Apache-2.0 components

ESP-IDF (including mbedTLS and the ESP-IDF bootloader, linked into the arduino-esp32
libraries and into the released `bootloader.bin`) and the PlatformIO `espressif32` platform
are licensed under the Apache License 2.0 (https://www.apache.org/licenses/LICENSE-2.0).
ESP Web Tools (https://github.com/esphome/esp-web-tools, version 10.4.0) is Apache-2.0 and
is served unmodified, together with its licence file, only from the GitHub Pages flasher.

## Fonts

The generated files `src/ui/fonts/ui_font_20.c`, `ui_font_28.c` and `ui_font_56.c` are
bitmap subsets produced by `tools/gen_fonts.sh` (lv_font_conv) from two font files that ship
with LVGL 9.2.2 in `scripts/built_in_font/`:

- `Montserrat-Medium.ttf`: Copyright 2011 The Montserrat Project Authors
  (https://github.com/JulietaUla/Montserrat), licensed under the SIL Open Font License 1.1.
- `FontAwesome5-Solid+Brands+Regular.woff`: Font Awesome Free, Copyright (c) Font Awesome
  (https://fontawesome.com). The font files are licensed under the SIL Open Font License 1.1;
  the icon designs under Creative Commons Attribution 4.0 International
  (https://creativecommons.org/licenses/by/4.0/). Attribution: icons by Font Awesome
  (https://fontawesome.com), CC BY 4.0; no endorsement implied; the icons were converted
  to bitmap glyphs.

The generated C files are Modified Versions / derivatives of these fonts under the OFL
and are distributed under the OFL 1.1 as to the font data; the OFL text follows. The Reserved
Font Name clause applies only if the copyright holders declared one; none is declared in the
notices above.

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

## LilyGo vendor repository

The LilyGo T-Encoder-Pro repository (https://github.com/Xinyuan-LilyGO/T-Encoder-Pro) is
GPL-3.0. This project uses only hardware facts from it (pin numbers, panel quirks). No
files or code were copied from it.
