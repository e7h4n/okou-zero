#!/usr/bin/env python3
"""Submit a verified arm64 release manifest PR to CardputerZero/packages.

Syncs e7h4n/packages:main with CardputerZero/packages:main via GitHub's
merge-upstream API, updates only pool/main/okou/*.deb.release.json from the
latest CardputerZero/packages main branch pointing directly to the published
e7h4n/okou-zero GitHub Release asset, and includes the release changelog in the
PR description.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request


SEMVER = r"(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)"
REPO_SLUG_RE = re.compile(r"^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+$")
BRANCH_NAME_RE = re.compile(r"^[A-Za-z0-9._/-]+$")
SOURCE_REPO = "e7h4n/okou-zero"
UPSTREAM_REPO = "CardputerZero/packages"
FORK_REPO = "e7h4n/packages"
UPSTREAM_BRANCH = "main"
FORK_BRANCH = "main"
PACKAGE_NAME = "okou"
EXPECTED_ARCH = "arm64"
UPLOADER_LOGIN = "e7h4n"
UPLOADER_EMAIL = "813596+e7h4n@users.noreply.github.com"
POOL_REL_DIR = Path("pool") / "main" / PACKAGE_NAME
MAX_ERROR_CHARS = 500


class StorePublishError(RuntimeError):
    """Step-scoped, secret-sanitized failure during store publication."""

    def __init__(self, step, detail, hint=None):
        self.step = step
        self.detail = detail
        self.hint = hint
        message = f"{step}: {detail}" if detail else step
        super().__init__(message)


def clean_env(extra=None):
    env = os.environ.copy()
    for key in (
        "CARDPUTER_PACKAGES_TOKEN",
        "OKOU_UPDATE_SIGNING_KEY",
        "OKOU_STORE_PUSH_TOKEN",
        "RELEASE_GH_TOKEN",
        "GH_TOKEN",
        "GITHUB_TOKEN",
    ):
        env.pop(key, None)
    if extra:
        env.update(extra)
    return env


def run(command, *, env_extra=None, **kwargs):
    kwargs.setdefault("check", True)
    return subprocess.run(command, env=clean_env(env_extra), **kwargs)


def sanitize_text(text, secrets=()):
    cleaned = (text or "").replace("\r\n", "\n").replace("\r", "\n")
    for secret in secrets:
        if secret:
            cleaned = cleaned.replace(secret, "[REDACTED]")
    cleaned = re.sub(r"(https?://)[^\s/@]+@", r"\1[REDACTED]@", cleaned)
    cleaned = re.sub(
        r"\b(?:gh[pousr]_[A-Za-z0-9_]+|github_pat_[A-Za-z0-9_]+)\b",
        "[REDACTED]",
        cleaned,
    )
    cleaned = re.sub(
        r"(?i)(authorization\s*:\s*(?:bearer|token)\s+)\S+",
        r"\1[REDACTED]",
        cleaned,
    )
    lines = [line.strip() for line in cleaned.splitlines() if line.strip()]
    joined = " | ".join(lines)
    if len(joined) > MAX_ERROR_CHARS:
        return joined[: MAX_ERROR_CHARS - 3].rstrip() + "..."
    return joined


def extract_api_error(stdout, stderr, secrets=()):
    status_code = None
    message = ""
    raw_stdout = (stdout or "").strip()
    if raw_stdout.startswith("{"):
        try:
            payload = json.loads(raw_stdout)
        except json.JSONDecodeError:
            payload = None
        if isinstance(payload, dict):
            raw_message = payload.get("message")
            if isinstance(raw_message, str) and raw_message.strip():
                message = raw_message
            raw_status = payload.get("status")
            if isinstance(raw_status, int):
                status_code = raw_status
            elif isinstance(raw_status, str) and raw_status.isdigit():
                status_code = int(raw_status)
    if status_code is None and stderr:
        match = re.search(r"\(HTTP\s+(\d{3})\)", stderr)
        if match:
            status_code = int(match.group(1))
    detail = sanitize_text(message or stderr or stdout, secrets=secrets)
    return status_code, detail


def is_workflow_scope_error(text):
    lower = (text or "").lower()
    return bool(
        re.search(r"\bworkflows?\s+(?:scope|permission)\b", lower)
        or ("refusing to allow" in lower and "workflow" in lower)
    )


def workflow_scope_hint(fork_repo):
    return (
        "Upstream commits touch .github/workflows/, which requires the `workflow` scope "
        "on CARDPUTER_PACKAGES_TOKEN (Classic PAT: `public_repo` + `workflow`). "
        "Add the `workflow` scope to the token, or manually click 'Sync fork' on "
        f"https://github.com/{fork_repo} and re-run this job."
    )


def run_step(step, command, *, secrets=(), env_extra=None, cwd=None):
    result = run(
        command,
        env_extra=env_extra,
        cwd=cwd,
        capture_output=True,
        text=True,
        check=False,
    )
    if result.returncode == 0:
        return result
    status_code, detail = extract_api_error(result.stdout, result.stderr, secrets=secrets)
    if status_code is not None and f"HTTP {status_code}" not in detail:
        detail = f"HTTP {status_code}: {detail}" if detail else f"HTTP {status_code}"
    if not detail:
        detail = f"command exited with code {result.returncode}"
    raise StorePublishError(step, detail)


def validate_repo_slug(repo, label):
    if (
        not isinstance(repo, str)
        or REPO_SLUG_RE.fullmatch(repo) is None
        or ".." in repo
    ):
        raise ValueError(f"{label} must be a valid GitHub owner/repo slug")
    return repo


def validate_branch_name(branch, label="Branch name"):
    if (
        not isinstance(branch, str)
        or BRANCH_NAME_RE.fullmatch(branch) is None
        or ".." in branch
        or branch.startswith("/")
        or branch.endswith("/")
    ):
        raise ValueError(f"{label} must be a valid Git branch name")
    return branch


def package_field(package, field):
    return run_step(
        f"inspect Debian package {field}",
        ["dpkg-deb", "--field", str(package), field],
    ).stdout.strip()


def validate_package(tag, package):
    if re.fullmatch("v" + SEMVER, tag) is None:
        raise ValueError("Release tag must be vMAJOR.MINOR.PATCH (stable SemVer)")
    version = tag[1:]
    if not package.is_file() or package.is_symlink():
        raise ValueError("Expected a regular Debian package")
    for field, expected in (
        ("Package", PACKAGE_NAME),
        ("Version", version),
        ("Architecture", EXPECTED_ARCH),
    ):
        if package_field(package, field) != expected:
            raise ValueError(f"Package {field} does not match store release requirements")
    filename = f"{PACKAGE_NAME}_{version}_{EXPECTED_ARCH}.deb"
    if package.name != filename:
        raise ValueError("Package filename does not match its verified metadata")
    sha256 = hashlib.sha256(package.read_bytes()).hexdigest()
    size = package.stat().st_size
    if size <= 0:
        raise ValueError("Debian package is empty")
    return version, filename, sha256, size


def build_store_manifest(version, filename, sha256, size, source_repo=SOURCE_REPO):
    return {
        "filename": filename,
        "url": f"https://github.com/{source_repo}/releases/download/v{version}/{filename}",
        "sha256": sha256,
        "size": size,
        "package": PACKAGE_NAME,
        "version": version,
        "architecture": EXPECTED_ARCH,
        "uploaded_by": UPLOADER_LOGIN,
        "uploader_email": UPLOADER_EMAIL,
    }


def format_pr_body(version, filename, sha256, release_notes="", source_repo=SOURCE_REPO):
    lines = [
        f"Update `{PACKAGE_NAME}` to `{version}` (`{EXPECTED_ARCH}`).",
        "",
        f"- Source release: https://github.com/{source_repo}/releases/tag/v{version}",
        f"- Package: `{filename}`",
        f"- SHA-256: `{sha256}`",
    ]
    notes = (release_notes or "").strip()
    if notes:
        lines.extend(["", "### Changelog", "", notes])
    return "\n".join(lines) + "\n"


def verify_asset_url(url, expected_size, attempts=4, delay_seconds=5):
    last_error = None
    for attempt in range(attempts):
        req = urllib.request.Request(
            url,
            method="HEAD",
            headers={"User-Agent": "okou-store-publisher/1"},
        )
        try:
            with urllib.request.urlopen(req, timeout=30) as response:
                if response.status != 200:
                    raise ValueError(f"Unexpected HTTP status {response.status} for release asset")
                length = response.headers.get("Content-Length")
                if length is not None and int(length) != expected_size:
                    raise ValueError("Published release asset size does not match local package")
                return
        except (urllib.error.URLError, TimeoutError, OSError) as exc:
            last_error = exc
            if attempt + 1 < attempts:
                time.sleep(delay_seconds)
    raise ValueError(f"Published release asset is not reachable yet: {last_error}")


def verify_token_owner(store_token):
    login = run_step(
        "verify CARDPUTER_PACKAGES_TOKEN owner",
        ["gh", "api", "user", "--jq", ".login"],
        secrets=(store_token,),
        env_extra={"GH_TOKEN": store_token},
    ).stdout.strip()
    if login.lower() != UPLOADER_LOGIN.lower():
        raise ValueError(
            f"CARDPUTER_PACKAGES_TOKEN belongs to @{login or 'unknown'}, expected @{UPLOADER_LOGIN}"
        )


def sync_fork_branch(fork_repo, branch, store_token):
    validate_repo_slug(fork_repo, "Fork repository")
    validate_branch_name(branch, "Fork branch")
    result = run(
        [
            "gh", "api",
            "--method", "POST",
            f"repos/{fork_repo}/merge-upstream",
            "-f", f"branch={branch}",
        ],
        env_extra={"GH_TOKEN": store_token},
        capture_output=True,
        text=True,
        check=False,
    )
    if result.returncode == 0:
        payload = {}
        raw_stdout = (result.stdout or "").strip()
        if raw_stdout:
            try:
                parsed = json.loads(raw_stdout)
                if isinstance(parsed, dict):
                    payload = parsed
            except json.JSONDecodeError:
                pass
        merge_type = payload.get("merge_type") or "unknown"
        base_branch = payload.get("base_branch") or "upstream"
        if merge_type == "none":
            print(f"Fork {fork_repo}:{branch} is already up to date with {base_branch}")
        else:
            print(f"Synced {fork_repo}:{branch} with {base_branch} ({merge_type})")
        return {"status": "synced", "merge_type": merge_type, "base_branch": base_branch}

    status_code, detail = extract_api_error(
        result.stdout, result.stderr, secrets=(store_token,)
    )
    combined = " ".join(
        part for part in (detail, sanitize_text(result.stderr, secrets=(store_token,))) if part
    )
    step = f"sync fork {fork_repo}:{branch}"
    if is_workflow_scope_error(combined):
        message = (
            f"HTTP {status_code}: {detail}"
            if status_code is not None and f"HTTP {status_code}" not in detail
            else (detail or f"command exited with code {result.returncode}")
        )
        raise StorePublishError(step, message, hint=workflow_scope_hint(fork_repo))

    if status_code == 409:
        warning = (
            f"Could not sync {fork_repo}:{branch} with upstream (HTTP 409: {detail}). "
            f"Continuing with upstream checkout; reset {fork_repo}:{branch} if push fails."
        )
        if os.environ.get("GITHUB_ACTIONS") == "true":
            print(f"::warning::{warning}")
        else:
            print(f"Warning: {warning}", file=sys.stderr)
        return {"status": "conflict", "detail": detail}

    message = (
        f"HTTP {status_code}: {detail}"
        if status_code is not None and f"HTTP {status_code}" not in detail
        else (detail or f"command exited with code {result.returncode}")
    )
    raise StorePublishError(step, message)


def fetch_release_notes(tag, release_id, token, source_repo=SOURCE_REPO):
    if release_id:
        if re.fullmatch(r"[1-9][0-9]*", release_id) is None:
            raise ValueError("Release ID must be a positive integer")
        endpoint = f"repos/{source_repo}/releases/{release_id}"
    else:
        endpoint = f"repos/{source_repo}/releases/tags/{tag}"
    env_extra = {"GH_TOKEN": token} if token else None
    secrets = (token,) if token else ()
    output = run_step(
        "fetch GitHub release notes",
        ["gh", "api", endpoint],
        secrets=secrets,
        env_extra=env_extra,
    ).stdout
    data = json.loads(output)
    return data.get("body") or ""


def is_newer_version(new_version, existing_version):
    result = run(
        ["dpkg", "--compare-versions", new_version, "gt", existing_version],
        check=False,
    )
    return result.returncode == 0


def update_store_worktree(repo_dir, version, manifest_data):
    repo_dir = Path(repo_dir)
    pool_dir = repo_dir / POOL_REL_DIR
    if not pool_dir.is_dir():
        raise ValueError(f"Expected existing store package directory {POOL_REL_DIR}")

    target_name = f"{manifest_data['filename']}.release.json"
    target_path = pool_dir / target_name
    if target_path.exists():
        return "already_published"

    existing_manifests = sorted(pool_dir.glob("*.deb.release.json"))
    for manifest_path in existing_manifests:
        existing = json.loads(manifest_path.read_text(encoding="utf-8"))
        existing_ver = existing.get("version", "")
        if existing_ver and not is_newer_version(version, existing_ver):
            raise ValueError(
                f"Version {version} is not newer than existing store version {existing_ver}"
            )

    if existing_manifests:
        run_step(
            "remove superseded store manifests",
            ["git", "rm", "--", *[str(p.relative_to(repo_dir)) for p in existing_manifests]],
            cwd=repo_dir,
        )

    target_path.write_text(json.dumps(manifest_data, indent=2) + "\n", encoding="utf-8")
    run_step(
        "stage new store manifest",
        ["git", "add", "--", str(target_path.relative_to(repo_dir))],
        cwd=repo_dir,
    )

    staged = [
        line.strip()
        for line in run_step(
            "inspect staged store manifest diff",
            ["git", "diff", "--cached", "--no-renames", "--name-only"],
            cwd=repo_dir,
        ).stdout.splitlines()
        if line.strip()
    ]
    if not staged:
        raise ValueError("No staged manifest changes to commit")
    expected_prefix = f"{POOL_REL_DIR.as_posix()}/"
    for rel_path in staged:
        if not rel_path.startswith(expected_prefix) or not rel_path.endswith(".deb.release.json"):
            raise ValueError(f"Refusing to commit unexpected path outside store manifests: {rel_path}")

    author_env = {
        "GIT_AUTHOR_NAME": UPLOADER_LOGIN,
        "GIT_AUTHOR_EMAIL": UPLOADER_EMAIL,
        "GIT_COMMITTER_NAME": UPLOADER_LOGIN,
        "GIT_COMMITTER_EMAIL": UPLOADER_EMAIL,
    }
    run_step(
        "commit store release manifest",
        ["git", "commit", "-m", f"publish: {PACKAGE_NAME} {version}"],
        cwd=repo_dir,
        env_extra=author_env,
    )
    return "committed"


def push_branch_with_token(repo_dir, fork_url, branch_name, store_token, fork_repo=FORK_REPO):
    with tempfile.TemporaryDirectory(prefix="okou-store-askpass-") as askpass_dir:
        askpass = Path(askpass_dir) / "askpass.sh"
        askpass.write_text(
            "#!/bin/sh\n"
            "case \"$1\" in\n"
            "  *Username*) printf 'x-access-token\\n' ;;\n"
            "  *Password*) printf '%s\\n' \"$OKOU_STORE_PUSH_TOKEN\" ;;\n"
            "  *) printf '\\n' ;;\n"
            "esac\n",
            encoding="utf-8",
        )
        askpass.chmod(0o700)
        result = run(
            ["git", "push", "--force", fork_url, f"HEAD:refs/heads/{branch_name}"],
            cwd=repo_dir,
            env_extra={
                "GIT_ASKPASS": str(askpass),
                "GIT_TERMINAL_PROMPT": "0",
                "OKOU_STORE_PUSH_TOKEN": store_token,
            },
            capture_output=True,
            text=True,
            check=False,
        )
        if result.returncode == 0:
            return
        detail = sanitize_text(result.stderr or result.stdout, secrets=(store_token,))
        step = f"push branch {branch_name} to {fork_repo}"
        if is_workflow_scope_error(detail):
            raise StorePublishError(step, detail, hint=workflow_scope_hint(fork_repo))
        raise StorePublishError(
            step, detail or f"git push exited with code {result.returncode}"
        )


def upsert_pull_request(upstream_repo, fork_owner, branch_name, title, body, store_token):
    head_ref = f"{fork_owner}:{branch_name}"
    existing = json.loads(
        run_step(
            f"list existing pull requests on {upstream_repo}",
            [
                "gh", "pr", "list",
                "--repo", upstream_repo,
                "--head", head_ref,
                "--state", "open",
                "--json", "number,url",
            ],
            secrets=(store_token,),
            env_extra={"GH_TOKEN": store_token},
        ).stdout
    )
    with tempfile.TemporaryDirectory(prefix="okou-store-pr-body-") as body_dir:
        body_file = Path(body_dir) / "body.md"
        body_file.write_text(body, encoding="utf-8")
        if existing:
            pr_number = str(existing[0]["number"])
            run_step(
                f"update store pull request #{pr_number}",
                [
                    "gh", "pr", "edit", pr_number,
                    "--repo", upstream_repo,
                    "--title", title,
                    "--body-file", str(body_file),
                ],
                secrets=(store_token,),
                env_extra={"GH_TOKEN": store_token},
            )
            return existing[0]["url"]
        return run_step(
            f"create store pull request on {upstream_repo}",
            [
                "gh", "pr", "create",
                "--repo", upstream_repo,
                "--base", UPSTREAM_BRANCH,
                "--head", head_ref,
                "--title", title,
                "--body-file", str(body_file),
            ],
            secrets=(store_token,),
            env_extra={"GH_TOKEN": store_token},
        ).stdout.strip()


def write_step_summary(version, pr_url):
    summary_path = os.environ.get("GITHUB_STEP_SUMMARY")
    if not summary_path:
        return
    with open(summary_path, "a", encoding="utf-8") as handle:
        handle.write(
            f"### CardputerZero Store PR\n\n"
            f"- Version: `{version}`\n"
            f"- Pull request: {pr_url}\n"
        )


def write_failure_summary(error):
    summary_path = os.environ.get("GITHUB_STEP_SUMMARY")
    if not summary_path:
        return
    lines = [
        "### CardputerZero Store Publish Failed",
        "",
        f"- Step: `{error.step}`",
        f"- Detail: {error.detail}",
    ]
    if error.hint:
        lines.append(f"- Action: {error.hint}")
    with open(summary_path, "a", encoding="utf-8") as handle:
        handle.write("\n".join(lines) + "\n")


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tag")
    parser.add_argument("--release-id")
    parser.add_argument("--package", type=Path)
    parser.add_argument("--source-repo", default=SOURCE_REPO)
    parser.add_argument("--upstream-repo", default=UPSTREAM_REPO)
    parser.add_argument("--fork-repo", default=FORK_REPO)
    parser.add_argument(
        "--fork-branch", default=FORK_BRANCH,
        help="fork branch to sync with upstream before pushing",
    )
    parser.add_argument(
        "--sync-fork-only", action="store_true",
        help="verify token owner and sync the packages fork with upstream without opening a PR",
    )
    parser.add_argument(
        "--skip-fork-sync", action="store_true",
        help="skip syncing the packages fork before pushing (when synced in a prior CI step)",
    )
    parser.add_argument(
        "--dry-run", action="store_true",
        help="validate package, manifest, and PR body without pushing or creating a PR",
    )
    args = parser.parse_args(argv)

    if args.sync_fork_only:
        if args.dry_run or args.skip_fork_sync:
            parser.error("--sync-fork-only cannot be combined with --dry-run or --skip-fork-sync")
    elif not args.tag or not args.package:
        parser.error("--tag and --package are required unless --sync-fork-only is set")

    store_token = os.environ.pop("CARDPUTER_PACKAGES_TOKEN", "").strip()
    release_token = os.environ.pop("RELEASE_GH_TOKEN", "").strip() or store_token
    try:
        validate_repo_slug(args.source_repo, "Source repository")
        validate_repo_slug(args.upstream_repo, "Upstream repository")
        validate_repo_slug(args.fork_repo, "Fork repository")
        validate_branch_name(args.fork_branch, "Fork branch")

        if args.sync_fork_only:
            if not store_token:
                raise ValueError("CARDPUTER_PACKAGES_TOKEN is required")
            verify_token_owner(store_token)
            if args.fork_repo.lower() != args.upstream_repo.lower():
                sync_fork_branch(args.fork_repo, args.fork_branch, store_token)
            return 0

        version, filename, sha256, size = validate_package(args.tag, args.package)
        manifest_data = build_store_manifest(
            version, filename, sha256, size, source_repo=args.source_repo
        )

        if args.dry_run:
            notes = ""
            if release_token:
                notes = fetch_release_notes(
                    args.tag, args.release_id, release_token, source_repo=args.source_repo
                )
            print(json.dumps(manifest_data, indent=2))
            print("---")
            print(format_pr_body(version, filename, sha256, notes, source_repo=args.source_repo))
            return 0

        if not store_token:
            raise ValueError("CARDPUTER_PACKAGES_TOKEN is required")

        verify_token_owner(store_token)
        verify_asset_url(manifest_data["url"], size)
        notes = fetch_release_notes(
            args.tag, args.release_id, release_token, source_repo=args.source_repo
        )
        title = f"publish: {PACKAGE_NAME} {version}"
        body = format_pr_body(version, filename, sha256, notes, source_repo=args.source_repo)
        branch_name = f"publish/{PACKAGE_NAME}-{version}"
        fork_owner = args.fork_repo.split("/", 1)[0]
        upstream_url = f"https://github.com/{args.upstream_repo}.git"
        fork_url = f"https://github.com/{args.fork_repo}.git"

        with tempfile.TemporaryDirectory(prefix="okou-store-packages-") as work_dir:
            repo_dir = Path(work_dir) / "packages"
            run_step(
                f"clone {args.upstream_repo}:{UPSTREAM_BRANCH}",
                [
                    "git", "clone",
                    "--filter=blob:none",
                    "--sparse",
                    "--single-branch",
                    "--branch", UPSTREAM_BRANCH,
                    upstream_url,
                    str(repo_dir),
                ],
            )
            run_step(
                "configure sparse checkout",
                ["git", "sparse-checkout", "set", POOL_REL_DIR.as_posix()],
                cwd=repo_dir,
            )
            outcome = update_store_worktree(repo_dir, version, manifest_data)
            if outcome == "already_published":
                print(f"Store version {version} is already published in {args.upstream_repo}")
                return 0
            if not args.skip_fork_sync and args.fork_repo.lower() != args.upstream_repo.lower():
                sync_fork_branch(args.fork_repo, args.fork_branch, store_token)
            push_branch_with_token(
                repo_dir, fork_url, branch_name, store_token, fork_repo=args.fork_repo
            )

        pr_url = upsert_pull_request(
            args.upstream_repo, fork_owner, branch_name, title, body, store_token
        )
        print(f"Store update PR ready: {pr_url}")
        write_step_summary(version, pr_url)
        return 0
    except StorePublishError as error:
        print(f"Store publish failed during {error.step}: {error.detail}", file=sys.stderr)
        if error.hint:
            print(f"Hint: {error.hint}", file=sys.stderr)
        write_failure_summary(error)
    except (ValueError, OSError) as error:
        print(f"Store publish refused: {error}", file=sys.stderr)
    except subprocess.CalledProcessError as error:
        print(
            f"Store publish failed during git or GitHub API operation (exit code {error.returncode})",
            file=sys.stderr,
        )
    return 1


if __name__ == "__main__":
    sys.exit(main())
