#!/usr/bin/python3 -I
"""Unprivileged GitHub Releases client; stdout is a closed status protocol."""
import argparse
import importlib.util
import os
from pathlib import Path
import signal
import ssl
import stat
import subprocess
import sys
import tempfile
import time
import urllib.parse
import urllib.request

SECURITY_PATH = "/usr/lib/okou-zero/security.py"
LATEST = "https://api.github.com/repos/e7h4n/okou-zero/releases/latest"
HOSTS = {"github.com", "api.github.com", "objects.githubusercontent.com", "release-assets.githubusercontent.com"}
RELEASE_LIMIT = 1024 * 1024
TOTAL_TIMEOUT = 180


def load_security():
    spec = importlib.util.spec_from_file_location("okou_update_security", SECURITY_PATH)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def validate_url(url):
    if not isinstance(url, str) or len(url) > 8192 or any(ord(c) < 33 or ord(c) > 126 for c in url) or "\\" in url:
        raise ValueError()
    parsed = urllib.parse.urlsplit(url)
    if (parsed.scheme != "https" or parsed.hostname not in HOSTS or
            parsed.username is not None or parsed.password is not None or
            parsed.port not in (None, 443) or parsed.fragment):
        raise ValueError()
    return url


class Redirects(urllib.request.HTTPRedirectHandler):
    max_redirections = 5
    max_repeats = 5

    def __init__(self):
        super().__init__()
        self.count = 0

    def redirect_request(self, req, fp, code, msg, headers, newurl):
        self.count += 1
        if self.count > 5:
            raise ValueError()
        validate_url(newurl)
        return super().redirect_request(req, fp, code, msg, headers, newurl)


class HTTP:
    def __init__(self, deadline):
        self.deadline = deadline

    def remaining(self):
        remaining = self.deadline - time.monotonic()
        if remaining <= 0:
            raise TimeoutError()
        return min(30, remaining)

    def fetch(self, url, limit, target=None):
        validate_url(url)
        opener = urllib.request.build_opener(urllib.request.ProxyHandler({}), Redirects(),
                                            urllib.request.HTTPSHandler(context=ssl.create_default_context()))
        request = urllib.request.Request(url, headers={"User-Agent": "okou-zero-updater/1", "Accept-Encoding": "identity"})
        with opener.open(request, timeout=self.remaining()) as response:
            validate_url(response.geturl())
            if response.status != 200 or response.headers.get("Content-Encoding", "identity").lower() != "identity":
                raise ValueError()
            length = response.headers.get("Content-Length")
            if length is not None and (not length.isascii() or not length.isdecimal() or int(length) > limit):
                raise ValueError()
            chunks = []
            total = 0
            while True:
                self.remaining()
                chunk = response.read(min(65536, limit + 1 - total))
                self.remaining()
                if not chunk:
                    break
                total += len(chunk)
                if total > limit:
                    raise ValueError()
                if target is None:
                    chunks.append(chunk)
                else:
                    target.write(chunk)
            if length is not None and total != int(length):
                raise ValueError()
            return b"".join(chunks) if target is None else total


def emit(status):
    print(status, flush=True)


def cache_directory():
    path = Path.home() / ".cache" / "okou-zero" / "update"
    path.mkdir(mode=0o700, parents=True, exist_ok=True)
    # Refuse symlinks on the application-owned portion of the cache tree.
    for component in (path.parent, path):
        info = component.lstat()
        if not stat.S_ISDIR(info.st_mode) or info.st_uid != os.getuid():
            raise ValueError()
    os.chmod(path, 0o700)
    return path


def release_assets(raw, security):
    data = security.json_object(raw)
    if (not isinstance(data, dict) or data.get("draft") is not False or
            data.get("prerelease") is not False or not isinstance(data.get("assets"), list)):
        raise ValueError()
    assets = {}
    for item in data["assets"]:
        if not isinstance(item, dict) or not isinstance(item.get("name"), str):
            raise ValueError()
        name = item["name"]
        if name in assets:
            raise ValueError()
        assets[name] = item.get("browser_download_url")
    return assets


def cached_ready(cache, raw, signature, data, security):
    """Reuse only identical signed framing plus a freshly verified whole payload."""
    try:
        fd = os.open(cache / "ready", os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK | os.O_CLOEXEC)
        with os.fdopen(fd, "rb") as bundle:
            info = os.fstat(bundle.fileno())
            if not stat.S_ISREG(info.st_mode) or info.st_size != 8 + len(raw) + len(signature) + data["size"]:
                return False
            if security.read_header(bundle) != (raw, signature):
                return False
            with open(os.devnull, "wb") as sink:
                security.copy_package(bundle, sink, data)
        return True
    except (OSError, ValueError, security.InvalidUpdate):
        return False


def check(current, security, http=None):
    security.version(current)
    emit("CHECKING")
    http = http or HTTP(time.monotonic() + TOTAL_TIMEOUT)
    native = security.architecture()
    assets = release_assets(http.fetch(LATEST, RELEASE_LIMIT), security)
    raw = http.fetch(assets["manifest.json"], security.MANIFEST_LIMIT)
    signature = http.fetch(assets["manifest.sig"], security.SIGNATURE_LIMIT)
    cache = cache_directory()
    with tempfile.TemporaryDirectory(prefix="verify-", dir=cache) as directory:
        os.chmod(directory, 0o700)
        security.verify_signature(raw, signature, directory)
        data = security.manifest(raw, native)
        # Require the exact canonical asset even when reusing a cached bundle.
        url = validate_url(assets[data["filename"]])
        if security.version(data["version"]) <= security.version(current):
            emit("CURRENT")
            return
        if cached_ready(cache, raw, signature, data, security):
            emit("READY " + data["version"])
            return
        # The manifest cannot supply a URL, command, path, or alternate asset.
        emit("DOWNLOADING")
        package = os.path.join(directory, "package.deb")
        with open(package, "xb+") as output:
            size = http.fetch(url, data["size"], output)
            if size != data["size"]:
                raise ValueError()
            output.seek(0)
            with open(os.path.join(directory, "bundle"), "xb") as bundle:
                os.chmod(bundle.name, 0o600)
                security.write_header(bundle, raw, signature)
                # Verify every byte before atomically publishing the bundle.
                security.copy_package(output, bundle, data)
                bundle.flush()
                os.fsync(bundle.fileno())
        os.replace(os.path.join(directory, "bundle"), cache / "ready")
        dirfd = os.open(cache, os.O_RDONLY | os.O_DIRECTORY | os.O_CLOEXEC)
        try:
            os.fsync(dirfd)
        finally:
            os.close(dirfd)
    emit("READY " + data["version"])


def apply(current, security):
    security.version(current)
    fd = os.open(cache_directory() / "ready", os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK | os.O_CLOEXEC)
    with os.fdopen(fd, "rb") as bundle:
        info = os.fstat(bundle.fileno())
        if not stat.S_ISREG(info.st_mode) or info.st_size > 8 + security.MANIFEST_LIMIT + security.SIGNATURE_LIMIT + security.PACKAGE_LIMIT:
            raise ValueError()
        # Untrusted cache is intentionally sent as bytes, never as a path or
        # command argument. Root re-verifies the entire bundle independently.
        subprocess.run(["/usr/bin/sudo", "-n", "/usr/libexec/okou-zero-update-install"],
                       stdin=bundle, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                       env=security.ENV, cwd="/", timeout=750, check=True, close_fds=True)
    emit("INSTALLED")


class Parser(argparse.ArgumentParser):
    def error(self, message):
        raise ValueError()


def expired(signum, frame):
    raise TimeoutError()


def main(argv=None):
    try:
        parser = Parser(add_help=False)
        parser.add_argument("command", choices=("check", "apply"))
        parser.add_argument("--current", required=True)
        args = parser.parse_args(argv)
        os.umask(0o077)
        security = load_security()
        if args.command == "check":
            signal.signal(signal.SIGALRM, expired)
            signal.alarm(TOTAL_TIMEOUT)
            try:
                check(args.current, security)
            finally:
                signal.alarm(0)
        else:
            apply(args.current, security)
        return 0
    except Exception:
        emit("ERROR")
        return 1


if __name__ == "__main__":
    sys.exit(main())
