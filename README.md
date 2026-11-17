# okou

A native, keyboard-first Okou client for the M5Stack Cardputer Zero.

**Milestone 0: offline Hello World, C++17/LVGL, headless UI tests and CI-built Debian packages.** Login, chat, Projects, Ably and voice are not implemented yet. This milestone makes no network requests.

[Acceptance record](docs/hello-world-acceptance.md): both CI architectures pass and the CI package is installed and visible in the device launcher. Physical App launch/input/return verification is still blocked by an SSH transport failure; it is not claimed complete.

## Try it on Cardputer Zero

1. Open [GitHub Actions](https://github.com/e7h4n/okou-zero/actions), choose a successful CI run and download **okou-zero-0.1.1-arm64** (GitHub sign-in may be required). Artifacts are retained for 14 days, not permanent releases.
2. Extract the artifact, then verify it: `sha256sum --check SHA256SUMS`.
3. Copy the `.deb` to the device and install: `sudo apt install ./okou-zero_0.1.1_arm64.deb`.
4. Reload the launcher (`systemctl --user restart APPLaunch.service` while no other app is active), select **okou**, press Enter.
5. **Enter** increments the greeting counter; **Esc** exits back to the launcher.

Targets the actual 320×170 RGB565 LCD (`/dev/fb0`) and the built-in evdev keyboard. Run as the normal device user with `video` and `input` group access, not root. Launch through the launcher so it hands off the framebuffer and input; do not start a second framebuffer app over another active app. No inbound port, daemon, firmware or network configuration is installed.

Uninstall: `sudo apt remove okou-zero`. `--version` and `--headless` require no device access. `--framebuffer PATH` and `--input PATH` override platform defaults.

## Build and test on Linux

Requires CMake ≥3.28, a C/C++17 compiler, Python 3, Git, and dpkg-dev for packaging. LVGL 9.5.0 is fetched from an immutable commit during configuration; internet access is needed for the first build.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j2
ctest --test-dir build --output-on-failure
./build/okou-zero --headless --output build/hello.ppm
./build/okou-zero --headless --output build/keys.ppm --keys enter,enter,esc
(cd build && cpack -G DEB)
```

The headless renderer uses the same LVGL view and state reducer as the device, emits a deterministic 320×170 PPM and exits without opening hardware. It is not an interactive desktop window yet.

CI runs on native Ubuntu 24.04 amd64 **and arm64** runners for pushes and public pull requests. Both run unit/UI tests, build `.deb` packages, verify architecture/dependencies/desktop metadata, install and run the packaged binary headlessly, uninstall, and upload packages, renders and SHA-256 checksums. Fork PRs use read-only permissions and no production credentials. Checksums detect corruption, not publisher authenticity; use a trusted CI run and commit. No release publication/signing policy is configured yet.

## Layout

- `src/ui.*`: greeting state and shared LVGL view.
- `src/main.cpp`: CLI, headless renderer, framebuffer/keyboard loop and signal cleanup.
- `tests/`: state transitions and deterministic UI/keyboard/error smoke tests.
- `packaging/`: launcher and standard Linux desktop entries.
- `.github/workflows/ci.yml`: native test/build/package/install pipeline.
- `docs/design-review.md`: future API compatibility and security requirements.

## V1 roadmap (not this milestone)

1. Clerk OAuth device-code login, without a client secret or CLI-token substitution.
2. Workspace/Project and thread lists, Ably connection and active/unread indicators.
3. Conversation history, text composer and voice → editable draft → explicit send.
4. Bounded synchronization, credential lifecycle, fake-server protocol tests and device acceptance.

Project, Ably and voice authorization coverage remains a separate backend dependency; see [the compatibility review](docs/design-review.md). No main Okou API changes are included here.

## References and licensing

- [Okou iOS behavior reference](https://github.com/okou-ai/okou/tree/main/ios)
- [Cardputer Zero application template](https://github.com/CardputerZero/Template)
- [Cardputer Zero launcher and hardware SDK](https://github.com/CardputerZero/launcher)
- [Clerk device authorization](https://clerk.com/docs/guides/configure/auth-strategies/oauth/device-authorization-grant)

[MIT](LICENSE), independent implementation. No BSL upstream implementation is copied. See [third-party notices](THIRD_PARTY_NOTICES.md) for LVGL and font licenses. The icon uses the official Okou avatar at the owner's request; brand assets are excluded from the MIT code license. The app display name is `okou`; the package and executable remain `okou-zero` for upgrade compatibility.
