#!/usr/bin/env python3
"""Prepare a signed arm64 store package and create (never overwrite) a stable release.

Only trusted main CI should supply OKOU_UPDATE_SIGNING_KEY. No publication is
attempted until all local package, commit, and signature checks have passed.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parent.parent
REPOSITORY = "e7h4n/okou-zero"
SEMVER = r"(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)"


def run(command, **kwargs):
    # Do not pass the signing secret to openssl, dpkg, or gh environments.
    env = os.environ.copy()
    env.pop("OKOU_UPDATE_SIGNING_KEY", None)
    kwargs.setdefault("check", True)
    return subprocess.run(command, env=env, **kwargs)


def package_field(package, field):
    return run(
        ["dpkg-deb", "--field", str(package), field],
        capture_output=True, text=True,
    ).stdout.strip()


def git_output(*args):
    return run(["git", *args], cwd=ROOT, capture_output=True, text=True).stdout.strip()


def next_version():
    tag = git_output(
        "describe", "--tags", "--first-parent", "--abbrev=0",
        "--match", "v[0-9]*.[0-9]*.[0-9]*", "HEAD",
    )
    parts = re.fullmatch("v" + SEMVER, tag)
    if parts is None:
        raise ValueError("The previous release tag is not stable SemVer")
    major, minor, patch = map(int, parts.groups())
    if git_output("rev-parse", f"{tag}^{{commit}}") != git_output("rev-parse", "HEAD"):
        patch += 1
    return f"{major}.{minor}.{patch}"


def release_exists(tag):
    result = run(
        ["gh", "release", "view", tag, "--repo", REPOSITORY,
         "--json", "isDraft,isPrerelease,assets"],
        check=False, capture_output=True, text=True,
    )
    if result.returncode:
        return False
    release = json.loads(result.stdout)
    expected = {
        "manifest.json", "manifest.sig", "SHA256SUMS",
        f"okou_{tag[1:]}_arm64.deb",
    }
    if (release["isDraft"] or release["isPrerelease"] or
            not expected <= {asset["name"] for asset in release["assets"]}):
        raise ValueError("An incomplete or non-stable release already exists for this tag")
    return True


def validate_package(tag, package):
    if re.fullmatch("v" + SEMVER, tag) is None:
        raise ValueError("Release tag must be vMAJOR.MINOR.PATCH (stable SemVer)")
    version = tag[1:]
    if not package.is_file() or package.is_symlink():
        raise ValueError("Expected a regular Debian package")
    for field, expected in (
        ("Package", "okou"), ("Version", version), ("Architecture", "arm64")
    ):
        if package_field(package, field) != expected:
            raise ValueError(f"Package {field} does not match release requirements")
    filename = f"okou_{version}_arm64.deb"
    if package.name != filename:
        raise ValueError("Package filename does not match its verified metadata")
    return version, filename


def manifest_bytes(version, filename, package):
    return (json.dumps({
        "schema": 1,
        "version": version,
        "architecture": "arm64",
        "package": "okou",
        "filename": filename,
        "sha256": hashlib.sha256(package.read_bytes()).hexdigest(),
        "size": package.stat().st_size,
    }, sort_keys=True, separators=(",", ":")) + "\n").encode("utf-8")


def prepare(tag, package, output_dir, private_key):
    # Metadata mismatch must fail before signing or any GitHub API operation.
    version, filename = validate_package(tag, package)
    if not private_key.strip():
        raise ValueError("OKOU_UPDATE_SIGNING_KEY is required")
    public_key = ROOT / "assets" / "update-public.pem"
    if not public_key.is_file():
        raise ValueError("The checked-in update public key is missing")
    # Refuse stale output, including accidental reuse from a previous release.
    output_dir.mkdir(parents=True, exist_ok=False)
    destination = output_dir / filename
    shutil.copyfile(package, destination)
    manifest = output_dir / "manifest.json"
    signature = output_dir / "manifest.sig"
    manifest.write_bytes(manifest_bytes(version, filename, destination))
    # mkstemp creates a 0600 file; the directory is private as well. Neither the
    # PEM nor openssl diagnostics are printed, even when key parsing fails.
    with tempfile.TemporaryDirectory(prefix="okou-release-key-") as key_dir:
        fd, key_name = tempfile.mkstemp(dir=key_dir, suffix=".pem")
        try:
            os.fchmod(fd, 0o600)
            with os.fdopen(fd, "w", encoding="utf-8") as key_file:
                key_file.write(private_key)
            run([
                "openssl", "dgst", "-sha256", "-sign", key_name,
                "-passin", "pass:", "-out", str(signature), str(manifest),
            ], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        finally:
            Path(key_name).unlink(missing_ok=True)
    # Fail closed for the wrong secret, before any release can become latest.
    run([
        "openssl", "dgst", "-sha256", "-verify", str(public_key),
        "-signature", str(signature), str(manifest),
    ], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    assets = [destination, manifest, signature]
    checksums = output_dir / "SHA256SUMS"
    checksums.write_text("".join(
        f"{hashlib.sha256(asset.read_bytes()).hexdigest()}  {asset.name}\n"
        for asset in sorted(assets, key=lambda asset: asset.name)
    ), encoding="utf-8")
    return version, assets + [checksums]


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--next-version", action="store_true")
    parser.add_argument("--tag")
    parser.add_argument("--commit")
    parser.add_argument("--package", type=Path)
    parser.add_argument("--output-dir", type=Path)
    parser.add_argument(
        "--prepare-only", action="store_true",
        help="validate, sign, and verify locally without publishing",
    )
    args = parser.parse_args(argv)
    private_key = os.environ.pop("OKOU_UPDATE_SIGNING_KEY", "")
    try:
        if args.next_version:
            print(next_version())
            return 0
        if args.tag is None or args.package is None or args.output_dir is None:
            parser.error("--tag, --package, and --output-dir are required")
        create_tag = False
        if not args.prepare_only:
            if args.commit != git_output("rev-parse", "HEAD") or args.tag != "v" + next_version():
                raise ValueError("Release commit or version does not match the tested main checkout")
            target = tuple(map(int, args.tag[1:].split(".")))
            for tag in git_output("tag", "--list", "v*").splitlines():
                candidate = re.fullmatch("v" + SEMVER, tag)
                if candidate and tuple(map(int, candidate.groups())) > target:
                    raise ValueError("A newer stable version already has a tag")
            tag_ref = "refs/tags/" + args.tag
            tags = git_output("tag", "--list", args.tag)
            if tags:
                if git_output("rev-parse", f"{tag_ref}^{{commit}}") != args.commit:
                    raise ValueError("Release tag already belongs to another commit")
                if release_exists(args.tag):
                    print(f"Release {args.tag} already exists for the tested commit")
                    return 0
            else:
                create_tag = True
        version, assets = prepare(
            args.tag, args.package.absolute(), args.output_dir.absolute(), private_key
        )
        private_key = ""
        if not args.prepare_only:
            # Create the ref atomically after signature verification. A
            # conflicting ref aborts without publishing or moving any tag.
            if create_tag:
                run([
                    "gh", "api", "--method", "POST", f"repos/{REPOSITORY}/git/refs",
                    "-f", f"ref={tag_ref}", "-f", f"sha={args.commit}",
                ], stdout=subprocess.DEVNULL)
            run([
                "gh", "release", "create", args.tag, "--repo", REPOSITORY,
                "--verify-tag", "--latest=false", "--title", f"Okou {version}",
                "--notes", (
                    f"Okou {version} for the Cardputer Zero AppStore (arm64).\n\n"
                    "Install the okou package through the AppStore or with apt. "
                    "The signed manifest and SHA-256 checksums identify this build."
                ),
                *map(str, assets),
            ])
        return 0
    except (ValueError, OSError) as error:
        print(f"Release refused: {error}", file=sys.stderr)
    except subprocess.CalledProcessError:
        # Do not echo subprocess output or arguments which may concern a key.
        print("Release failed: package inspection, signing, verification, or publication failed", file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main())
