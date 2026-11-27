#!/usr/bin/env python3
"""Small, deterministic source-policy check; not a replacement for secret scanning."""
import json
import pathlib
import re
import subprocess

SEMVER = r"(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)"

root = pathlib.Path(__file__).resolve().parents[1]
files = subprocess.check_output(["git", "ls-files", "-z"], cwd=root).split(b"\0")
errors = []
for raw in files:
    if not raw:
        continue
    relative = raw.decode()
    path = root / relative
    if not path.is_file():
        continue
    public_key = relative == "assets/update-public.pem"
    if public_key and not re.fullmatch(r"-----BEGIN PUBLIC KEY-----\n[A-Za-z0-9+/=\n]+-----END PUBLIC KEY-----\n", path.read_text()):
        errors.append(f"{relative}: only a public update-verification key is allowed")
    if (path.suffix in {".deb", ".pem", ".key", ".wav"} and not public_key) or "credentials.json" in relative:
        errors.append(f"{relative}: runtime artifact must not be tracked")
    if path.suffix not in {".cpp", ".hpp", ".h", ".md", ".yml", ".json", ".py", ".txt"}:
        continue
    text = path.read_text(encoding="utf-8")
    if re.search(r"-----BEGIN (?:RSA |EC |OPENSSH )?PRIVATE KEY-----", text):
        errors.append(f"{relative}: private key content")
    if re.search(r"\b(?:sk_live_|ghp_|github_pat_)[A-Za-z0-9_]{20,}", text):
        errors.append(f"{relative}: potential production credential")
    if relative.startswith(".github/workflows/") and "pull_request_target" in text:
        errors.append(f"{relative}: privileged fork execution is prohibited")

manifest_file = root / ".release-please-manifest.json"
cmake_file = root / "CMakeLists.txt"
if manifest_file.is_file() and cmake_file.is_file():
    manifest_data = json.loads(manifest_file.read_text(encoding="utf-8"))
    manifest_ver = manifest_data.get(".")
    cmake_match = re.search(
        r'set\(OKOU_ZERO_BUILD_VERSION\s+"([^"]+)"\s+CACHE\s+STRING\b',
        cmake_file.read_text(encoding="utf-8"),
    )
    cmake_ver = cmake_match.group(1) if cmake_match else None
    if not isinstance(manifest_ver, str) or re.fullmatch(SEMVER, manifest_ver) is None:
        errors.append(".release-please-manifest.json: root version must be stable SemVer")
    if not isinstance(cmake_ver, str) or re.fullmatch(SEMVER, cmake_ver) is None:
        errors.append("CMakeLists.txt: OKOU_ZERO_BUILD_VERSION must be stable SemVer")
    elif isinstance(manifest_ver, str) and cmake_ver != manifest_ver:
        errors.append(
            f"Version mismatch: CMakeLists.txt ({cmake_ver}) != .release-please-manifest.json ({manifest_ver})"
        )

if errors:
    raise SystemExit("\n".join(errors))
print("Repository hygiene checks passed (not a comprehensive secret audit)")
