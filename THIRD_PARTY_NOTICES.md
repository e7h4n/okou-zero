# Third-party notices

The application source is MIT-licensed. Dependencies retain their own licenses.
No BSL application implementation is copied from the Okou reference repository.
The official avatar is retained from the repository's approved branding baseline;
its separate brand-asset terms below are not covered by the MIT code license.

## MD4C 0.5.2

The Markdown parser and entity table are MIT licensed, copyright © 2016–2024 Martin Mitáš.
The package includes the upstream license in `MD4C-LICENSE.md`.

## LVGL 9.5.0

Source: https://github.com/lvgl/lvgl/tree/85aa60d18b3d5e5588d7b247abf90198f07c8a63

MIT licence
Copyright (c) 2025 LVGL Kft

Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files (the “Software”), to deal in the Software without restriction, including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED “AS IS”, WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

## IXWebSocket 11.4.6

Source: https://github.com/machinezone/IXWebSocket/tree/2efe037c9cc96fd536774f17bdb5215161ee5087

Copyright (c) 2018 Machine Zone, Inc. All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are
met:

1. Redistributions of source code must retain the above copyright
   notice, this list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright
   notice, this list of conditions and the following disclaimer in the
   documentation and/or other materials provided with the
   distribution.

3. Neither the name of the copyright holder nor the names of its
   contributors may be used to endorse or promote products derived
   from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
"AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

## nlohmann/json

System header dependency, MIT license. Version is recorded by the CI build's
system package inventory.

Copyright (c) 2013-2023 Niels Lohmann
Copyright (c) 2009 Florian Loitsch
Copyright (c) 2008-2009 Bjoern Hoehrmann

Permission is hereby granted, free of charge, to any person obtaining a copy of
this software and associated documentation files (the "Software"), to deal in
the Software without restriction, including without limitation the rights to
use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
of the Software, and to permit persons to whom the Software is furnished to
do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

## Built-in Montserrat font

LVGL's built-in Montserrat font is licensed under the SIL Open Font License 1.1.
The complete upstream notice is installed as `/usr/share/doc/okou-zero/Montserrat-OFL.txt`.
Source: LVGL `scripts/built_in_font/font_license/Montserrat/OFL.txt` at the pinned commit.

## Okou avatar (brand asset, not MIT-licensed code)

The existing approved source asset `assets/okou-avatar-source.png` is the official
Okou ready-state avatar. Source: `ios/Widgets/Assets.xcassets/Avatar-ready.imageset/Avatar-ready@3x.png`
in https://github.com/okou-ai/okou at commit `72ef73aec992722ecd4b4173a02113a3e4f48327`.
Original SHA-256: `abe5cab23c7d4d7a5cbbc6a27643161fb3ef6b27cafab82f3b81f1cadf71062d`.
The launcher and desktop icons use the orange logomark from `ios/Assets/AppIcon.svg`
with the dark background from `OkouColor.generated.swift` in the same upstream
repository at commit `a0affd52f4f350d51d857d46772a01ff058ec439`.
These brand assets are excluded from the repository's
MIT code license; no general license or trademark rights to reuse Okou brand
assets are granted. No BSL application implementation is copied.

## Dynamically linked system dependencies

libcurl, OpenSSL, zlib, ALSA and FreeType are provided by the target system and
are not copied into the package. SDL2 is used only by the desktop simulator.
Their license notices are available in the corresponding distribution packages
under `/usr/share/doc`. The device font is supplied by `fonts-noto-cjk`, not
bundled into this repository or package. Test TLS certificates are generated
at test runtime and never installed with the application.

## Pocket Orbit fonts

Geist by the Geist Project Authors and Silkscreen by the Silkscreen Project Authors
are distributed under the SIL Open Font License 1.1. The device uses two static
instances of the Geist Latin variable font and the regular Silkscreen face.
Their full notices are installed in `/usr/share/doc/okou-zero/`.
