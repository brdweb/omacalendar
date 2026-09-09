#!/usr/bin/env python3
"""Verify a finalized SPDX inventory against the exact exported Flatpak bundle."""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import subprocess
import tempfile


APP_REF = "app/org.omacalendar.OmaCalendar/x86_64/stable"


def require(condition: bool, message: str) -> None:
    if not condition:
        raise ValueError(message)


def verify_payload(document: dict, payload: Path, version: str) -> int:
    require(document.get("spdxVersion") == "SPDX-2.3", "expected SPDX-2.3")
    require(document.get("name") == f"omacalendar-{version}-flatpak-payload", "wrong SPDX release identity")
    require(payload.is_dir(), "exported payload is missing")
    payload = payload.resolve()
    actual = {path.relative_to(payload).as_posix(): path for path in payload.rglob("*")
              if path.is_file() and not path.is_symlink()}
    files = document.get("files", [])
    by_name = {}
    by_id = {}
    for item in files:
        relative = PurePosixPath(item["fileName"])
        require(not relative.is_absolute() and ".." not in relative.parts and str(relative) != ".",
                f"unsafe SPDX file path: {item['fileName']}")
        name = str(relative)
        require(name not in by_name and item["SPDXID"] not in by_id, "duplicate SPDX file path or ID")
        by_name[name] = item
        by_id[item["SPDXID"]] = item
    missing = sorted(by_name.keys() - actual.keys())
    unlisted = sorted(actual.keys() - by_name.keys())
    require(not missing and not unlisted, f"bundle inventory mismatch: missing={missing}; unlisted={unlisted}")
    require(files, "empty SPDX file inventory")
    for name, item in by_name.items():
        path = actual[name]
        require(path.resolve().is_relative_to(payload), f"payload file escapes exported tree: {name}")
        data = path.read_bytes()
        checks = {check["algorithm"]: check["checksumValue"] for check in item["checksums"]}
        for algorithm, digest in (("SHA1", "sha1"), ("SHA256", "sha256")):
            require(checks.get(algorithm) == hashlib.new(digest, data).hexdigest(),
                    f"{algorithm} mismatch: {name}")

    packages = document.get("packages", [])
    roots = [item for item in packages if item["SPDXID"].startswith("SPDXRef-DocumentRoot-")]
    require(len(roots) == 1, "expected one SPDX application package")
    root = roots[0]
    require(root.get("name") == "omacalendar" and root.get("versionInfo") == version,
            "wrong application package identity")
    bundled = [item for item in packages if item["SPDXID"].startswith("SPDXRef-Bundled-")]
    require({item["name"] for item in bundled} == {"libical", "libsecret"}, "missing bundled library packages")
    for package in [root, *bundled]:
        owned = {relation["relatedSpdxElement"] for relation in document["relationships"]
                 if relation["spdxElementId"] == package["SPDXID"]
                 and relation["relationshipType"] == "CONTAINS"
                 and relation["relatedSpdxElement"] in by_id}
        require(package.get("filesAnalyzed") is True and bool(owned),
                f"missing package file coverage: {package['name']}")
        if package is root:
            require(owned == by_id.keys(), "application package does not contain every shipped file")
        sha1s = sorted(next(check["checksumValue"] for check in by_id[file_id]["checksums"]
                           if check["algorithm"] == "SHA1") for file_id in owned)
        code = hashlib.sha1("".join(sha1s).encode()).hexdigest()
        require(package.get("packageVerificationCode", {}).get("packageVerificationCodeValue") == code,
                f"package verification code mismatch: {package['name']}")
    return len(files)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bundle", type=Path, required=True)
    parser.add_argument("--sbom", type=Path, required=True)
    parser.add_argument("--version", required=True)
    args = parser.parse_args()
    try:
        document = json.loads(args.sbom.read_text())
        require(args.bundle.is_file(), "Flatpak bundle is missing")
        # No app installation, sandbox execution, network, or user profile use.
        with tempfile.TemporaryDirectory(prefix="omacalendar-flatpak-sbom-") as temporary:
            root = Path(temporary)
            repository = root / "repo"
            payload = root / "payload"
            subprocess.run(["ostree", f"--repo={repository}", "init", "--mode=bare-user"], check=True)
            subprocess.run(["flatpak", "build-import-bundle", str(repository), str(args.bundle.resolve())], check=True)
            subprocess.run(["ostree", f"--repo={repository}", "checkout", "--user-mode", "--force-copy",
                            "--subpath=/files", APP_REF, str(payload)], check=True)
            count = verify_payload(document, payload, args.version)
    except (ValueError, KeyError, StopIteration, OSError, subprocess.CalledProcessError) as error:
        parser.exit(1, f"Flatpak SPDX verification failed: {error}\n")
    print(f"PASS: exact Flatpak bundle SPDX covers all {count} regular files with SHA1/SHA256 and package verification codes")


if __name__ == "__main__":
    main()
