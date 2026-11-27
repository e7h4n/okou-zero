# Store releases and updates

Both native CI jobs run the test suite. The arm64 job creates one store package,
`okou_VERSION_arm64.deb`, and smoke-tests the installed executable and Launcher
entry. Pull requests build and test the package without publishing a release.

Versions and `CHANGELOG.md` are managed by `release-please` from linear Squash
commits on `main` (`.release-please-manifest.json` and `CMakeLists.txt`). Merging
the generated Release PR runs the full native build and smoke tests on `main`,
creates a draft GitHub Release, validates and signs the tested `.deb` with
`manifest.json`, `manifest.sig`, and `SHA256SUMS`, uploads all four assets, and
publishes the release as the GitHub `Latest` release.

When the `CARDPUTER_PACKAGES_TOKEN` repository secret (`@e7h4n` Classic PAT with
`public_repo` scope) is configured, the workflow then updates `e7h4n/packages`
and opens a `CardputerZero/packages` store update PR pointing directly to the
published GitHub Release `.deb` asset with the release changelog in the PR body.
Installing the store package switches the device to AppStore-managed updates.

## Legacy standalone updates

The following applies to the standalone `okou-zero` package through 0.2.33.

### User flow

- Open the device app: an isolated Python child checks the latest stable release of `e7h4n/okou-zero` over HTTPS. It does not block LVGL or require GitHub/Okou credentials.
- Newer releases are automatically downloaded to a user-only cache, verified, and atomically marked ready. Existing verified identical bundles are reused.
- Only the explicit **Ctrl+Alt+U** chord on a fresh U key-down starts installation. Ctrl+U, Alt+U and key repeats do not. There is no startup installation or timer-triggered installation.
- During installation the UI remains visible, Escape is disabled and the process waits for its installer. After successful installation, Okou `exec`s the installed executable, preserving the launcher's child ownership. On failure it shows an update error and remains usable; it does not automatically replay installation. Okou defers installation during voice capture, upload, retained audio, an uncertain send, client ID entry or another busy action.
- Network errors, absent Releases, GitHub rate limits, missing privileges, invalid signatures, stale releases or a busy package manager never cause a forced upgrade.

### Trust and privilege boundary

The standalone package embeds the RSA public key `assets/update-public.pem` at `/usr/share/okou-zero/update-public.pem`. The signing private key is stored as the GitHub Actions repository secret `OKOU_UPDATE_SIGNING_KEY`, never in source, artifacts, packages or on the device. The repository's maintainers and signing workflow are trusted code publishers. Compromise of the signing authority can authorize malicious maintainer scripts as root; this is not a sandbox for publisher code.

Release assets:

- `okou-zero_VERSION_arm64.deb`
- `manifest.json`: strict fields `schema`, `version`, `architecture`, `package`, `filename`, `sha256`, `size`
- `manifest.sig`: detached RSA/SHA-256 signature over the **exact manifest bytes**
- `SHA256SUMS`: transport/debug convenience; the embedded-key signature is the authority

The manifest does not carry URLs, commands, destination paths or alternative package names. HTTPS hosts and redirects are allowlisted; metadata, signatures, downloads and version syntax are bounded. Only stable numeric `MAJOR.MINOR.PATCH` versions and the native architecture are accepted. No bearer tokens, client secrets or private account state are involved. There is no default update downgrade or same-version reinstall.

The unprivileged checker writes a framed signed bundle to `~/.cache/okou-zero/update/ready`. That cache is considered fully hostile by the installer. On the shortcut, the client sends bytes on stdin to exactly:

```
sudo -n /usr/libexec/okou-zero-update-install
```

The sudo rule grants only that root-owned program with **zero arguments**, to existing members of the local `sudo` group. It grants no arbitrary shell, URL, filesystem path or `dpkg` command. The helper also rejects arguments and non-root execution. Python runs in isolated mode and loads shared code only from a fixed root-owned absolute path; subprocess environments are rebuilt.

When upgrading an existing installation, install the PulseAudio PCM plugin once with `sudo apt-get install libasound2-plugins` before applying the signed update. A fresh installation with `apt install ./okou-zero_*.deb` resolves this prerequisite automatically.

The root helper takes an exclusive update lock, copies bounded stdin into a fresh root-only directory, independently verifies the signature, full payload SHA-256 and length, exact Debian Package/Version/Architecture, and that the installed package is fully configured and older. Only then does it run fixed `/usr/bin/dpkg --install` on the root-owned verified copy. It does not trust filenames or paths supplied by the user or manifest. It does not download arbitrary dependencies, retry a locked/failed package manager, or run user-supplied commands.

### Recovery

- No automatic rollback of Debian maintainer-script side effects. Keep power connected. An interrupted/failed `dpkg` may require an administrator to inspect and repair package-manager state. No failure is represented as confirmed success, and no uncertain install is automatically replayed.
- A failed relaunch returns to the launcher; reopen Okou manually. A failed check is retried only on a future app launch.
- `sudo` group membership is required for confirmation installs; other users may still use the app but cannot invoke this helper through the shipped rule.
- Public GitHub rate limits and connectivity apply. Signatures establish publisher authenticity, not release freshness or availability; blocking network traffic can prevent updates.
- Rotate signing keys through a package signed by the currently trusted key before switching future release signing. Do not replace the key behind existing devices without a transition.
