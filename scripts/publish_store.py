#!/usr/bin/env python3
"""Submit a verified arm64 release manifest PR to CardputerZero/packages.

Updates only pool/main/okou/*.deb.release.json in e7h4n/packages from the
latest CardputerZero/packages main branch, pointing directly to the published
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
SOURCE_REPO = "e7h4n/okou-zero"
UPSTREAM_REPO = "CardputerZero/packages"
FORK_REPO = "e7h4n/packages"
PACKAGE_NAME = "okou"
EXPECTED_ARCH = "arm64"
UPLOADER_LOGIN = "e7h4n"
UPLOADER_EMAIL = "813596+e7h4n@users.noreply.github.com"
POOL_REL_DIR = Path("pool") / "main" / PACKAGE_NAME


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


def package_field(package, field):
    return run(
        ["dpkg-deb", "--field", str(package), field],
        capture_output=True, text=True,
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
    login = run(
        ["gh", "api", "user", "--jq", ".login"],
        env_extra={"GH_TOKEN": store_token},
        capture_output=True, text=True,
    ).stdout.strip()
    if login.lower() != UPLOADER_LOGIN.lower():
        raise ValueError(
            f"CARDPUTER_PACKAGES_TOKEN belongs to @{login or 'unknown'}, expected @{UPLOADER_LOGIN}"
        )


def fetch_release_notes(tag, release_id, token, source_repo=SOURCE_REPO):
    if release_id:
        if re.fullmatch(r"[1-9][0-9]*", release_id) is None:
            raise ValueError("Release ID must be a positive integer")
        endpoint = f"repos/{source_repo}/releases/{release_id}"
    else:
        endpoint = f"repos/{source_repo}/releases/tags/{tag}"
    env_extra = {"GH_TOKEN": token} if token else None
    output = run(
        ["gh", "api", endpoint],
        env_extra=env_extra,
        capture_output=True, text=True,
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
        run(
            ["git", "rm", "--", *[str(p.relative_to(repo_dir)) for p in existing_manifests]],
            cwd=repo_dir,
            stdout=subprocess.DEVNULL,
        )

    target_path.write_text(json.dumps(manifest_data, indent=2) + "\n", encoding="utf-8")
    run(
        ["git", "add", "--", str(target_path.relative_to(repo_dir))],
        cwd=repo_dir,
    )

    staged = [
        line.strip()
        for line in run(
            ["git", "diff", "--cached", "--no-renames", "--name-only"],
            cwd=repo_dir,
            capture_output=True, text=True,
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
    run(
        ["git", "commit", "-m", f"publish: {PACKAGE_NAME} {version}"],
        cwd=repo_dir,
        env_extra=author_env,
        stdout=subprocess.DEVNULL,
    )
    return "committed"


def push_branch_with_token(repo_dir, fork_url, branch_name, store_token):
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
        run(
            ["git", "push", "--force", fork_url, f"HEAD:refs/heads/{branch_name}"],
            cwd=repo_dir,
            env_extra={
                "GIT_ASKPASS": str(askpass),
                "GIT_TERMINAL_PROMPT": "0",
                "OKOU_STORE_PUSH_TOKEN": store_token,
            },
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )


def upsert_pull_request(upstream_repo, fork_owner, branch_name, title, body, store_token):
    head_ref = f"{fork_owner}:{branch_name}"
    existing = json.loads(
        run(
            [
                "gh", "pr", "list",
                "--repo", upstream_repo,
                "--head", head_ref,
                "--state", "open",
                "--json", "number,url",
            ],
            env_extra={"GH_TOKEN": store_token},
            capture_output=True, text=True,
        ).stdout
    )
    with tempfile.TemporaryDirectory(prefix="okou-store-pr-body-") as body_dir:
        body_file = Path(body_dir) / "body.md"
        body_file.write_text(body, encoding="utf-8")
        if existing:
            pr_number = str(existing[0]["number"])
            run(
                [
                    "gh", "pr", "edit", pr_number,
                    "--repo", upstream_repo,
                    "--title", title,
                    "--body-file", str(body_file),
                ],
                env_extra={"GH_TOKEN": store_token},
                stdout=subprocess.DEVNULL,
            )
            return existing[0]["url"]
        return run(
            [
                "gh", "pr", "create",
                "--repo", upstream_repo,
                "--base", "main",
                "--head", head_ref,
                "--title", title,
                "--body-file", str(body_file),
            ],
            env_extra={"GH_TOKEN": store_token},
            capture_output=True, text=True,
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


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tag", required=True)
    parser.add_argument("--release-id")
    parser.add_argument("--package", type=Path, required=True)
    parser.add_argument("--source-repo", default=SOURCE_REPO)
    parser.add_argument("--upstream-repo", default=UPSTREAM_REPO)
    parser.add_argument("--fork-repo", default=FORK_REPO)
    parser.add_argument(
        "--dry-run", action="store_true",
        help="validate package, manifest, and PR body without pushing or creating a PR",
    )
    args = parser.parse_args(argv)

    store_token = os.environ.pop("CARDPUTER_PACKAGES_TOKEN", "").strip()
    release_token = os.environ.pop("RELEASE_GH_TOKEN", "").strip() or store_token
    try:
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
            run(
                [
                    "git", "clone",
                    "--filter=blob:none",
                    "--sparse",
                    "--single-branch",
                    "--branch", "main",
                    upstream_url,
                    str(repo_dir),
                ],
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )
            run(
                ["git", "sparse-checkout", "set", POOL_REL_DIR.as_posix()],
                cwd=repo_dir,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )
            outcome = update_store_worktree(repo_dir, version, manifest_data)
            if outcome == "already_published":
                print(f"Store version {version} is already published in {args.upstream_repo}")
                return 0
            push_branch_with_token(repo_dir, fork_url, branch_name, store_token)

        pr_url = upsert_pull_request(
            args.upstream_repo, fork_owner, branch_name, title, body, store_token
        )
        print(f"Store update PR ready: {pr_url}")
        write_step_summary(version, pr_url)
        return 0
    except (ValueError, OSError) as error:
        print(f"Store publish refused: {error}", file=sys.stderr)
    except subprocess.CalledProcessError:
        print("Store publish failed during git or GitHub API operation", file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main())
