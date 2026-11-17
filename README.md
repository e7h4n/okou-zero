# Okou Zero

A native, keyboard-first Okou client for the M5Stack Cardputer Zero.

**Status: design and API compatibility review. No application binary or CI package is available yet.**

## First-version requirements

1. Sign in with Clerk's OAuth Device Authorization Grant.
2. Load chat threads and projects, establish an Ably connection, and update chat indicators.
3. Open a conversation and communicate with Okou using typed text or voice input.
4. Build installable ARM64 Debian packages in GitHub Actions. Use the physical device for acceptance testing, not compilation.
5. Run automated tests on public CI without production accounts, tokens, or private conversations.

## Proposed implementation

- Native C++17/LVGL interface, rendering at 320×170 to the device framebuffer.
- Desktop simulator for UI development and automated interaction tests.
- Separate authentication, HTTP, realtime synchronization, chat state, audio, and platform adapters.
- Runtime credentials stored only on the device; no client secret embedded in the source or package.
- Public-client device authorization, not a webview login or a replacement CLI personal access token.

These are proposals, not a claim that the features are implemented. See [the compatibility review](docs/design-review.md) for known backend dependencies and pending decisions.

## References

- [Okou iOS client](https://github.com/okou-ai/okou/tree/main/ios)
- [Cardputer Zero application template](https://github.com/CardputerZero/Template)
- [Cardputer Zero launcher and hardware SDK](https://github.com/CardputerZero/launcher)
- [Clerk Device Authorization Grant](https://clerk.com/docs/guides/configure/auth-strategies/oauth/device-authorization-grant)

Licensing is pending an explicit decision. No upstream application implementation has been copied into this repository.
