#!/usr/bin/python3 -I
"""Install as root-owned /usr/libexec/okou-zero-update-install (0755).

The only input is a framed signed bundle on stdin. Publisher-maintained Debian
maintainer scripts intentionally execute as root after verification.
"""
import fcntl
import importlib.util
import os
import signal
import stat
import subprocess
import sys
import tempfile

SECURITY_PATH = "/usr/lib/okou-zero/security.py"
# /run is root-owned, unlike commonly world-writable /run/lock.
LOCK_PATH = "/run/okou-zero-update.lock"


def load_security():
    spec = importlib.util.spec_from_file_location("okou_update_security", SECURITY_PATH)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def expired(signum, frame):
    raise TimeoutError()


def install(stream, security):
    fd = os.open(LOCK_PATH, os.O_CREAT | os.O_RDWR | os.O_NOFOLLOW | os.O_CLOEXEC | os.O_NONBLOCK, 0o600)
    try:
        info = os.fstat(fd)
        if not stat.S_ISREG(info.st_mode) or info.st_uid != 0 or info.st_nlink != 1:
            raise security.InvalidUpdate()
        fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        with tempfile.TemporaryDirectory(prefix="okou-zero-install-", dir="/var/tmp") as directory:
            os.chmod(directory, 0o700)
            raw, signature = security.read_header(stream)
            security.verify_signature(raw, signature, directory)
            native = security.architecture()
            data = security.manifest(raw, native)
            path = os.path.join(directory, "package.deb")
            with open(path, "xb") as target:
                os.chmod(path, 0o600)
                security.copy_package(stream, target, data)
                target.flush()
                os.fsync(target.fileno())
            security.validate_upgrade(data, security.installed_version(), native)
            security.validate_package(path, data, native)
            # Input deadline is no longer relevant. dpkg has a separate bounded
            # execution time; failed/locked dpkg operations are never retried.
            signal.alarm(0)
            subprocess.run(["/usr/bin/dpkg", "--install", path], stdin=subprocess.DEVNULL,
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                           env=security.ENV, cwd="/", timeout=600, check=True, close_fds=True)
    finally:
        os.close(fd)


def main():
    if len(sys.argv) != 1 or os.getuid() != 0 or os.geteuid() != 0:
        return 1
    os.umask(0o077)
    signal.signal(signal.SIGALRM, expired)
    signal.alarm(120)
    try:
        install(sys.stdin.buffer, load_security())
        return 0
    except Exception:
        return 1
    finally:
        signal.alarm(0)


if __name__ == "__main__":
    sys.exit(main())
