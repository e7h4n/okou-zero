import importlib.util
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from unittest.mock import patch

SCRIPT = Path(__file__).resolve().parents[1] / "scripts" / "release.py"
SPEC = importlib.util.spec_from_file_location("release", SCRIPT)
release = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(release)


class ReleaseScriptTest(unittest.TestCase):
    def test_validate_repo_version_requires_manifest_and_cmake_match(self):
        with tempfile.TemporaryDirectory() as directory, patch.object(
            release, "ROOT", Path(directory)
        ):
            root = Path(directory)
            (root / ".release-please-manifest.json").write_text(
                json.dumps({".": "0.2.35"}) + "\n", encoding="utf-8"
            )
            (root / "CMakeLists.txt").write_text(
                'cmake_minimum_required(VERSION 3.24)\n'
                'set(OKOU_ZERO_BUILD_VERSION "0.2.35" CACHE STRING "Package and application version") # x-release-please-version\n',
                encoding="utf-8",
            )
            self.assertEqual(release.validate_repo_version("v0.2.35"), "0.2.35")

            with self.assertRaisesRegex(ValueError, "release-please-manifest"):
                release.validate_repo_version("v0.2.36")

            (root / "CMakeLists.txt").write_text(
                'set(OKOU_ZERO_BUILD_VERSION "0.2.34" CACHE STRING "Package and application version")\n',
                encoding="utf-8",
            )
            with self.assertRaisesRegex(ValueError, "CMakeLists.txt"):
                release.validate_repo_version("v0.2.35")

    def test_package_metadata_must_match_generated_version(self):
        with tempfile.TemporaryDirectory() as directory:
            package = Path(directory) / "okou_0.2.6_arm64.deb"
            package.touch()
            fields = {"Package": "okou", "Version": "0.2.5", "Architecture": "arm64"}
            with patch.object(release, "package_field", side_effect=lambda _, key: fields[key]):
                with self.assertRaisesRegex(ValueError, "Package Version"):
                    release.validate_package("v0.2.6", package)

    def test_inspect_release_handles_draft_published_and_mismatches(self):
        commit = "a" * 40
        draft = {
            "tag_name": "v0.2.35",
            "target_commitish": commit,
            "draft": True,
            "prerelease": False,
            "assets": [],
        }
        self.assertEqual(release.inspect_release(draft, "v0.2.35", commit), "draft")

        published = {
            **draft,
            "draft": False,
            "assets": [
                {"name": "okou_0.2.35_arm64.deb"},
                {"name": "manifest.json"},
                {"name": "manifest.sig"},
                {"name": "SHA256SUMS"},
            ],
        }
        self.assertEqual(release.inspect_release(published, "v0.2.35", commit), "published")

        incomplete = {**draft, "draft": False, "assets": [{"name": "manifest.json"}]}
        with self.assertRaisesRegex(ValueError, "incomplete"):
            release.inspect_release(incomplete, "v0.2.35", commit)

        wrong_commit = {**draft, "target_commitish": "b" * 40}
        with self.assertRaisesRegex(ValueError, "commit"):
            release.inspect_release(wrong_commit, "v0.2.35", commit)

        prerelease = {**draft, "prerelease": True}
        with self.assertRaisesRegex(ValueError, "prerelease"):
            release.inspect_release(prerelease, "v0.2.35", commit)

    def test_upload_and_publish_replaces_stale_draft_assets_and_marks_latest(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            assets = []
            for name, content in (
                ("okou_0.2.35_arm64.deb", b"deb-bytes"),
                ("manifest.json", b"{}\n"),
                ("manifest.sig", b"sig"),
                ("SHA256SUMS", b"sums\n"),
            ):
                p = root / name
                p.write_bytes(content)
                assets.append(p)

            initial_release = {
                "id": 98765,
                "upload_url": "https://uploads.github.com/repos/e7h4n/okou-zero/releases/98765/assets{?name,label}",
                "assets": [{"id": 555, "name": "manifest.json", "size": 1}],
            }
            refreshed_release = {
                "id": 98765,
                "assets": [{"name": a.name, "size": a.stat().st_size} for a in assets],
            }
            with patch.object(release, "run") as mock_run, patch.object(
                release, "fetch_release", return_value=refreshed_release
            ):
                release.upload_and_publish("98765", initial_release, assets)

            commands = [c.args[0] for c in mock_run.call_args_list]
            self.assertEqual(
                commands[0],
                ["gh", "api", "--method", "DELETE", "repos/e7h4n/okou-zero/releases/assets/555"],
            )
            self.assertEqual(
                commands[-1],
                [
                    "gh", "api", "--method", "PATCH",
                    "repos/e7h4n/okou-zero/releases/98765",
                    "-F", "draft=false",
                    "-f", "make_latest=true",
                ],
            )
            self.assertEqual(len(commands), 6)

    @unittest.skipUnless(shutil.which("openssl"), "openssl required for signing fixture")
    def test_prepare_signs_and_verifies_bundle(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "assets").mkdir()
            priv = root / "private.pem"
            pub = root / "assets" / "update-public.pem"
            subprocess.run(
                ["openssl", "genpkey", "-algorithm", "RSA", "-pkeyopt", "rsa_keygen_bits:2048", "-out", str(priv)],
                check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
            )
            subprocess.run(
                ["openssl", "pkey", "-in", str(priv), "-pubout", "-out", str(pub)],
                check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
            )
            package = root / "okou_0.2.35_arm64.deb"
            package.write_bytes(b"fixture-deb")
            fields = {"Package": "okou", "Version": "0.2.35", "Architecture": "arm64"}
            with patch.object(release, "ROOT", root), patch.object(
                release, "package_field", side_effect=lambda _, key: fields[key]
            ):
                version, assets = release.prepare(
                    "v0.2.35", package, root / "out", priv.read_text(encoding="utf-8")
                )
            self.assertEqual(version, "0.2.35")
            self.assertEqual(
                {a.name for a in assets},
                {"okou_0.2.35_arm64.deb", "manifest.json", "manifest.sig", "SHA256SUMS"},
            )


if __name__ == "__main__":
    unittest.main()
