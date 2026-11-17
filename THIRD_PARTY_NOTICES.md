# Third-party notices

This independently written client is MIT licensed (see LICENSE).

The build statically links LVGL 9.5.0, pinned to commit
`85aa60d18b3d5e5588d7b247abf90198f07c8a63` from https://github.com/lvgl/lvgl.
LVGL is MIT licensed; its full license is installed as LVGL-LICENSE.txt.
LVGL's bundled Montserrat font is distributed under the SIL Open Font License 1.1;
see LVGL's `scripts/built_in_font/font_license/Montserrat/OFL.txt`
(installed in the package as Montserrat-OFL.txt).

No Okou BSL implementation or Cardputer launcher implementation is copied here.
## Okou avatar (brand asset, not MIT-licensed code)

`assets/okou-avatar-source.png` is the official Okou ready-state avatar, used at
the brand owner's explicit request. Source: `ios/Widgets/Assets.xcassets/Avatar-ready.imageset/Avatar-ready@3x.png`
in https://github.com/okou-ai/okou at commit
`72ef73aec992722ecd4b4173a02113a3e4f48327`.
Original SHA-256: `abe5cab23c7d4d7a5cbbc6a27643161fb3ef6b27cafab82f3b81f1cadf71062d`.
The 100px launcher and 96px desktop icons are proportional resizes only, without
redrawing, recoloring or cropping. This brand artwork is excluded from this
repository's MIT code license; no general license or trademark rights to reuse
Okou brand assets are granted. No BSL application code was copied.
