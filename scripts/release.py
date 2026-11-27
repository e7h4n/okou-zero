#!/usr/bin/env python3
"""Prepare a signed arm64 store package and publish a release-please draft as latest.

Only trusted main CI should supply OKOU_UPDATE_SIGNING_KEY. No publication is
attempted until all local package, commit, version, and signature checks have passed.
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
import urllib.parse


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


def manifest_version():
    manifest_path = ROOT / ".release-please-manifest.json"
    if not manifest_path.is_file():
        raise ValueError("Missing .release-please-manifest.json")
    data = json.loads(manifest_path.read_text(encoding="utf-8"))
    version = data.get(".")
    if not isinstance(version, str) or re.fullmatch(SEMVER, version) is None:
        raise ValueError("Root version in .release-please-manifest.json is not stable SemVer")
    return version


def cmake_version():
    cmake_path = ROOT / "CMakeLists.txt"
    if not cmake_path.is_file():
        raise ValueError("Missing CMakeLists.txt")
    match = re.search(
        r'set\(OKOU_ZERO_BUILD_VERSION\s+"([^"]+)"\s+CACHE\s+STRING\b',
        cmake_path.read_text(encoding="utf-8"),
    )
    if match is None or re.fullmatch(SEMVER, match.group(1)) is None:
        raise ValueError("CMakeLists.txt version is not stable SemVer")
    return match.group(1)


def validate_repo_version(tag):
    if re.fullmatch("v" + SEMVER, tag) is None:
        raise ValueError("Release tag must be vMAJOR.MINOR.PATCH (stable SemVer)")
    version = tag[1:]
    if manifest_version() != version:
        raise ValueError("Release tag does not match .release-please-manifest.json")
    if cmake_version() != version:
        raise ValueError("Release tag does not match CMakeLists.txt")
    return version


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


def expected_asset_names(version):
    return {
        "manifest.json",
        "manifest.sig",
        "SHA256SUMS",
        f"okou_{version}_arm64.deb",
    }


def fetch_release(release_id):
    if not isinstance(release_id, str) or re.fullmatch(r"[1-9][0-9]*", release_id) is None:
        raise ValueError("Release ID must be a positive integer")
    output = run(
        ["gh", "api", f"repos/{REPOSITORY}/releases/{release_id}"],
        capture_output=True, text=True,
    ).stdout
    return json.loads(output)


def validate_upload_url(upload_url, release_id):
    base = upload_url.split("{", 1)[0]
    expected = f"https://uploads.github.com/repos/{REPOSITORY}/releases/{release_id}/assets"
    if base != expected:
        raise ValueError("Unexpected GitHub release upload URL")
    return base


def inspect_release(release, tag, commit):
    version = tag[1:]
    if release.get("tag_name") != tag:
        raise ValueError("GitHub release tag does not match the requested release")
    if release.get("target_commitish") != commit:
        raise ValueError("GitHub release commit does not match the tested main checkout")
    if release.get("prerelease") is not False:
        raise ValueError("Refusing to publish a prerelease as a stable release")
    expected = expected_asset_names(version)
    existing_names = {asset.get("name") for asset in release.get("assets", [])}
    if release.get("draft") is False:
        if expected <= existing_names:
            return "published"
        raise ValueError("An incomplete published release already exists for this tag")
    if release.get("draft") is not True:
        raise ValueError("Unexpected GitHub release draft state")
    return "draft"


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


def upload_and_publish(release_id, release, assets):
    upload_base = validate_upload_url(release.get("upload_url", ""), release_id)
    expected_sizes = {asset.name: asset.stat().st_size for asset in assets}
    # Clean up any partial uploads on this draft before uploading fresh assets.
    for existing in release.get("assets", []):
        if existing.get("name") in expected_sizes:
            asset_id = existing.get("id")
            if not isinstance(asset_id, int) or asset_id <= 0:
                raise ValueError("Invalid existing GitHub release asset ID")
            run([
                "gh", "api", "--method", "DELETE",
                f"repos/{REPOSITORY}/releases/assets/{asset_id}",
            ], stdout=subprocess.DEVNULL)
    for asset in assets:
        upload_url = f"{upload_base}?name={urllib.parse.quote(asset.name, safe='')}"
        run([
            "gh", "api", "--method", "POST", upload_url,
            "-H", "Content-Type: application/octet-stream",
            "--input", str(asset),
        ], stdout=subprocess.DEVNULL)
    refreshed = fetch_release(release_id)
    uploaded = {
        asset.get("name"): asset.get("size")
        for asset in refreshed.get("assets", [])
    }
    for name, size in expected_sizes.items():
        if uploaded.get(name) != size:
            raise ValueError(f"Uploaded asset verification failed for {name}")
    run([
        "gh", "api", "--method", "PATCH",
        f"repos/{REPOSITORY}/releases/{release_id}",
        "-F", "draft=false",
        "-f", "make_latest=true",
    ], stdout=subprocess.DEVNULL)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tag")
    parser.add_argument("--commit")
    parser.add_argument("--release-id")
    parser.add_argument("--package", type=Path)
    parser.add_argument("--output-dir", type=Path)
    parser.add_argument(
        "--prepare-only", action="store_true",
        help="validate, sign, and verify locally without publishing",
    )
    args = parser.parse_args(argv)
    private_key = os.environ.pop("OKOU_UPDATE_SIGNING_KEY", "")
    try:
        if args.tag is None or args.package is None or args.output_dir is None:
            parser.error("--tag, --package, and --output-dir are required")
        release = None
        if not args.prepare_only:
            if not args.commit or not args.release_id:
                parser.error("--commit and --release-id are required when publishing")
            validate_repo_version(args.tag)
            if args.commit != git_output("rev-parse", "HEAD"):
                raise ValueError("Release commit does not match the tested main checkout")
            release = fetch_release(args.release_id)
            state = inspect_release(release, args.tag, args.commit)
            if state == "published":
                print(f"Release {args.tag} is already published for the tested commit")
                return 0
        _, assets = prepare(
            args.tag, args.package.absolute(), args.output_dir.absolute(), private_key
        )
        private_key = ""
        if not args.prepare_only:
            upload_and_publish(args.release_id, release, assets)
        return 0
    except (ValueError, OSError) as error:
        print(f"Release refused: {error}", file=sys.stderr)
    except subprocess.CalledProcessError:
        # Do not echo subprocess output or arguments which may concern a key.
        print("Release failed: package inspection, signing, verification, or publication failed", file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main())
