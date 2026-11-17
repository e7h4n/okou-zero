#!/usr/bin/env bash
# Usage: bash scripts/ci-package-smoke.sh PACKAGE.deb [OUTPUT_DIRECTORY]
# Intended for disposable Ubuntu CI runners; installs and then removes the package.
set -euo pipefail

if [[ $# -lt 1 || $# -gt 2 ]]; then
  echo "Usage: $0 PACKAGE.deb [OUTPUT_DIRECTORY]" >&2
  exit 2
fi
package=$(realpath -- "$1")
output_dir=${2:-build}
mkdir -p -- "$output_dir"
output_dir=$(realpath -- "$output_dir")

[[ $(dpkg-deb --field "$package" Package) == okou-zero ]]
[[ $(dpkg-deb --field "$package" Version) == 0.1.3 ]]
expected_arch=$(dpkg --print-architecture)
case "$(uname -m):$expected_arch" in
  x86_64:amd64|aarch64:arm64) ;;
  *) echo "Unsupported native runner architecture" >&2; exit 1 ;;
esac
actual_arch=$(dpkg-deb --field "$package" Architecture)
[[ "$actual_arch" == "$expected_arch" ]] || {
  echo "Package architecture $actual_arch does not match runner $expected_arch" >&2
  exit 1
}

# Never remove an existing installation belonging to the caller.
if dpkg-query -W -f='${Status}' okou-zero 2>/dev/null | grep -q 'install ok installed'; then
  echo "Refusing to replace an existing okou-zero installation" >&2
  exit 1
fi
staging=$(mktemp -d)
installation_attempted=false
cleanup() {
  status=$?
  trap - EXIT
  if [[ "$installation_attempted" == true ]]; then
    if ! sudo apt-get remove -y okou-zero; then
      echo "Failed to remove smoke-test installation" >&2
      status=1
    fi
  fi
  rm -rf -- "$staging"
  exit "$status"
}
trap cleanup EXIT

dpkg-deb --extract "$package" "$staging"
desktop="$staging/usr/share/applications/okou-zero.desktop"
test -f "$desktop"
grep -qx '\[Desktop Entry\]' "$desktop"
grep -qx 'Type=Application' "$desktop"
grep -Eq '^Name=.+' "$desktop"
grep -Eq '^Exec=(/usr/bin/)?okou-zero([[:space:]].*)?$' "$desktop"
test -x "$staging/usr/bin/okou-zero"
grep -qx 'Name=Okou' "$desktop"
launcher="$staging/usr/share/APPLaunch/applications/okou-zero.desktop"
grep -qx 'Name=Okou' "$launcher"
grep -qx 'Icon=share/images/okou-zero.png' "$launcher"
cmp "$staging/usr/share/APPLaunch/share/images/okou-zero.png" assets/okou-zero.png
test -f "$staging/usr/share/icons/hicolor/96x96/apps/okou-zero.png"

installation_attempted=true
sudo apt-get install -y "$package"
test -x /usr/bin/okou-zero
test -f /usr/share/applications/okou-zero.desktop
/usr/bin/okou-zero --version | grep -Fx 'Okou 0.1.3'
timeout 30s /usr/bin/okou-zero --headless --output "$output_dir/installed-headless.ppm"
timeout 30s /usr/bin/okou-zero --headless --output "$output_dir/installed-headless-keys.ppm" --keys enter,enter,esc

# Check every render, including the uninstalled binary's output when run by CI.
python3 - "$output_dir" <<'PY'
import pathlib
import re
import sys

paths = sorted(pathlib.Path(sys.argv[1]).glob('*.ppm'))
if not paths:
    raise SystemExit('No PPM renders found')
for path in paths:
    data = path.read_bytes()
    # PPM permits comments and whitespace between header tokens.
    tokens = []
    offset = 0
    for _ in range(4):
        match = re.match(rb'(?:\s+|#[^\r\n]*(?:\r\n|\r|\n))*([^\s#]+)', data[offset:])
        if not match:
            raise SystemExit(f'{path}: malformed PPM header')
        tokens.append(match.group(1))
        offset += match.end()
    magic, width, height, maximum = tokens
    if magic not in (b'P6', b'P3') or (int(width), int(height)) != (320, 170):
        raise SystemExit(f'{path}: expected 320x170 PPM')
    maximum = int(maximum)
    if not 1 <= maximum <= 65535:
        raise SystemExit(f'{path}: invalid sample maximum')
    if offset >= len(data) or data[offset] not in b' \t\r\n\v\f':
        raise SystemExit(f'{path}: missing raster separator')
    if magic == b'P6':
        # Consume exactly the separator, not whitespace-valued pixel bytes.
        offset += 2 if data[offset:offset + 2] == b'\r\n' else 1
        expected = 320 * 170 * 3 * (2 if maximum > 255 else 1)
        if len(data) - offset != expected:
            raise SystemExit(f'{path}: incorrect raster length')
    else:
        raster = re.sub(rb'#[^\r\n]*', b'', data[offset:]).split()
        if len(raster) != 320 * 170 * 3 or any(not 0 <= int(v) <= maximum for v in raster):
            raise SystemExit(f'{path}: invalid raster samples')
    print(f'{path.name}: valid 320x170 {magic.decode()} render')
PY

sudo apt-get remove -y okou-zero
installation_attempted=false
if dpkg-query -W -f='${Status}' okou-zero 2>/dev/null | grep -q 'install ok installed'; then
  echo "okou-zero remains installed after removal" >&2
  exit 1
fi
echo "Package smoke test passed ($actual_arch)"
