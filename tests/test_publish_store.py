import hashlib
import importlib.util
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from unittest.mock import patch

SCRIPT = Path(__file__).resolve().parents[1] / "scripts" / "publish_store.py"
SPEC = importlib.util.spec_from_file_location("publish_store", SCRIPT)
publish_store = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(publish_store)


class PublishStoreTest(unittest.TestCase):
    def test_build_store_manifest_and_pr_body_with_changelog(self):
        sha = hashlib.sha256(b"pkg").hexdigest()
        manifest = publish_store.build_store_manifest(
            "0.3.0", "okou_0.3.0_arm64.deb", sha, 1024
        )
        self.assertEqual(
            manifest,
            {
                "filename": "okou_0.3.0_arm64.deb",
                "url": "https://github.com/e7h4n/okou-zero/releases/download/v0.3.0/okou_0.3.0_arm64.deb",
                "sha256": sha,
                "size": 1024,
                "package": "okou",
                "version": "0.3.0",
                "architecture": "arm64",
                "uploaded_by": "e7h4n",
                "uploader_email": "813596+e7h4n@users.noreply.github.com",
            },
        )
        body = publish_store.format_pr_body(
            "0.3.0", "okou_0.3.0_arm64.deb", sha, "### Features\n\n* add cool feature\n"
        )
        self.assertIn("https://github.com/e7h4n/okou-zero/releases/tag/v0.3.0", body)
        self.assertIn("### Changelog", body)
        self.assertIn("* add cool feature", body)

        empty_body = publish_store.format_pr_body("0.3.0", "okou_0.3.0_arm64.deb", sha, "   ")
        self.assertNotIn("### Changelog", empty_body)

    @unittest.skipUnless(shutil.which("dpkg"), "dpkg required for version comparison")
    def test_update_store_worktree_sparse_replaces_old_manifest_only(self):
        with tempfile.TemporaryDirectory() as directory:
            tmp = Path(directory)
            upstream = tmp / "upstream.git"
            seed = tmp / "seed"
            checkout = tmp / "checkout"

            def git(cwd, *args):
                return subprocess.run(
                    ["git", *args],
                    cwd=cwd,
                    check=True,
                    capture_output=True,
                    text=True,
                ).stdout.strip()

            git(tmp, "init", "--bare", "-b", "main", str(upstream))
            git(tmp, "init", "-b", "main", str(seed))
            git(seed, "config", "user.name", "Fixture")
            git(seed, "config", "user.email", "fixture@example.invalid")

            okou_dir = seed / "pool" / "main" / "okou"
            other_dir = seed / "pool" / "main" / "other"
            okou_dir.mkdir(parents=True)
            other_dir.mkdir(parents=True)
            (okou_dir / "meta.json").write_text('{"title":"Okou"}\n', encoding="utf-8")
            (okou_dir / "okou_0.2.33_arm64.deb.release.json").write_text(
                json.dumps({"version": "0.2.33", "uploaded_by": "e7h4n"}) + "\n",
                encoding="utf-8",
            )
            (other_dir / "meta.json").write_text('{"title":"Other"}\n', encoding="utf-8")
            git(seed, "add", ".")
            git(seed, "commit", "-m", "initial")
            git(seed, "remote", "add", "origin", str(upstream))
            git(seed, "push", "-u", "origin", "main")

            git(
                tmp,
                "clone",
                "--sparse",
                "--single-branch",
                "--branch",
                "main",
                str(upstream),
                str(checkout),
            )
            git(checkout, "sparse-checkout", "set", "pool/main/okou")

            new_manifest = publish_store.build_store_manifest(
                "0.3.0", "okou_0.3.0_arm64.deb", "a" * 64, 2048
            )
            self.assertEqual(
                publish_store.update_store_worktree(checkout, "0.3.0", new_manifest),
                "committed",
            )
            changed = git(checkout, "diff", "--no-renames", "--name-status", "HEAD~1..HEAD").splitlines()
            self.assertEqual(
                sorted(changed),
                [
                    "A\tpool/main/okou/okou_0.3.0_arm64.deb.release.json",
                    "D\tpool/main/okou/okou_0.2.33_arm64.deb.release.json",
                ],
            )
            self.assertEqual(git(checkout, "log", "-1", "--format=%s"), "publish: okou 0.3.0")
            self.assertEqual(
                git(checkout, "log", "-1", "--format=%an <%ae>"),
                "e7h4n <813596+e7h4n@users.noreply.github.com>",
            )
            self.assertTrue((checkout / "pool" / "main" / "okou" / "meta.json").is_file())

            # Re-running for the exact same version is idempotent.
            self.assertEqual(
                publish_store.update_store_worktree(checkout, "0.3.0", new_manifest),
                "already_published",
            )

            # Older versions are rejected.
            older_manifest = publish_store.build_store_manifest(
                "0.2.34", "okou_0.2.34_arm64.deb", "b" * 64, 2048
            )
            with self.assertRaisesRegex(ValueError, "not newer"):
                publish_store.update_store_worktree(checkout, "0.2.34", older_manifest)

    def test_validate_package_rejects_mismatch(self):
        with tempfile.TemporaryDirectory() as directory:
            package = Path(directory) / "okou_0.3.0_arm64.deb"
            package.write_bytes(b"deb")
            fields = {"Package": "okou", "Version": "0.2.34", "Architecture": "arm64"}
            with patch.object(publish_store, "package_field", side_effect=lambda _, key: fields[key]):
                with self.assertRaisesRegex(ValueError, "Package Version"):
                    publish_store.validate_package("v0.3.0", package)


if __name__ == "__main__":
    unittest.main()
