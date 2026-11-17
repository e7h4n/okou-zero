"""Shared updater validation. Installed root-owned at /usr/lib/okou-zero/security.py."""
import hashlib
import json
import os
import re
import struct
import subprocess
import tempfile

MANIFEST_LIMIT = 16 * 1024
SIGNATURE_LIMIT = 1024
PACKAGE_LIMIT = 50 * 1024 * 1024
KEY_PATH = "/usr/share/okou-zero/update-public.pem"
ENV = {"PATH": "/usr/sbin:/usr/bin:/sbin:/bin", "LANG": "C", "LC_ALL": "C"}
VERSION = re.compile(r"(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)", re.ASCII)
ARCH = re.compile(r"[a-z0-9][a-z0-9-]{0,31}", re.ASCII)
FIELDS = {"schema", "version", "architecture", "package", "filename", "sha256", "size"}


class InvalidUpdate(Exception):
    pass


def run(args, timeout=30):
    return subprocess.run(args, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                          stderr=subprocess.DEVNULL, env=ENV, cwd="/", check=True,
                          timeout=timeout, close_fds=True).stdout


def version(value):
    if not isinstance(value, str) or len(value) > 64 or VERSION.fullmatch(value) is None:
        raise InvalidUpdate()
    return tuple(int(part) for part in value.split("."))


def architecture():
    value = run(["/usr/bin/dpkg", "--print-architecture"]).decode("ascii").strip()
    if ARCH.fullmatch(value) is None:
        raise InvalidUpdate()
    return value


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise InvalidUpdate()
        result[key] = value
    return result


def json_object(raw):
    try:
        return json.loads(raw.decode("utf-8"), object_pairs_hook=unique_object,
                          parse_constant=lambda _: (_ for _ in ()).throw(InvalidUpdate()))
    except (ValueError, UnicodeError, RecursionError) as exc:
        raise InvalidUpdate() from exc


def manifest(raw, native):
    if not 0 < len(raw) <= MANIFEST_LIMIT:
        raise InvalidUpdate()
    data = json_object(raw)
    if not isinstance(data, dict) or set(data) != FIELDS:
        raise InvalidUpdate()
    if type(data["schema"]) is not int or data["schema"] != 1:
        raise InvalidUpdate()
    version(data["version"])
    if not isinstance(native, str) or ARCH.fullmatch(native) is None:
        raise InvalidUpdate()
    if data["package"] != "okou-zero" or data["architecture"] != native:
        raise InvalidUpdate()
    expected = "okou-zero_" + data["version"] + "_" + native + ".deb"
    if data["filename"] != expected:
        raise InvalidUpdate()
    if not isinstance(data["sha256"], str) or re.fullmatch(r"[0-9a-fA-F]{64}", data["sha256"], re.ASCII) is None:
        raise InvalidUpdate()
    if type(data["size"]) is not int or not 0 < data["size"] <= PACKAGE_LIMIT:
        raise InvalidUpdate()
    return data


def verify_signature(raw, signature, directory, key=None):
    if not 0 < len(raw) <= MANIFEST_LIMIT or not 0 < len(signature) <= SIGNATURE_LIMIT:
        raise InvalidUpdate()
    key = KEY_PATH if key is None else key
    # Only private, freshly created directories may be supplied by callers.
    with tempfile.NamedTemporaryFile(dir=directory) as mf, tempfile.NamedTemporaryFile(dir=directory) as sf:
        mf.write(raw)
        mf.flush()
        sf.write(signature)
        sf.flush()
        run(["/usr/bin/openssl", "dgst", "-sha256", "-verify", key,
             "-signature", sf.name, mf.name])


def read_exact(stream, count):
    parts = []
    while count:
        chunk = stream.read(min(count, 65536))
        if not chunk or len(chunk) > count:
            raise InvalidUpdate()
        parts.append(chunk)
        count -= len(chunk)
    return b"".join(parts)


def read_header(stream):
    mlen, slen = struct.unpack(">II", read_exact(stream, 8))
    if not 0 < mlen <= MANIFEST_LIMIT or not 0 < slen <= SIGNATURE_LIMIT:
        raise InvalidUpdate()
    return read_exact(stream, mlen), read_exact(stream, slen)


def copy_package(stream, target, data):
    remaining = data["size"]
    digest = hashlib.sha256()
    while remaining:
        chunk = stream.read(min(remaining, 65536))
        if not chunk or len(chunk) > remaining:
            raise InvalidUpdate()
        digest.update(chunk)
        target.write(chunk)
        remaining -= len(chunk)
    if stream.read(1) or digest.hexdigest() != data["sha256"].lower():
        raise InvalidUpdate()


def write_header(target, raw, signature):
    target.write(struct.pack(">II", len(raw), len(signature)))
    target.write(raw)
    target.write(signature)


def installed_version():
    output = run(["/usr/bin/dpkg-query", "--show", "--showformat=${Status}\n${Version}\n", "okou-zero"])
    lines = output.decode("ascii").splitlines()
    if len(lines) != 2 or lines[0] != "install ok installed":
        raise InvalidUpdate()
    version(lines[1])
    return lines[1]


def validate_upgrade(data, installed, native):
    version(installed)
    if data["architecture"] != native:
        raise InvalidUpdate()
    # dpkg is authoritative for ordering, and receives no option-like versions.
    run(["/usr/bin/dpkg", "--compare-versions", data["version"], "gt", installed])


def validate_package(path, data, native):
    output = run(["/usr/bin/dpkg-deb", "--show", "--showformat=${Package}\n${Architecture}\n${Version}\n", path])
    if output.decode("ascii").splitlines() != ["okou-zero", native, data["version"]]:
        raise InvalidUpdate()
