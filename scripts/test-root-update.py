#!/usr/bin/env python3
"""Destructive signed-install fixture, ONLY for an explicitly disposable CI host.

The smoke-test caller has installed PACKAGE.deb and owns removing it afterwards.
This uses temporary TEST keys, never the production Actions signing secret.
The future fixture changes Debian metadata only, not the binary's compiled version.
"""
import hashlib
import json
import os
from pathlib import Path
import re
import struct
import subprocess
import sys
import tempfile

KEY = Path('/usr/share/okou-zero/update-public.pem')
HELPER = '/usr/libexec/okou-zero-update-install'
ENV = {'PATH': '/usr/sbin:/usr/bin:/sbin:/bin', 'LANG': 'C', 'LC_ALL': 'C'}


def run(args, **kwargs):
    return subprocess.run(args, check=True, env=ENV, stdin=subprocess.DEVNULL,
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE, **kwargs).stdout


def field(package, name):
    return run(['/usr/bin/dpkg-deb', '--field', str(package), name]).decode().strip()


def installed():
    return run(['/usr/bin/dpkg-query', '-W', '-f=${Status}\n${Version}', 'okou-zero']).decode()


def main():
    if os.geteuid() != 0 or os.environ.get('OKOU_ZERO_DISPOSABLE_UPDATE_TEST') != '1' or len(sys.argv) != 2:
        raise SystemExit('Refusing: requires root and explicit disposable-host test opt-in')
    os.umask(0o077)
    baseline = Path(sys.argv[1]).resolve(strict=True)
    version = field(baseline, 'Version')
    if field(baseline, 'Package') != 'okou-zero' or not re.fullmatch(r'(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)', version):
        raise RuntimeError('Invalid baseline package')
    if installed() != 'install ok installed\n' + version:
        raise RuntimeError('Disposable test requires exactly the supplied baseline installed')
    architecture = field(baseline, 'Architecture')
    if KEY.is_symlink() or not KEY.is_file():
        raise RuntimeError('Missing protected public key')
    original_key = KEY.read_bytes()
    parts = version.split('.')
    parts[-1] = str(int(parts[-1]) + 1)
    future = '.'.join(parts)
    try:
        with tempfile.TemporaryDirectory(prefix='okou-root-update-fixture-') as temp:
            folder = Path(temp)
            private, public = folder / 'test-private.pem', folder / 'test-public.pem'
            run(['/usr/bin/openssl', 'genpkey', '-algorithm', 'RSA', '-pkeyopt', 'rsa_keygen_bits:2048', '-out', str(private)])
            run(['/usr/bin/openssl', 'pkey', '-in', str(private), '-pubout', '-out', str(public)])
            extracted = folder / 'package'
            run(['/usr/bin/dpkg-deb', '--raw-extract', str(baseline), str(extracted)])
            control = extracted / 'DEBIAN/control'
            text, count = re.subn(r'^Version: .+$', 'Version: ' + future, control.read_text(), flags=re.MULTILINE)
            if count != 1:
                raise RuntimeError('Malformed baseline control')
            control.write_text(text)
            candidate = folder / f'okou-zero_{future}_{architecture}.deb'
            run(['/usr/bin/dpkg-deb', '--build', '--root-owner-group', str(extracted), str(candidate)])

            def bundle(package, package_version):
                payload = package.read_bytes()
                raw = (json.dumps({'schema': 1, 'version': package_version, 'architecture': architecture,
                                   'package': 'okou-zero', 'filename': f'okou-zero_{package_version}_{architecture}.deb',
                                   'sha256': hashlib.sha256(payload).hexdigest(), 'size': len(payload)}, sort_keys=True) + '\n').encode()
                manifest, signature = folder / 'manifest', folder / 'signature'
                manifest.write_bytes(raw)
                run(['/usr/bin/openssl', 'dgst', '-sha256', '-sign', str(private), '-out', str(signature), str(manifest)])
                sig = signature.read_bytes()
                return struct.pack('>II', len(raw), len(sig)) + raw + sig + payload

            def invoke(data, success):
                result = subprocess.run([HELPER], input=data, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                        env=ENV, cwd='/', timeout=650, check=False)
                if (result.returncode == 0) != success:
                    raise RuntimeError('Signed root-helper result did not match expectation')

            valid = bundle(candidate, future)
            KEY.write_bytes(public.read_bytes())
            damaged = valid[:-1] + bytes([valid[-1] ^ 1])
            invoke(damaged, False)
            if installed() != 'install ok installed\n' + version:
                raise RuntimeError('Tampered update changed the installed package')
            invoke(valid, True)
            if installed() != 'install ok installed\n' + future:
                raise RuntimeError('Verified fixture was not installed')
            # The newly installed fixture carries the original production PUBLIC
            # key. This also proves the test key never entered a package asset.
            if KEY.read_bytes() != original_key:
                raise RuntimeError('Fixture unexpectedly changed the production public key')
            KEY.write_bytes(public.read_bytes())
            invoke(valid, False)  # Same version must not be reinstalled.
            invoke(bundle(baseline, version), False)  # Nor may it downgrade.
            if installed() != 'install ok installed\n' + future:
                raise RuntimeError('Rejected update changed package state')
    finally:
        KEY.write_bytes(original_key)
        os.chmod(KEY, 0o644)
    print('Real root-helper install passed; tampering, reinstall and downgrade refused (temporary test keys only)')


if __name__ == '__main__':
    main()
