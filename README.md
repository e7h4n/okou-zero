# Okou

A native, keyboard-first Okou client for the **M5Stack Cardputer Zero**.
C++17, LVGL, a 320×170 RGB565 framebuffer, and the device's physical keyboard.
Independent implementation, [MIT licensed](LICENSE). The app display name is `Okou`;
package/executable remain `okou-zero`. Official avatar assets are retained from the
approved baseline and excluded from the code license; see [notices](THIRD_PARTY_NOTICES.md).

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
  zlib1g-dev nlohmann-json3-dev libasound2-dev libsdl2-dev libfreetype-dev fonts-noto-cjk \
  python3 openssl sudo
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DOKOU_ZERO_DESKTOP=ON
cmake --build build --parallel 2
ctest --test-dir build --output-on-failure
./build/okou-zero --demo
```

Public CI uses no production accounts, secrets, conversations or microphone.
Protocol fixtures run on loopback; UI tests use SDL's dummy video driver. Mock
screenshots intentionally contain demo conversations, never a real account.

Device packages are built by the native ARM64 CI matrix leg and
published as the `okou-zero-<version>-arm64` artifact, with SHA-256 checksums.
Both native amd64 (SDL2 simulator) and arm64 (device backend) builds retain
protocol, UI, mocked updater and installed-package smoke tests. **Do not
compile on the Cardputer.** No repository automation merges PRs or deploys a device.

## Install a CI-built package

On the device, after downloading and verifying the CI artifact:

```sh
sha256sum -c SHA256SUMS
sudo apt-get install ./okou-zero_0.2.3_arm64.deb
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

## Signed updates

Use only the [official GitHub Releases](https://github.com/e7h4n/okou-zero/releases).
Version **0.2.2** is the native V1 signed-update baseline; **0.2.3** passed the
real-device signed-upgrade flow ([acceptance evidence](docs/updates-acceptance.md)).
The obsolete Hello World release `v0.1.4` was deliberately cancelled and must not
be published or installed.
The app checks for a stable update at launch without installing it automatically.
Press **Ctrl+Alt+U** to explicitly confirm an offered update. The unprivileged
updater verifies the signed manifest, version, arm64 package metadata, digest and
size; the narrowly scoped root helper re-verifies before installation. Failure
leaves the current app in place. See [update security and recovery](docs/updates.md).

The package installs updater modules in `/usr/lib/okou-zero`, the public key in
`/usr/share/okou-zero`, and an isolated-Python helper at
`/usr/libexec/okou-zero-update-install`. Its root-owned `0440` sudoers rule permits
only that helper with **no arguments** for the sudo group, not arbitrary commands.
Runtime dependencies include Python 3, OpenSSL and sudo alongside native V1 dependencies.

Trusted repository tag-push CI publishes only after **both** native architecture
builds, tests and package-install/render/removal checks pass. Release metadata must
match the configured CMake version and package. The release-only
`OKOU_UPDATE_SIGNING_KEY` secret signs `manifest.json`; CI verifies the signature
against the bundled public key before creating the release, which also contains
`manifest.sig`, the arm64 `.deb` and `SHA256SUMS`. Existing releases are never
replaced. Public CI tests mock updater network calls and never install a real update.

## Keyboard

| Key | Action |
| --- | --- |
| Up / down, Enter | Select and open a row; scroll conversation history |
| Tab | Switch between chats and projects |
| Enter in composer | Explicitly submit text |
| Shift+Enter | Insert a newline |
| Backspace | Delete one UTF-8 code point |
| Ctrl+R | Start/finish dictation, or explicitly retry retained voice work |
| Esc while recording/recovering | Discard recording and retained voice work |
| Esc | Back/menu |
| Ctrl+W | Workspace/sign-out menu |
| Ctrl+L | Refresh authoritative state |
| Ctrl+Alt+U | Explicitly confirm an offered signed update |
| Ctrl+Q | Exit to launcher |

Workspace switching signs out and starts a new device grant; select the new
organization in Clerk's browser approval page. The client never changes an
organization ID locally to manufacture access. Unsupported rich approvals direct
you to `app.okou.ai`; this client never approves them implicitly.

## Limits and acceptance boundaries

- The list shows at most 128 recent visible threads and 128 projects. Conversation
  state retains a bounded recent window; oversized archives fail explicitly rather
  than exhausting device memory. See [protocol notes](docs/protocol.md).
- Typed drafts and recoverable audio remain in process memory, not on disk, and
  are cleared on account transition. Voice failures retain bounded in-memory
  progress for explicit retry; exiting/crashing loses that recovery state.
  Tokens and pending-send identities use private files. Unix permissions are not
  protection against root or theft of an unencrypted SD card.
- A successful HTTP submission means accepted, not run completion. An uncertain
  submission stays blocked until its stable event identity is found in durable
  history. Never delete its record and resend without checking the web conversation.
- The framebuffer/keyboard and actual ES8389 microphone require physical acceptance.
  Unit tests do not certify input routing, microphone quality or device memory.
- Version 0.2.0 supersedes the separately accepted 0.1.0 Hello World baseline;
  [its historical acceptance record](docs/hello-world-acceptance.md) is preserved.
- See [design and API dependencies](docs/design-review.md),
  [realtime behavior](docs/realtime.md), and [third-party licenses](THIRD_PARTY_NOTICES.md).

## References

- [Okou iOS](https://github.com/okou-ai/okou/tree/main/ios)
- [Cardputer Zero Template](https://github.com/CardputerZero/Template)
- [Cardputer Zero launcher](https://github.com/CardputerZero/launcher)
