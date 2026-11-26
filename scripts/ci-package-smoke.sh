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
[[ $(dpkg-deb --field "$package" Package) == okou ]]
version=$(dpkg-deb --field "$package" Version)
expected_arch=$(dpkg --print-architecture)
[[ $(dpkg-deb --field "$package" Architecture) == "$expected_arch" ]]
[[ "$version" =~ ^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$ ]]
[[ $(basename -- "$package") == "okou_${version}_${expected_arch}.deb" ]]
case "$(uname -m):$expected_arch" in
  aarch64:arm64) ;;
  *) echo 'Unsupported native runner architecture' >&2; exit 1 ;;
esac
[[ $(dpkg-deb --field "$package" Pre-Depends) == libasound2-plugins ]]
dependencies=$(dpkg-deb --field "$package" Depends)
for dependency in ca-certificates fonts-noto-cjk python3; do
  grep -Eq "(^|,)[[:space:]]*${dependency}([[:space:]]*\\([^)]*\\))?[[:space:]]*(,|$)" <<< "$dependencies"
done
for installed in okou okou-zero; do
  if dpkg-query -W -f='${Status}' "$installed" 2>/dev/null | grep -q 'install ok installed'; then
    echo "Refusing to replace an existing $installed installation" >&2
    exit 1
  fi
done
staging=$(mktemp -d)
installation_attempted=false
cleanup() {
  status=$?
  trap - EXIT
  if [[ "$installation_attempted" == true ]]; then
    if ! sudo apt-get remove -y okou; then status=1; fi
  fi
  rm -rf -- "$staging"
  exit "$status"
}
trap cleanup EXIT
dpkg-deb --extract "$package" "$staging/package"
[[ $(dpkg-deb --field "$package" Conflicts) == okou-zero ]]
[[ $(dpkg-deb --field "$package" Replaces) == okou-zero ]]
root="$staging/package/usr"
cmp packaging/okou-zero.desktop "$root/share/APPLaunch/applications/okou-zero.desktop"
cmp assets/okou-zero.png "$root/share/APPLaunch/share/images/okou-zero.png"
cmp packaging/store_updates.py "$root/lib/okou-zero/updater.py"
test -x "$root/bin/okou-zero"
test ! -e "$root/libexec/okou-zero-update-install"
test ! -e "$staging/package/etc/sudoers.d/okou-zero-update"
for asset in assets/orbit/*; do
  cmp "$asset" "$staging/package/usr/share/okou-zero/orbit/${asset##*/}"
done
for license in assets/licenses/*; do
  cmp "$license" "$staging/package/usr/share/doc/okou-zero/${license##*/}"
done
installation_attempted=true
sudo apt-get install -y "$package"
/usr/bin/okou-zero --version | grep -Fx "Okou $version"
[[ $(python3 -I /usr/lib/okou-zero/updater.py check --current "$version") == CURRENT ]]
# Native UI self-test runs with an isolated home and does not use a real account.
test -f /usr/share/doc/okou-zero/LICENSE
test -f /usr/share/doc/okou-zero/THIRD_PARTY_NOTICES.md
mkdir -p "$staging/home"
HOME="$staging/home" XDG_CONFIG_HOME="$staging/home/config" XDG_DATA_HOME="$staging/home/data" \
  timeout 30s /usr/bin/okou-zero --headless --self-test-ui --output "$output_dir/installed-headless.ppm"
HOME="$staging/home" XDG_CONFIG_HOME="$staging/home/config" XDG_DATA_HOME="$staging/home/data" \
  timeout 30s /usr/bin/okou-zero --headless --demo-screen update --output "$output_dir/installed-update.ppm"
python3 - "$output_dir/installed-headless.ppm" "$output_dir/installed-update.ppm" <<'PY'
import pathlib
import sys
for filename in sys.argv[1:]:
    image = pathlib.Path(filename).read_bytes()
    header = b'P6\n320 170\n255\n'
    if not image.startswith(header) or len(image) != len(header) + 320 * 170 * 3:
        raise SystemExit(f'{filename}: invalid native headless framebuffer render')
    if len(set(image[len(header):])) < 10:
        raise SystemExit(f'{filename}: blank native headless framebuffer render')
    print(f'{filename}: valid, nonblank installed 320x170 render')
PY
sudo apt-get remove -y okou
installation_attempted=false
test ! -e /usr/bin/okou-zero
echo "Package installation/render/removal passed ($expected_arch)"
