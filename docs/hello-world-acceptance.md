# Hello World acceptance record

Date: 2026-11-17. **Partially verified: CI, package install and launcher discovery pass; physical application launch/input/return remain blocked by transport failure.**

## Build evidence

- Source commit: `11fc975ec3f8eec6a2a191f4be0ce5d17fa3a59f`.
- CI: https://github.com/e7h4n/okou-zero/actions/runs/44063636040
- Native Ubuntu 24.04 amd64 and arm64 jobs both succeeded.
- Each job built the binary, passed state/framebuffer metadata and headless LVGL tests, built a Debian package, installed and ran the packaged binary headlessly, removed it, and generated/verified SHA-256 checksums.
- ARM64 artifact ID: `14320125082`, name `okou-zero-0.1.0-arm64`.
- Installed package: `okou-zero_0.1.0_arm64.deb`, 509978 bytes.
- Package SHA-256: `a8430ff5242190c5fcff282295c551c8cdc0d04a420cdf7bd3351643b9d24730`.
- Runtime package dependencies: libc6 >=2.34, libgcc-s1 >=3.0, libstdc++6 >=13.1.
- Artifact retention: 14 days. These are CI artifacts, not signed permanent releases.

## Device evidence

The device is ARM64 Raspberry Pi OS / Debian 13.7, with a physical 320×170 RGB565 framebuffer (640-byte stride), and the built-in evdev keyboard. It was not used for compilation.

Verified:

1. Downloaded the ARM64 artifact from the successful CI run and checked every checksum locally.
2. Uploaded the exact package; transfer receipt and on-device sha256sum matched the CI package hash.
3. Installed with apt; package database reports `okou-zero 0.1.0`.
4. Installed `okou-zero --version` reports `Okou Zero 0.1.0`.
5. Installed headless run with `enter,enter,esc` reports `greetings=2 running=0`.
6. Restarted the user launcher while no app was active; service reported active.
7. Read actual `/dev/fb0` pixels and confirmed **Okou Zero** is visible and selectable in the launcher. A right-arrow event injected into the built-in keyboard device selected it. This validates the evdev software path, not a human pressing a physical key.

Not yet verified:

- Entering the app from the launcher and seeing Hello World on the physical framebuffer.
- Physical framebuffer greeting update after Enter.
- Esc exit and successful launcher return.
- Human keypresses. Automated device input above was evdev injection, not a physical finger press.

The launch-key SSH attempt failed **before command execution**, at the Cloudflare Access WebSocket stage. No SSH authentication or application execution was reached. A subsequent read-only device-state query failed at the same stage. Exact failure receipts were inspected by Attempt ID. These observations do not establish the underlying network/device cause. No uncertain input was replayed, no route or firewall setting was changed, and no application/SSH/VNC background session was left by this acceptance work.

## Resume safely

Recheck authorized SSH access and current device/launcher state first. Capture the current framebuffer before selecting or submitting anything. If Okou Zero is still selected, launch once, capture the actual framebuffer, press Enter once and confirm the changed view, then Esc once and confirm both process exit and launcher redraw. Record the device evidence here; do not rebuild on the device or substitute a locally built package.
