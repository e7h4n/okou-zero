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
[[ "$version" =~ ^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$ ]]
[[ $(basename -- "$package") == "okou-zero_${version}_${expected_arch}.deb" ]]
case "$(uname -m):$expected_arch" in
  x86_64:amd64|aarch64:arm64) ;;
  *) echo 'Unsupported native runner architecture' >&2; exit 1 ;;
esac
[[ $(dpkg-deb --field "$package" Pre-Depends) == libasound2-plugins ]]
dependencies=$(dpkg-deb --field "$package" Depends)
for dependency in ca-certificates fonts-noto-cjk python3 openssl sudo; do
  grep -Eq "(^|,)[[:space:]]*${dependency}([[:space:]]*\\([^)]*\\))?[[:space:]]*(,|$)" <<< "$dependencies"
done
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
verify_updater_files() {
  local root=$1
  local helper="$root/usr/libexec/okou-zero-update-install"
  local key="$root/usr/share/okou-zero/update-public.pem"
  local sudoers="$root/etc/sudoers.d/okou-zero-update"
  test -f "$root/usr/lib/okou-zero/updater.py"
  test -f "$root/usr/lib/okou-zero/security.py"
  test -x "$helper"
  IFS= read -r shebang < "$helper"
  [[ "$shebang" == '#!/usr/bin/python3 -I' ]]
  cmp "$key" assets/update-public.pem
  openssl pkey -pubin -in "$key" -noout
  sudo cmp "$sudoers" packaging/okou-zero-update.sudoers
  [[ $(sudo grep -Ev '^[[:space:]]*(#|$)' "$sudoers") == '%sudo ALL=(root) NOPASSWD: /usr/libexec/okou-zero-update-install ""' ]]
  sudo visudo -cf "$sudoers"
  [[ $(sudo stat -c '%a' "$sudoers") == 440 ]]
}
for asset in assets/orbit/*; do
  cmp "$asset" "$staging/package/usr/share/okou-zero/orbit/${asset##*/}"
done
for license in assets/licenses/*; do
  cmp "$license" "$staging/package/usr/share/doc/okou-zero/${license##*/}"
done
verify_updater_files "$staging/package"
installation_attempted=true
sudo apt-get install -y "$package"
/usr/bin/okou-zero --version | grep -Fx "Okou $version"
verify_updater_files ''
for protected in /usr/libexec/okou-zero-update-install /usr/lib/okou-zero/updater.py /usr/lib/okou-zero/security.py /usr/share/okou-zero/update-public.pem /etc/sudoers.d/okou-zero-update; do
  [[ $(sudo stat -c '%U:%G' "$protected") == root:root ]]
  mode=$(sudo stat -c '%a' "$protected")
  (( (8#$mode & 8#022) == 0 ))
done
# Never invoke installed updater check/apply or feed the root helper a real bundle.
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
# Disposable host only: exercise an actual signed dpkg installation using an
# ephemeral TEST key and fixture, then reject tampering/reinstall/downgrade.
# This never accesses a production signing secret or a release endpoint.
sudo env OKOU_ZERO_DISPOSABLE_UPDATE_TEST=1 /usr/bin/python3 scripts/test-root-update.py "$package"
sudo apt-get remove -y okou-zero
installation_attempted=false
test ! -e /usr/bin/okou-zero
echo "Package installation/render/removal passed ($expected_arch)"
