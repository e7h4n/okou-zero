import hashlib
import importlib.util
import io
import json
import os
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

    def test_sanitize_text_redacts_secrets_tokens_and_url_credentials(self):
        fake_pat = "gh" + "p_" + "A" * 30
        fake_fine = "github_" + "pat_" + "B" * 30
        raw = (
            f"remote: https://x-access-token:{fake_pat}@github.com/e7h4n/packages.git\n"
            f"Authorization: Bearer {fake_fine}\n"
            "fatal: custom-secret-value rejected\n"
        )
        sanitized = publish_store.sanitize_text(raw, secrets=("custom-secret-value",))
        self.assertNotIn(fake_pat, sanitized)
        self.assertNotIn(fake_fine, sanitized)
        self.assertNotIn("custom-secret-value", sanitized)
        self.assertIn("https://[REDACTED]@github.com/e7h4n/packages.git", sanitized)
        self.assertIn("Authorization: Bearer [REDACTED]", sanitized)
        self.assertIn("fatal: [REDACTED] rejected", sanitized)

    def test_validate_repo_slug_and_branch_reject_unsafe_values(self):
        self.assertEqual(publish_store.validate_repo_slug("e7h4n/packages", "Fork"), "e7h4n/packages")
        self.assertEqual(publish_store.validate_branch_name("main"), "main")
        for bad_repo in ("../other/repo", "e7h4n/packages?foo=1", "just-one-part", "a/b/c"):
            with self.assertRaises(ValueError):
                publish_store.validate_repo_slug(bad_repo, "Fork")
        for bad_branch in ("../main", "/main", "main/", "main branch", ""):
            with self.assertRaises(ValueError):
                publish_store.validate_branch_name(bad_branch)

    def test_sync_fork_branch_success_and_already_up_to_date(self):
        ff_result = subprocess.CompletedProcess(
            args=["gh"],
            returncode=0,
            stdout=json.dumps({
                "message": "Successfully fetched and fast-forwarded from upstream CardputerZero:main.",
                "merge_type": "fast-forward",
                "base_branch": "CardputerZero:main",
            }),
            stderr="",
        )
        noop_result = subprocess.CompletedProcess(
            args=["gh"],
            returncode=0,
            stdout=json.dumps({
                "message": "The branch is not behind the upstream CardputerZero:main.",
                "merge_type": "none",
                "base_branch": "CardputerZero:main",
            }),
            stderr="",
        )
        with patch.object(publish_store, "run", side_effect=[ff_result, noop_result]) as mock_run:
            buf = io.StringIO()
            with patch("sys.stdout", buf):
                first = publish_store.sync_fork_branch("e7h4n/packages", "main", "tok-123")
                second = publish_store.sync_fork_branch("e7h4n/packages", "main", "tok-123")

        self.assertEqual(first["status"], "synced")
        self.assertEqual(first["merge_type"], "fast-forward")
        self.assertEqual(second["status"], "synced")
        self.assertEqual(second["merge_type"], "none")
        self.assertIn("Synced e7h4n/packages:main with CardputerZero:main (fast-forward)", buf.getvalue())
        self.assertIn("already up to date", buf.getvalue())
        self.assertEqual(
            mock_run.call_args_list[0].args[0],
            ["gh", "api", "--method", "POST", "repos/e7h4n/packages/merge-upstream", "-f", "branch=main"],
        )
        self.assertEqual(mock_run.call_args_list[0].kwargs["env_extra"], {"GH_TOKEN": "tok-123"})

    def test_sync_fork_branch_workflow_scope_error_gives_actionable_hint(self):
        token = "secret-store-token"
        api_err = subprocess.CompletedProcess(
            args=["gh"],
            returncode=1,
            stdout=json.dumps({
                "message": (
                    "refusing to allow a Personal Access Token to create or update workflow "
                    "`.github/workflows/validate-pr.yml` without `workflow` scope"
                ),
                "status": "422",
            }),
            stderr=f"gh: HTTP 422 using {token}\n",
        )
        with patch.object(publish_store, "run", return_value=api_err):
            with self.assertRaises(publish_store.StorePublishError) as ctx:
                publish_store.sync_fork_branch("e7h4n/packages", "main", token)
        error = ctx.exception
        self.assertEqual(error.step, "sync fork e7h4n/packages:main")
        self.assertIn("HTTP 422", error.detail)
        self.assertIn("workflow", error.detail)
        self.assertNotIn(token, error.detail)
        self.assertIsNotNone(error.hint)
        self.assertIn("`workflow` scope", error.hint)
        self.assertIn("https://github.com/e7h4n/packages", error.hint)

    def test_sync_fork_branch_conflict_warns_and_continues(self):
        conflict_err = subprocess.CompletedProcess(
            args=["gh"],
            returncode=1,
            stdout=json.dumps({
                "message": "Merge conflict.",
                "status": "409",
            }),
            stderr="gh: Merge conflict. (HTTP 409)\n",
        )
        stderr_buf = io.StringIO()
        with patch.object(publish_store, "run", return_value=conflict_err), patch.dict(
            os.environ, {"GITHUB_ACTIONS": ""}, clear=False
        ), patch("sys.stderr", stderr_buf):
            outcome = publish_store.sync_fork_branch("e7h4n/packages", "main", "tok-123")
        self.assertEqual(outcome["status"], "conflict")
        self.assertIn("HTTP 409", stderr_buf.getvalue())

        stdout_buf = io.StringIO()
        with patch.object(publish_store, "run", return_value=conflict_err), patch.dict(
            os.environ, {"GITHUB_ACTIONS": "true"}, clear=False
        ), patch("sys.stdout", stdout_buf):
            outcome_gha = publish_store.sync_fork_branch("e7h4n/packages", "main", "tok-123")
        self.assertEqual(outcome_gha["status"], "conflict")
        self.assertIn("::warning::", stdout_buf.getvalue())

    def test_sync_fork_branch_generic_api_failure_sanitizes_secret(self):
        token = "secret-api-tok"
        err_result = subprocess.CompletedProcess(
            args=["gh"],
            returncode=1,
            stdout=json.dumps({"message": "Resource not accessible by integration", "status": "403"}),
            stderr=f"gh: Resource not accessible using {token} (HTTP 403)\n",
        )
        with patch.object(publish_store, "run", return_value=err_result):
            with self.assertRaises(publish_store.StorePublishError) as ctx:
                publish_store.sync_fork_branch("e7h4n/packages", "main", token)
        self.assertEqual(ctx.exception.step, "sync fork e7h4n/packages:main")
        self.assertIn("HTTP 403", ctx.exception.detail)
        self.assertNotIn(token, ctx.exception.detail)
        self.assertIsNone(ctx.exception.hint)

    def test_push_branch_with_token_reports_sanitized_workflow_scope_error(self):
        token = "push-secret-xyz"
        failed_push = subprocess.CompletedProcess(
            args=["git", "push"],
            returncode=1,
            stdout="",
            stderr=(
                f"To https://x-access-token:{token}@github.com/e7h4n/packages.git\n"
                " ! [remote rejected] HEAD -> publish/okou-0.3.1 "
                "(refusing to allow a Personal Access Token to create or update workflow "
                "`.github/workflows/validate-pr.yml` without `workflow` scope)\n"
            ),
        )
        with tempfile.TemporaryDirectory() as repo_dir, patch.object(
            publish_store, "run", return_value=failed_push
        ):
            with self.assertRaises(publish_store.StorePublishError) as ctx:
                publish_store.push_branch_with_token(
                    Path(repo_dir),
                    "https://github.com/e7h4n/packages.git",
                    "publish/okou-0.3.1",
                    token,
                    fork_repo="e7h4n/packages",
                )
        error = ctx.exception
        self.assertIn("push branch publish/okou-0.3.1 to e7h4n/packages", error.step)
        self.assertIn("remote rejected", error.detail)
        self.assertNotIn(token, error.detail)
        self.assertIn("https://[REDACTED]@github.com/e7h4n/packages.git", error.detail)
        self.assertIsNotNone(error.hint)
        self.assertIn("`workflow` scope", error.hint)

    def test_main_sync_fork_only_mode_and_failure_summary(self):
        with patch.dict(os.environ, {"CARDPUTER_PACKAGES_TOKEN": "tok-abc"}, clear=False), patch.object(
            publish_store, "verify_token_owner"
        ) as mock_verify, patch.object(
            publish_store, "sync_fork_branch", return_value={"status": "synced", "merge_type": "none"}
        ) as mock_sync:
            rc = publish_store.main(["--sync-fork-only"])
        self.assertEqual(rc, 0)
        mock_verify.assert_called_once_with("tok-abc")
        mock_sync.assert_called_once_with("e7h4n/packages", "main", "tok-abc")

        with tempfile.TemporaryDirectory() as tmp:
            summary_file = Path(tmp) / "summary.md"
            err = publish_store.StorePublishError(
                "sync fork e7h4n/packages:main",
                "HTTP 422: refusing without workflow scope",
                hint="Add workflow scope.",
            )
            stderr_buf = io.StringIO()
            with patch.dict(
                os.environ,
                {"CARDPUTER_PACKAGES_TOKEN": "tok-abc", "GITHUB_STEP_SUMMARY": str(summary_file)},
                clear=False,
            ), patch.object(publish_store, "verify_token_owner"), patch.object(
                publish_store, "sync_fork_branch", side_effect=err
            ), patch("sys.stderr", stderr_buf):
                rc = publish_store.main(["--sync-fork-only"])
            self.assertEqual(rc, 1)
            self.assertIn("Store publish failed during sync fork e7h4n/packages:main", stderr_buf.getvalue())
            self.assertIn("Hint: Add workflow scope.", stderr_buf.getvalue())
            summary_text = summary_file.read_text(encoding="utf-8")
            self.assertIn("CardputerZero Store Publish Failed", summary_text)
            self.assertIn("Add workflow scope.", summary_text)

    def test_main_syncs_fork_by_default_and_respects_skip_fork_sync(self):
        with tempfile.TemporaryDirectory() as tmp:
            pkg = Path(tmp) / "okou_0.3.1_arm64.deb"
            pkg.write_bytes(b"deb-content")
            sha = hashlib.sha256(b"deb-content").hexdigest()
            for extra_args, expect_sync in (([], True), (["--skip-fork-sync"], False)):
                call_order = []
                with patch.dict(
                    os.environ, {"CARDPUTER_PACKAGES_TOKEN": "tok-1"}, clear=False
                ), patch.object(
                    publish_store, "validate_package", return_value=("0.3.1", pkg.name, sha, 11)
                ), patch.object(
                    publish_store, "verify_token_owner"
                ), patch.object(
                    publish_store, "verify_asset_url"
                ), patch.object(
                    publish_store, "fetch_release_notes", return_value="* fix bug"
                ), patch.object(
                    publish_store, "run_step"
                ), patch.object(
                    publish_store, "update_store_worktree", return_value="committed"
                ), patch.object(
                    publish_store,
                    "sync_fork_branch",
                    side_effect=lambda *a, **kw: call_order.append("sync") or {"status": "synced"},
                ) as mock_sync, patch.object(
                    publish_store,
                    "push_branch_with_token",
                    side_effect=lambda *a, **kw: call_order.append("push"),
                ) as mock_push, patch.object(
                    publish_store, "upsert_pull_request", return_value="https://github.com/CardputerZero/packages/pull/9999"
                ), patch("sys.stdout", io.StringIO()):
                    rc = publish_store.main(["--tag", "v0.3.1", "--package", str(pkg), *extra_args])
                self.assertEqual(rc, 0)
                mock_push.assert_called_once()
                if expect_sync:
                    mock_sync.assert_called_once_with("e7h4n/packages", "main", "tok-1")
                    self.assertEqual(call_order, ["sync", "push"])
                else:
                    mock_sync.assert_not_called()
                    self.assertEqual(call_order, ["push"])


if __name__ == "__main__":
    unittest.main()
