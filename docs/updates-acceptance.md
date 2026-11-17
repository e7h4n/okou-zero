# Signed updater acceptance, 2026-11-17

## Published baseline

- Release: https://github.com/e7h4n/okou-zero/releases/tag/v0.2.2
- Source: `0af328652ccca8f91181078a9d06de2552935560`, based on the merged native V1 client, not the old Hello World client.
- Successful tag CI: https://github.com/e7h4n/okou-zero/actions/runs/44074006787
- Both native amd64 and native arm64 jobs built, ran protocol/UI/updater tests, packaged, installed, rendered the packaged UI, and removed the package successfully. Release publication depended on both jobs.
- Downloaded published manifest, detached signature, package and SHA256SUMS independently after publication. All hashes and the RSA/SHA-256 signature verified against the committed public key.
- Public-key DER SHA-256: `6c20694295871b5672a88a359259dac87d3b7820d22246f34fed70c54c4c21f4`.
- The obsolete `v0.1.4` run was cancelled; `v0.2.1` failed a protected-file metadata smoke-test check. Neither published a release. The check was fixed to use privileged `stat`; tags were not moved.

## Behavior and software checks

- Background startup check and automatic signed download, without blocking UI input.
- Only Ctrl+Alt+U requests installation; repeated keydown does not trigger it.
- Active work, voice capture, pending/uncertain sends and drafts in current or other threads defer installation.
- Installation waits through process interruption; successful replacement restarts the same executable with the original arguments, without rebooting the device.
- Local desktop CTest: 18/18; device-backend build: passed; Python updater/security tests: 27/27. The GitHub job supplies the native ARM64 execution evidence.
- Independent source reviews covered privilege/signature handling and native event-loop/process integration; identified installation-lifecycle issues were fixed before publication.
- `scripts/test-root-update.py` additionally exercises the **actual installed root helper and dpkg** on a disposable host. It creates an ephemeral TEST signing key, installs a signed next-patch fixture, rejects package tampering, same-version reinstall and downgrade, and restores the production PUBLIC key. It never uses the production signing secret or a release endpoint.
- That privileged smoke test passed locally against the CI-built amd64 0.2.2 package. The fixture changes Debian version metadata only, not the binary's compiled version, and is removed afterwards. Its fixture version is not a published release. The test is wired into subsequent dual-native CI smoke runs.

## Not yet accepted on physical hardware

Remote connectivity was unavailable for this stage. The new package was **not installed on the physical Cardputer during updater acceptance**, and no on-device shortcut, real update/relaunch, or physical finger-press acceptance is claimed.

Once connectivity is restored:

1. Inspect package/process/launcher state, then install the signed 0.2.2 baseline once.
2. Verify native framebuffer startup, input and return to the launcher.
3. Publish a subsequent CI-passed signed version, launch the baseline, and observe automatic download and the ready footer.
4. Confirm no installation occurs without Ctrl+Alt+U; then confirm package replacement and relaunch, preserving launcher ownership and arguments.
5. Confirm draft/voice deferral and failure recovery without a device reboot.

No device compilation, authentication secrets in public CI, network-security changes, forced updates, or reboot were used.
