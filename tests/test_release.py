import importlib.util
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

SCRIPT = Path(__file__).resolve().parents[1] / "scripts" / "release.py"
SPEC = importlib.util.spec_from_file_location("release", SCRIPT)
release = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(release)


class ReleaseVersionTest(unittest.TestCase):
    def test_increment_latest_main_release_and_reuse_exact_tag(self):
        with tempfile.TemporaryDirectory() as directory, patch.object(
            release, "ROOT", Path(directory)
        ):
            def git(*args):
                return subprocess.run(
                    ["git", *args], cwd=directory, check=True,
                    capture_output=True, text=True,
                ).stdout.strip()

            git("init", "-b", "main")
            git("config", "user.name", "Release Fixture")
            git("config", "user.email", "fixture@example.invalid")
            git("commit", "--allow-empty", "-m", "initial release")
            git("tag", "-a", "v0.2.5", "-m", "stable")
            self.assertEqual(release.next_version(), "0.2.5")

            git("switch", "-c", "other")
            git("commit", "--allow-empty", "-m", "side branch")
            git("tag", "v0.2.9")
            git("switch", "main")
            git("merge", "--no-ff", "other", "-m", "merge pull request")
            self.assertEqual(release.next_version(), "0.2.6")
            git("tag", "v0.2.6")
            self.assertEqual(release.next_version(), "0.2.6")

    def test_package_metadata_must_match_generated_version(self):
        with tempfile.TemporaryDirectory() as directory:
            package = Path(directory) / "okou-zero_0.2.6_arm64.deb"
            package.touch()
            fields = {"Package": "okou-zero", "Version": "0.2.5", "Architecture": "arm64"}
            with patch.object(release, "package_field", side_effect=lambda _, key: fields[key]):
                with self.assertRaisesRegex(ValueError, "Package Version"):
                    release.validate_package("v0.2.6", package)
