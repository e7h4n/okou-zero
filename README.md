# Okou

A native Okou client for the **M5Stack Cardputer Zero**. It opens one conversation
per account and workspace. Tap Enter to record and tap it again to send.
The folded reader shows one Markdown reply at a time, with its local send time and
an index of earlier replies. The built-in interface is in English. Messages use
their original language.

The device uses LVGL, a 320×170 RGB565 framebuffer and the physical keyboard.
An SDL2 simulator runs the same reader on a development machine. The C++17
implementation is [MIT licensed](LICENSE). See [third-party notices](THIRD_PARTY_NOTICES.md)
for dependencies and the official Okou launcher icons.

## Build and test on a development machine

Ubuntu 24.04 (amd64 or arm64):

```sh
sudo apt-get install cmake ninja-build g++ libcurl4-openssl-dev libssl-dev \
  zlib1g-dev nlohmann-json3-dev libasound2-dev libasound2-plugins libpulse-dev libsdl2-dev libfreetype-dev fonts-noto-cjk \
  python3 openssl sudo
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DOKOU_ZERO_DESKTOP=ON
cmake --build build --parallel 2
ctest --test-dir build --output-on-failure
./build/okou-zero --demo-screen tour
```

The amd64 CI job builds the simulator. The native arm64 job builds the device
backend. Both run protocol and UI tests. Only arm64 builds a Debian package. CI
installs, exercises and removes the store package before publishing it.

## Install and sign in

Install **Okou** from the Cardputer Zero AppStore once the listing is approved.
To install a build directly, download `okou_VERSION_arm64.deb`, `manifest.json`,
`manifest.sig` and `SHA256SUMS` from the same versioned
[GitHub release](https://github.com/e7h4n/okou-zero/releases) into an empty
directory. Run these commands on the device:

```sh
sha256sum -c SHA256SUMS
sudo apt-get install ./okou_*_arm64.deb
```

Open **Okou** from APPLaunch so the launcher hands over the framebuffer and keyboard.
Press Enter to sign in and select the workspace on Clerk's device approval page.
Official packages bundle the public OAuth client ID. Local builds can set it
on the setup screen. See [authentication and configuration](docs/auth-setup.md).

The store manages updates. The Debian package replaces an existing standalone
`okou-zero` installation and uses the same account and conversation directories.
See [release and update details](docs/updates.md).

## Keyboard

| Key | Action |
| --- | --- |
| Up / down | Scroll the current reply one line, or select a settings item |
| Left / right | Jump to the beginning / end of the reply, then to the older / newer reply |
| Page Up / Page Down | Scroll the current reply three lines |
| Space on the welcome screen | Send Okou on a short orbit |
| Enter in settings | Open the selected action |
| Enter in chat | Tap to record. Tap again to send (recordings under 1s are skipped). |
| Volume up / down, Mute | Adjust system output by 10%, or toggle mute. The screen briefly shows the resulting level. |
| Ctrl+R | Retry a retained recording or failed voice message |
| Esc while recording/recovering | Discard recording and retained voice work |
| Esc | Back/menu |
| Ctrl+W | Workspace/sign-out menu |
| Ctrl+L | Refresh authoritative state |
| Ctrl+Q | Exit to launcher |

On the handheld, Okou uses the launcher's **Screen → DarkTime** setting when it opens
(default 30 seconds, Never disables automatic screen-off). Recording and installing
an update keep the screen awake. When idle, the backlight turns off and drawing
pauses while messages continue to arrive. Press any key once to restore the
previous brightness. A reply to a message sent from this device also wakes the screen.

Workspace switching signs out and starts a new device grant. Select the other
organization on Clerk's browser approval page.

## Conversation storage

The client retains a bounded window of conversation history. Failed uploads keep
the recording in memory for explicit retry. If an upload has completed, retry
reuses its file ID. Exiting the app loses this in-memory recovery state.

Before sending, the client writes a private identity record scoped to the account,
workspace, API origin and thread. An ambiguous response blocks further sends
until the matching input appears in validated history. The record survives an
app restart. See [the client protocol](docs/protocol.md).

## References

- [Okou iOS](https://github.com/okou-ai/okou/tree/main/ios)
- [Cardputer Zero Template](https://github.com/CardputerZero/Template)
- [Cardputer Zero launcher](https://github.com/CardputerZero/launcher)
