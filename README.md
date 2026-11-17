# Okou Zero

A native, keyboard-first Okou client for the **M5Stack Cardputer Zero**.
C++17, LVGL, a 320×170 RGB565 framebuffer, and the device's physical keyboard.
Independent implementation, [MIT licensed](LICENSE).

## Status

V1 implementation and automated tests are available on this branch. **Live
account acceptance is pending the companion API change and Clerk public-client
configuration.** Mock tests are not evidence that production login, Ably or voice
has been accepted. See [authentication setup](docs/auth-setup.md).

Implemented surfaces:

- Clerk OAuth device authorization, browser-side workspace selection,
  refresh-token rotation and local logout. No password or client secret on device.
- Project and conversation lists, active/unread indicators and history.
- Chat-only Ably subscription with reconnect, token renewal and gap reconciliation.
- Explicit text submission with durable uncertain-send identity, never blind resend.
- Deliberate microphone recording, transcription into an editable draft,
  then **manual** submission. No hot microphone, automatic sending or spoken reply.
- SDL2 desktop simulator, Linux framebuffer/evdev device backend and CPack `.deb`.
- GitHub Actions desktop tests/screenshots and native ARM64 package/install tests.

The iOS client is a **protocol/behavior reference**, not a source dependency.
No BSL application implementation is copied into this repository.

## Build and test on a development machine

Ubuntu 24.04 (amd64 or arm64):

```sh
sudo apt-get install cmake ninja-build g++ libcurl4-openssl-dev libssl-dev \
  zlib1g-dev nlohmann-json3-dev libasound2-dev libsdl2-dev libfreetype-dev fonts-noto-cjk
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DOKOU_ZERO_DESKTOP=ON
cmake --build build --parallel 2
ctest --test-dir build --output-on-failure
./build/okou-zero --demo
```

Public CI uses no production accounts, secrets, conversations or microphone.
Protocol fixtures run on loopback; UI tests use SDL's dummy video driver. Mock
screenshots intentionally contain demo conversations, never a real account.

Device packages are built by the `ARM64 package and installation` CI job and
published as the `okou-zero-arm64-deb` artifact, with SHA-256 checksums. **Do not
compile on the Cardputer.** No repository automation merges PRs or deploys a device.

## Install a CI-built package

On the device, after downloading and verifying the CI artifact:

```sh
sha256sum -c SHA256SUMS
sudo apt-get install ./okou-zero_0.1.0_arm64.deb
```

The package adds an `Okou` entry in `/usr/share/APPLaunch/applications`. Start it
through the launcher so the launcher releases framebuffer/input ownership. Do
not run a competing foreground GUI over SSH. The app runs as the normal launcher
user, not root, with existing video/input/audio access. No systemd GUI autostart,
network listener or remote-control configuration is installed or changed.

Configure the public OAuth client as described in [auth setup](docs/auth-setup.md).
The app prompts for a missing public client ID; optional settings are documented
in [the example](config/config.example.json). Store local configuration in a 0700
directory with the file mode 0600. No client secret belongs in this file.

## Keyboard

| Key | Action |
| --- | --- |
| Up / down, Enter | Select and open a row; scroll conversation history |
| Tab | Switch between chats and projects |
| Enter in composer | Explicitly submit text |
| Shift+Enter | Insert a newline |
| Backspace | Delete one UTF-8 code point |
| Ctrl+R | Start/finish dictation |
| Esc while recording | Discard recording |
| Esc | Back/menu |
| Ctrl+W | Workspace/sign-out menu |
| Ctrl+L | Refresh authoritative state |
| Ctrl+Q | Exit to launcher |

Workspace switching signs out and starts a new device grant; select the new
organization in Clerk's browser approval page. The client never changes an
organization ID locally to manufacture access. Unsupported rich approvals direct
you to `app.okou.ai`; this client never approves them implicitly.

## Limits and acceptance boundaries

- The list shows at most 128 recent visible threads and 128 projects. Conversation
  state retains a bounded recent window; oversized archives fail explicitly rather
  than exhausting device memory. See [protocol notes](docs/protocol.md).
- Typed drafts and audio are local sensitive data; Unix file permissions are not
  protection against root or theft of an unencrypted SD card.
- A successful HTTP submission means accepted, not run completion. An uncertain
  submission stays blocked until its stable event identity is found in durable
  history. Never delete its record and resend without checking the web conversation.
- The framebuffer/keyboard and actual ES8389 microphone require physical acceptance.
  Unit tests do not certify input routing, microphone quality or device memory.
- See [design and API dependencies](docs/design-review.md),
  [realtime behavior](docs/realtime.md), and [third-party licenses](THIRD_PARTY_NOTICES.md).

## References

- [Okou iOS](https://github.com/okou-ai/okou/tree/main/ios)
- [Cardputer Zero Template](https://github.com/CardputerZero/Template)
- [Cardputer Zero launcher](https://github.com/CardputerZero/launcher)
