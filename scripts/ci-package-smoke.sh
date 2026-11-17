#!/usr/bin/env bash
# Disposable native CI runner only. Never use to replace a user's installation.
set -euo pipefail
if [[ $# -lt 1 || $# -gt 2 ]]; then
  echo 'Usage: ci-package-smoke.sh PACKAGE.deb [OUTPUT_DIRECTORY]' >&2
  exit 2
fi
package=$(realpath -- "$1")
output_dir=$(realpath -m -- "${2:-build}")
mkdir -p -- "$output_dir"
[[ $(dpkg-deb --field "$package" Package) == okou-zero ]]
version=$(dpkg-deb --field "$package" Version)
expected_arch=$(dpkg --print-architecture)
[[ $(dpkg-deb --field "$package" Architecture) == "$expected_arch" ]]
if dpkg-query -W -f='${Status}' okou-zero 2>/dev/null | grep -q 'install ok installed'; then
  echo 'Refusing to replace an existing installation' >&2
  exit 1
fi
staging=$(mktemp -d)
installation_attempted=false
cleanup() {
  status=$?
  trap - EXIT
  if [[ "$installation_attempted" == true ]]; then
    if ! sudo apt-get remove -y okou-zero; then status=1; fi
  fi
  rm -rf -- "$staging"
  exit "$status"
}
trap cleanup EXIT
dpkg-deb --extract "$package" "$staging/package"
for desktop in share/applications share/APPLaunch/applications; do
  test -f "$staging/package/usr/$desktop/okou-zero.desktop"
  grep -qx 'Exec=/usr/bin/okou-zero' "$staging/package/usr/$desktop/okou-zero.desktop"
done
test -x "$staging/package/usr/bin/okou-zero"
installation_attempted=true
sudo apt-get install -y "$package"
/usr/bin/okou-zero --version | grep -F "$version"
test -f /usr/share/doc/okou-zero/LICENSE
test -f /usr/share/doc/okou-zero/THIRD_PARTY_NOTICES.md
mkdir -p "$staging/home"
HOME="$staging/home" XDG_CONFIG_HOME="$staging/home/config" XDG_DATA_HOME="$staging/home/data" \
  timeout 30s /usr/bin/okou-zero --headless --self-test-ui --output "$output_dir/installed-headless.ppm"
python3 - "$output_dir/installed-headless.ppm" <<'PY'
import pathlib
import sys
image = pathlib.Path(sys.argv[1]).read_bytes()
header = b'P6\n320 170\n255\n'
if not image.startswith(header) or len(image) != len(header) + 320 * 170 * 3:
    raise SystemExit('Invalid native headless framebuffer render')
if len(set(image[len(header):])) < 10:
    raise SystemExit('Blank native headless framebuffer render')
print('Installed application: valid, nonblank 320x170 render')
PY
sudo apt-get remove -y okou-zero
installation_attempted=false
test ! -e /usr/bin/okou-zero
echo "Package installation/render/removal passed ($expected_arch)"
