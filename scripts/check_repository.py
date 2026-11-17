#!/usr/bin/env python3
"""Small, deterministic source-policy check; not a replacement for secret scanning."""
import pathlib
import re
import subprocess

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
if errors:
    raise SystemExit("\n".join(errors))
print("Repository hygiene checks passed (not a comprehensive secret audit)")
