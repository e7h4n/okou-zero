"""Updater security fixtures. No production keys, network, sudo or installation."""
import contextlib
import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import time
import unittest
from unittest.mock import MagicMock, patch
import urllib.request

ROOT = Path(__file__).resolve().parents[1]


def load(name, filename):
    spec = importlib.util.spec_from_file_location(name, ROOT / "updater" / filename)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


sec = load("updater_security_test", "security.py")
client = load("updater_client_test", "updater.py")
helper = load("updater_helper_test", "install_helper.py")


def metadata(payload=b"fixture Debian bytes", **changes):
    data = {"schema": 1, "version": "0.1.5", "architecture": "arm64", "package": "okou-zero",
            "filename": "okou-zero_0.1.5_arm64.deb", "sha256": hashlib.sha256(payload).hexdigest(), "size": len(payload)}
    data.update(changes)
    return data


def encoded(data):
    return json.dumps(data, separators=(",", ":")).encode()


def framed(raw, signature, payload):
    return struct.pack(">II", len(raw), len(signature)) + raw + signature + payload


class SchemaTests(unittest.TestCase):
    def test_valid(self):
        self.assertEqual(sec.manifest(encoded(metadata()), "arm64"), metadata())

    def test_reject_invalid_schema(self):
        changes = [{"schema": True}, {"schema": 2}, {"schema": 1.0}, {"size": True}, {"size": 0},
                   {"size": -1}, {"size": sec.PACKAGE_LIMIT + 1}, {"size": "1"}, {"size": 1.5},
                   {"version": "0.1.5-rc1"}, {"version": "0.1.5+meta"}, {"version": "01.1.5"},
                   {"version": "1.2"}, {"version": "1:0.1.5"}, {"version": "0.1.5\nREADY"},
                   {"version": "9" * 65 + ".1.1"}, {"version": None}, {"package": "evil"},
                   {"architecture": "amd64"}, {"filename": "../evil.deb"}, {"sha256": "0" * 63},
                   {"sha256": "z" * 64}, {"sha256": []}, {"extra": "field"}]
        for change in changes:
            with self.subTest(change=change), self.assertRaises(sec.InvalidUpdate):
                sec.manifest(encoded(metadata(**change)), "arm64")
        data = metadata()
        del data["package"]
        with self.assertRaises(sec.InvalidUpdate):
            sec.manifest(encoded(data), "arm64")

    def test_duplicate_keys_invalid_json_and_limits(self):
        for raw in (b'{"schema":1,"schema":1}', b'[]', b'null', b'NaN', b'\xff',
                    b'{}' + b' ' * sec.MANIFEST_LIMIT, b'[' * 2000, b'{}trailing'):
            with self.subTest(raw=raw[:40]), self.assertRaises(sec.InvalidUpdate):
                sec.manifest(raw, "arm64")

    def test_header_bounds_truncation(self):
        for mlen, slen in ((0, 1), (1, 0), (sec.MANIFEST_LIMIT + 1, 1), (1, 1025), (2**32 - 1, 1)):
            with self.subTest(lengths=(mlen, slen)), self.assertRaises(sec.InvalidUpdate):
                sec.read_header(io.BytesIO(struct.pack(">II", mlen, slen)))
        for raw in (b'', b'1234567', struct.pack(">II", 5, 5) + b'a' * 9):
            with self.assertRaises(sec.InvalidUpdate):
                sec.read_header(io.BytesIO(raw))
        self.assertEqual(sec.read_header(io.BytesIO(framed(b"m", b"s", b"p"))), (b"m", b"s"))

    def test_chunked_stream(self):
        class Fragmented(io.BytesIO):
            def read(self, count=-1):
                return super().read(min(count, 2))
        self.assertEqual(sec.read_exact(Fragmented(b"hello"), 5), b"hello")

    def test_package_hash_size_and_trailing(self):
        data = metadata(b"hello")
        output = io.BytesIO()
        sec.copy_package(io.BytesIO(b"hello"), output, data)
        self.assertEqual(output.getvalue(), b"hello")
        for payload in (b"hellx", b"hell", b"hello!", b""):
            with self.subTest(payload=payload), self.assertRaises(sec.InvalidUpdate):
                sec.copy_package(io.BytesIO(payload), io.BytesIO(), data)

    def test_upgrade_uses_dpkg(self):
        for installed in ("0.1.5", "0.1.6"):
            with self.assertRaises(subprocess.CalledProcessError):
                sec.validate_upgrade(metadata(), installed, "arm64")
        sec.validate_upgrade(metadata(), "0.1.4", "arm64")
        with self.assertRaises(sec.InvalidUpdate):
            sec.validate_upgrade(metadata(), "0.1.4", "amd64")
        with self.assertRaises(sec.InvalidUpdate):
            sec.validate_upgrade(metadata(), "--root=/tmp", "arm64")

    def test_metadata_exact_match(self):
        with patch.object(sec, "run", return_value=b"okou-zero\narm64\n0.1.5\n") as run:
            sec.validate_package("/root/package.deb", metadata(), "arm64")
            self.assertEqual(run.call_args.args[0][-1], "/root/package.deb")
        for out in (b"other\narm64\n0.1.5\n", b"okou-zero\namd64\n0.1.5\n",
                    b"okou-zero\narm64\n0.1.4\n", b"okou-zero\narm64\n0.1.5\nextra\n"):
            with patch.object(sec, "run", return_value=out), self.assertRaises(sec.InvalidUpdate):
                sec.validate_package("/root/package.deb", metadata(), "arm64")

    def test_installed_state(self):
        with patch.object(sec, "run", return_value=b"install ok installed\n0.1.4\n"):
            self.assertEqual(sec.installed_version(), "0.1.4")
        for out in (b"deinstall ok config-files\n0.1.4\n", b"install ok unpacked\n0.1.4\n", b"install ok installed\n1:0.1.4\n"):
            with patch.object(sec, "run", return_value=out), self.assertRaises(sec.InvalidUpdate):
                sec.installed_version()


class NetworkTests(unittest.TestCase):
    def test_url_allowlist(self):
        for host in client.HOSTS:
            self.assertEqual(client.validate_url("https://" + host + "/asset?token=abc"), "https://" + host + "/asset?token=abc")
        for url in ("http://github.com/x", "https://evil.test/x", "https://github.com.evil.test/x",
                    "https://user@github.com/x", "https://github.com:444/x", "file:///etc/passwd",
                    "https://github.com/#fragment", "https://github.com\\@evil.test/x",
                    "https://github.com/\nx", "https://github.com /x", "https://127.0.0.1/x"):
            with self.subTest(url=url), self.assertRaises(ValueError):
                client.validate_url(url)

    def test_redirect_validation_and_cap(self):
        redirects = client.Redirects()
        req = urllib.request.Request("https://github.com/asset")
        with self.assertRaises(ValueError):
            redirects.redirect_request(req, None, 302, "", {}, "https://evil.test/asset")
        redirects = client.Redirects()
        for _ in range(5):
            result = redirects.redirect_request(req, None, 302, "", {}, "https://release-assets.githubusercontent.com/asset")
            self.assertEqual(result.full_url, "https://release-assets.githubusercontent.com/asset")
        with self.assertRaises(ValueError):
            redirects.redirect_request(req, None, 302, "", {}, "https://github.com/x")

    def fetch(self, payload, limit, headers=None, status=200):
        response = MagicMock()
        response.__enter__.return_value = response
        response.geturl.return_value = "https://github.com/asset"
        response.status = status
        response.headers = headers or {}
        response.read.side_effect = io.BytesIO(payload).read
        opener = MagicMock()
        opener.open.return_value = response
        with patch.object(client.urllib.request, "build_opener", return_value=opener):
            return client.HTTP(time.monotonic() + 30).fetch("https://github.com/asset", limit)

    def test_bounded_http_reads(self):
        self.assertEqual(self.fetch(b"abc", 3, {"Content-Length": "3"}), b"abc")
        for payload, limit, headers in ((b"abcd", 3, {}), (b"abc", 3, {"Content-Length": "4"}),
                                        (b"abc", 4, {"Content-Length": "4"}),
                                        (b"abc", 3, {"Content-Length": "-1"}),
                                        (b"abc", 3, {"Content-Encoding": "gzip"})):
            with self.subTest(headers=headers), self.assertRaises(ValueError):
                self.fetch(payload, limit, headers)
        with self.assertRaises(ValueError):
            self.fetch(b"", 10, status=404)
        with self.assertRaises(TimeoutError):
            client.HTTP(time.monotonic() - 1).fetch("https://github.com/asset", 10)

    def test_total_deadline_expires_during_read(self):
        response = MagicMock()
        response.__enter__.return_value = response
        response.geturl.return_value = "https://github.com/asset"
        response.status = 200
        response.headers = {}
        response.read.return_value = b"a"
        opener = MagicMock()
        opener.open.return_value = response
        with patch.object(client.urllib.request, "build_opener", return_value=opener), \
                patch.object(client.time, "monotonic", side_effect=[0, 1, 11]), self.assertRaises(TimeoutError):
            client.HTTP(10).fetch("https://github.com/asset", 100)
        self.assertEqual(response.read.call_count, 1)

    def test_stable_release_and_duplicate_assets(self):
        for data in ({"draft": False, "prerelease": True, "assets": []},
                     {"draft": True, "prerelease": False, "assets": []},
                     {"assets": []},
                     {"draft": False, "prerelease": False, "assets": [{"name": "x"}, {"name": "x"}]}):
            with self.assertRaises(ValueError):
                client.release_assets(encoded(data), sec)


@unittest.skipUnless(shutil.which("openssl"), "openssl required for real signature fixtures")
class SignedFixtures(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.keys = tempfile.TemporaryDirectory(prefix="okou-updater-test-keys-")
        cls.private = str(Path(cls.keys.name) / "private.pem")
        cls.public = str(Path(cls.keys.name) / "public.pem")
        subprocess.run(["/usr/bin/openssl", "genpkey", "-algorithm", "RSA", "-pkeyopt", "rsa_keygen_bits:2048", "-out", cls.private],
                       check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        subprocess.run(["/usr/bin/openssl", "pkey", "-in", cls.private, "-pubout", "-out", cls.public],
                       check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    @classmethod
    def tearDownClass(cls):
        cls.keys.cleanup()

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="okou-updater-test-")
        self.addCleanup(self.temp.cleanup)
        self.cache = Path(self.temp.name) / "cache"
        self.cache.mkdir(mode=0o700)
        self.payload = b"fixture Debian bytes"
        self.raw = encoded(metadata(self.payload))
        self.signature = self.sign(self.raw)
        self.key_patch = patch.object(sec, "KEY_PATH", self.public)
        self.key_patch.start()
        self.addCleanup(self.key_patch.stop)

    def sign(self, raw):
        return subprocess.run(["/usr/bin/openssl", "dgst", "-sha256", "-sign", self.private],
                              input=raw, capture_output=True, check=True).stdout

    def test_real_signature_and_tampering(self):
        sec.verify_signature(self.raw, self.signature, self.temp.name)
        for raw, sig in ((self.raw.replace(b"0.1.5", b"0.1.6"), self.signature),
                         (self.raw, bytes([self.signature[0] ^ 1]) + self.signature[1:]),
                         (self.raw, b""), (self.raw, b"x" * 1025)):
            with self.assertRaises((sec.InvalidUpdate, subprocess.CalledProcessError)):
                sec.verify_signature(raw, sig, self.temp.name)

    def http(self, raw=None, sig=None, payload=None):
        raw = self.raw if raw is None else raw
        sig = self.signature if sig is None else sig
        payload = self.payload if payload is None else payload
        assets = [{"name": name, "browser_download_url": "https://github.com/" + name}
                  for name in ("manifest.json", "manifest.sig", "okou-zero_0.1.5_arm64.deb")]
        contents = {client.LATEST: encoded({"draft": False, "prerelease": False, "assets": assets}),
                    "https://github.com/manifest.json": raw, "https://github.com/manifest.sig": sig,
                    "https://github.com/okou-zero_0.1.5_arm64.deb": payload}
        http = MagicMock()
        def fetch(url, limit, target=None):
            data = contents[url]
            if len(data) > limit:
                raise ValueError()
            if target is not None:
                target.write(data)
                return len(data)
            return data
        http.fetch.side_effect = fetch
        return http

    def run_check(self, current="0.1.4", **kwargs):
        with patch.object(client, "cache_directory", return_value=self.cache), patch.object(sec, "architecture", return_value="arm64"):
            client.check(current, sec, self.http(**kwargs))

    def test_check_atomic_ready_and_current(self):
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            self.run_check()
        self.assertEqual(output.getvalue(), "CHECKING\nDOWNLOADING\nREADY 0.1.5\n")
        self.assertEqual((self.cache / "ready").read_bytes(), framed(self.raw, self.signature, self.payload))
        self.assertEqual((self.cache / "ready").stat().st_mode & 0o777, 0o600)
        with contextlib.redirect_stdout(io.StringIO()) as output:
            self.run_check(current="0.1.5")
        self.assertEqual(output.getvalue(), "CHECKING\nCURRENT\n")

    def test_cache_reuse_verifies_payload_and_skips_download(self):
        ready = self.cache / "ready"
        ready.write_bytes(framed(self.raw, self.signature, self.payload))
        http = self.http()
        with patch.object(client, "cache_directory", return_value=self.cache), \
                patch.object(sec, "architecture", return_value="arm64"), \
                contextlib.redirect_stdout(io.StringIO()) as output:
            client.check("0.1.4", sec, http)
        self.assertEqual(output.getvalue(), "CHECKING\nREADY 0.1.5\n")
        self.assertEqual(http.fetch.call_count, 3)
        ready.write_bytes(framed(self.raw, self.signature, b"x" * len(self.payload)))
        with contextlib.redirect_stdout(io.StringIO()) as output:
            self.run_check()
        self.assertIn("DOWNLOADING\n", output.getvalue())
        self.assertEqual(ready.read_bytes(), framed(self.raw, self.signature, self.payload))

    def test_network_failure_preserves_valid_ready(self):
        bundle = framed(self.raw, self.signature, self.payload)
        (self.cache / "ready").write_bytes(bundle)
        http = MagicMock()
        http.fetch.side_effect = TimeoutError()
        with patch.object(client, "cache_directory", return_value=self.cache), \
                patch.object(sec, "architecture", return_value="arm64"), \
                contextlib.redirect_stdout(io.StringIO()), self.assertRaises(TimeoutError):
            client.check("0.1.4", sec, http)
        self.assertEqual((self.cache / "ready").read_bytes(), bundle)

    def test_failed_check_never_replaces_ready(self):
        ready = self.cache / "ready"
        ready.write_bytes(b"previous")
        bad_raw = encoded(metadata(self.payload, architecture="amd64"))
        for kwargs in ({"payload": b"tampered Debian bytes"}, {"sig": b"bad"},
                       {"raw": self.raw + b" "}, {"raw": bad_raw, "sig": self.sign(bad_raw)}):
            with self.subTest(kwargs=list(kwargs)), contextlib.redirect_stdout(io.StringIO()), self.assertRaises(Exception):
                self.run_check(**kwargs)
            self.assertEqual(ready.read_bytes(), b"previous")

    def install(self, bundle, install_mock=None):
        # Signature/hash/framing/upgrade checks are real. Only OS identity,
        # lock ownership and dpkg package inspection/install are mocked.
        real_run = subprocess.run
        real_fstat = os.fstat
        def root_stat(fd):
            fields = list(real_fstat(fd))
            fields[4] = 0
            return os.stat_result(fields)
        def process(args, **kwargs):
            if args[:2] == ["/usr/bin/dpkg", "--install"]:
                if install_mock is not None:
                    return install_mock(args, **kwargs)
                return subprocess.CompletedProcess(args, 0)
            return real_run(args, **kwargs)
        with patch.object(helper, "LOCK_PATH", str(Path(self.temp.name) / "lock")), \
                patch.object(helper.os, "fstat", side_effect=root_stat), \
                patch.object(helper.signal, "alarm"), \
                patch.object(sec, "architecture", return_value="arm64"), \
                patch.object(sec, "installed_version", return_value="0.1.4"), \
                patch.object(sec, "validate_package") as validate, \
                patch.object(helper.subprocess, "run", side_effect=process):
            helper.install(io.BytesIO(bundle), sec)
            return validate

    def test_root_verifies_bundle_and_install_command(self):
        called = []
        def installed(args, **kwargs):
            path = Path(args[-1])
            self.assertEqual(path.read_bytes(), self.payload)
            self.assertEqual(path.parent.stat().st_mode & 0o777, 0o700)
            self.assertEqual(path.stat().st_mode & 0o777, 0o600)
            self.assertEqual(kwargs["env"], sec.ENV)
            self.assertEqual(args[:2], ["/usr/bin/dpkg", "--install"])
            self.assertEqual(len(args), 3)
            called.append(path)
            return subprocess.CompletedProcess(args, 0)
        validate = self.install(framed(self.raw, self.signature, self.payload), installed)
        self.assertEqual(validate.call_count, 1)
        self.assertEqual(len(called), 1)
        self.assertFalse(called[0].exists())

    def test_root_rejects_tampering_and_unsigned_schema(self):
        bad = encoded(metadata(self.payload, filename="../../tmp/evil.deb"))
        old = encoded(metadata(self.payload, version="0.1.4", filename="okou-zero_0.1.4_arm64.deb"))
        downgrade = encoded(metadata(self.payload, version="0.1.3", filename="okou-zero_0.1.3_arm64.deb"))
        for bundle in (framed(self.raw, self.signature, self.payload + b"x"),
                       framed(self.raw, self.signature, self.payload[:-1]),
                       framed(self.raw, self.signature, b"x" * len(self.payload)),
                       framed(self.raw + b" ", self.signature, self.payload),
                       framed(self.raw, b"bad", self.payload),
                       framed(bad, self.sign(bad), self.payload),
                       framed(old, self.sign(old), self.payload),
                       framed(downgrade, self.sign(downgrade), self.payload)):
            with self.subTest(bundle=bundle[:20]), self.assertRaises(Exception):
                self.install(bundle, lambda *a, **kw: self.fail("must not install"))


class EntrypointTests(unittest.TestCase):
    def test_helper_rejects_arguments_and_nonroot(self):
        for args, uid, euid in ((["helper", "--help"], 0, 0), (["helper"], 1000, 0), (["helper"], 0, 1000)):
            with patch.object(helper.sys, "argv", args), patch.object(helper.os, "getuid", return_value=uid), \
                    patch.object(helper.os, "geteuid", return_value=euid), patch.object(helper, "install") as install:
                self.assertEqual(helper.main(), 1)
                install.assert_not_called()

    def test_client_protocol_errors(self):
        for args in ([], ["--help"], ["check", "--current", "0.1.4\nINJECTED"], ["wat", "--current", "0.1.4"]):
            with patch.object(client, "load_security", return_value=sec), contextlib.redirect_stdout(io.StringIO()) as output:
                self.assertEqual(client.main(args), 1)
            self.assertEqual(output.getvalue(), "ERROR\n")

    def test_main_enforces_total_alarm(self):
        with patch.object(client, "load_security", return_value=sec), \
                patch.object(client.signal, "signal") as handler, patch.object(client.signal, "alarm") as alarm, \
                patch.object(client, "check", side_effect=TimeoutError()), \
                contextlib.redirect_stdout(io.StringIO()) as output:
            self.assertEqual(client.main(["check", "--current", "0.1.4"]), 1)
        self.assertEqual([call.args for call in alarm.call_args_list], [(client.TOTAL_TIMEOUT,), (0,)])
        handler.assert_called_once_with(client.signal.SIGALRM, client.expired)
        self.assertEqual(output.getvalue(), "ERROR\n")
        with self.assertRaises(TimeoutError):
            client.expired(None, None)

    def test_apply_fixed_command_and_exit_status(self):
        with tempfile.TemporaryDirectory() as directory:
            cache = Path(directory)
            (cache / "ready").write_bytes(b"untrusted cache")
            with patch.object(client, "cache_directory", return_value=cache), \
                    patch.object(client.subprocess, "run") as run, contextlib.redirect_stdout(io.StringIO()) as output:
                client.apply("0.1.4", sec)
                args, kwargs = run.call_args
                self.assertEqual(args[0], ["/usr/bin/sudo", "-n", "/usr/libexec/okou-zero-update-install"])
                self.assertEqual(kwargs["env"], sec.ENV)
                self.assertEqual(output.getvalue(), "INSTALLED\n")
            with patch.object(client, "cache_directory", return_value=cache), \
                    patch.object(client, "load_security", return_value=sec), \
                    patch.object(client.subprocess, "run", side_effect=subprocess.CalledProcessError(1, "sudo")), \
                    contextlib.redirect_stdout(io.StringIO()) as output:
                self.assertEqual(client.main(["apply", "--current", "0.1.4"]), 1)
                self.assertEqual(output.getvalue(), "ERROR\n")

    def test_apply_rejects_symlink_fifo_and_oversize(self):
        with tempfile.TemporaryDirectory() as directory:
            cache = Path(directory)
            ready = cache / "ready"
            ready.symlink_to("/etc/passwd")
            with patch.object(client, "cache_directory", return_value=cache), self.assertRaises(OSError):
                client.apply("0.1.4", sec)
            ready.unlink()
            os.mkfifo(ready)
            with patch.object(client, "cache_directory", return_value=cache), self.assertRaises(ValueError):
                client.apply("0.1.4", sec)
            ready.unlink()
            with ready.open("wb") as stream:
                stream.truncate(sec.PACKAGE_LIMIT + sec.MANIFEST_LIMIT + sec.SIGNATURE_LIMIT + 9)
            with patch.object(client, "cache_directory", return_value=cache), self.assertRaises(ValueError):
                client.apply("0.1.4", sec)

    def test_cache_mode_and_symlink(self):
        with tempfile.TemporaryDirectory() as home:
            with patch.object(client.Path, "home", return_value=Path(home)):
                cache = client.cache_directory()
                self.assertEqual(cache.stat().st_mode & 0o777, 0o700)
                cache.rmdir()
                cache.symlink_to(Path(home))
                with self.assertRaises(ValueError):
                    client.cache_directory()


if __name__ == "__main__":
    unittest.main()
