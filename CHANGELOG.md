# Changelog

## [0.4.0](https://github.com/e7h4n/okou-zero/compare/v0.3.0...v0.4.0) (2026-11-27)


### Features

* add native Cardputer Zero client and ARM64 CI packages ([bae7fa2](https://github.com/e7h4n/okou-zero/commit/bae7fa2524088068295fe0baa04238a5d1b553bb))
* add push-to-talk in chat ([6671711](https://github.com/e7h4n/okou-zero/commit/6671711bfca753e2c3be1ca1de443940d406b935))
* add signed Ctrl+Alt+U updates to native client ([c8171ab](https://github.com/e7h4n/okou-zero/commit/c8171ab61127a23d0ccfc911bf1651a5472e7718))
* bootstrap LVGL Hello World with native ARM64 CI packages ([11fc975](https://github.com/e7h4n/okou-zero/commit/11fc975ec3f8eec6a2a191f4be0ce5d17fa3a59f))
* bundle public OAuth client for companion preview ([bf0c828](https://github.com/e7h4n/okou-zero/commit/bf0c8280102d66e2e7dda55d9b73482dfa1dfc8a))
* bundle public OAuth client in isolated staging builds ([40e0f80](https://github.com/e7h4n/okou-zero/commit/40e0f808f2f14776721197f58e1d9676ebdfede0))
* implement native Cardputer Zero client and CI packages ([ba1aa48](https://github.com/e7h4n/okou-zero/commit/ba1aa480e8b3ec25fead3c21a65ea1ca1e8b9a4d))
* improve chat presentation and show battery status ([8bb314f](https://github.com/e7h4n/okou-zero/commit/8bb314f69fe27dca0b2a2c4be1e3d16d0d521ac5))
* make Cardputer chat voice-only ([7311648](https://github.com/e7h4n/okou-zero/commit/731164897ae26d1017e4280edc50d3f7119ea006))
* make Cardputer chat voice-only ([c3714e4](https://github.com/e7h4n/okou-zero/commit/c3714e4a2a0bd1a5b546282ce702a1df83b68a3f))
* open one persistent Cardputer chat ([5ab99aa](https://github.com/e7h4n/okou-zero/commit/5ab99aaf3ad6c45f5f25ed3c52d95ff562e01ac5))
* open one persistent Cardputer chat ([76a911b](https://github.com/e7h4n/okou-zero/commit/76a911b7b06b0e7e04e704bed116c297742c2b1e))
* play visible voice messages with Space ([d3a0666](https://github.com/e7h4n/okou-zero/commit/d3a066610f644aae9009101253ac6dc255f3470d))
* play visible voice messages with Space ([b24ecbe](https://github.com/e7h4n/okou-zero/commit/b24ecbea028def5874ff27795af9e27162beca84))
* send recordings as voice messages ([efa99bb](https://github.com/e7h4n/okou-zero/commit/efa99bb8fe1929640603627938403e8654a181c8))
* send recordings as voice messages ([bc6f88a](https://github.com/e7h4n/okou-zero/commit/bc6f88af00171135418920c0a850d2bc745210c6))
* skip recordings shorter than 1 second ([#35](https://github.com/e7h4n/okou-zero/issues/35)) ([ea663f6](https://github.com/e7h4n/okou-zero/commit/ea663f6a301d8f9f63658cea4ab28b63e82ddcda))
* use MCP for device chat, realtime and voice ([e3ba622](https://github.com/e7h4n/okou-zero/commit/e3ba622f282e341b74ad6aac23f97f30af4f844d))
* use MCP for device chat, realtime and voice ([46413ea](https://github.com/e7h4n/okou-zero/commit/46413ea92b9f2a1758e8c64a05118f21121f89c7))
* use one standard OAuth package targeting companion preview ([57e226f](https://github.com/e7h4n/okou-zero/commit/57e226f874b4dd999d67627fa23ec2d21646f873))


### Bug Fixes

* add sanitized client failure diagnostics ([277632b](https://github.com/e7h4n/okou-zero/commit/277632b5861f7482efaa1e69909a6f9390f016e1))
* brand app as okou with official character avatar ([59a2ce3](https://github.com/e7h4n/okou-zero/commit/59a2ce36ee343de2d8b90d3ca6ff55f190f1ed1c))
* capitalize Okou app display name ([85d2075](https://github.com/e7h4n/okou-zero/commit/85d20758f6a252a23ed89e51f0ec6f665e4089ea))
* fence submissions and recover scoped voice drafts safely ([90d4a7d](https://github.com/e7h4n/okou-zero/commit/90d4a7d6f2c088cca1889e89f368bfb67f1ec99d))
* inspect protected sudo policy with privilege in CI ([0af3286](https://github.com/e7h4n/okou-zero/commit/0af328652ccca8f91181078a9d06de2552935560))
* keep voice send results visible in chat ([402163f](https://github.com/e7h4n/okou-zero/commit/402163f2fcc7099fb31382c8e739aaafd1e16aa1))
* match the current chat send contract ([34f91f7](https://github.com/e7h4n/okou-zero/commit/34f91f7e6127b721380ce8e6db5c13d2f96ee7f1))
* preserve current branding and prevent draft edits during submission ([b604911](https://github.com/e7h4n/okou-zero/commit/b6049110c52b169135de0ccb35c9eba405e31fe7))
* preserve non-spinning assertions and framebuffer allocation regression ([6f6e53d](https://github.com/e7h4n/okou-zero/commit/6f6e53d914ebbb6ae19b8f6c816bf2e8bd21cf09))
* prevent framebuffer allocation assertion from freezing launcher ([da13011](https://github.com/e7h4n/okou-zero/commit/da130115550c815799e094a5cd9c55cfb58151e7))
* require fresh authorization after uncertain token rotation ([6a06c37](https://github.com/e7h4n/okou-zero/commit/6a06c3785fe16d2ab6b3eb59a3e7114c22127fdf))
* restore cardputer chat and realtime ([bce04e1](https://github.com/e7h4n/okou-zero/commit/bce04e11ebdb10f92431bc021eca01a7c9241903))
* retain approved launcher name and official avatar packaging ([141de31](https://github.com/e7h4n/okou-zero/commit/141de3105b78a518aae18bc90c4c908d010580d5))
* shorten rejected push-to-talk segments ([0b6454a](https://github.com/e7h4n/okou-zero/commit/0b6454af142a998142ebdcc06169ee900f4a007a))
* target production API for signed 0.2.5 release ([2049a02](https://github.com/e7h4n/okou-zero/commit/2049a02a1e67d917a39195aa627c9f26041670e8))

## [0.3.0](https://github.com/e7h4n/okou-zero/compare/v0.2.34...v0.3.0) (2026-11-27)


### Features

* skip recordings shorter than 1 second ([#35](https://github.com/e7h4n/okou-zero/issues/35)) ([ea663f6](https://github.com/e7h4n/okou-zero/commit/ea663f6a301d8f9f63658cea4ab28b63e82ddcda))
