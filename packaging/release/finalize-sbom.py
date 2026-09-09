#!/usr/bin/env python3
"""Complete a Syft SPDX 2.3 staged-payload inventory, not a host/runtime SBOM."""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import re


REPOSITORY = Path(__file__).resolve().parents[2]
TOOL = "Tool: omacalendar-finalize-sbom-1"


def require(condition: bool, message: str) -> None:
    if not condition:
        raise ValueError(message)


def pins(repository: Path, package_format: str) -> dict[str, dict[str, str]]:
    manifest = json.loads((repository / "packaging/flatpak/org.omacalendar.OmaCalendar.json").read_text())
    result = {}
    for name in ("libical", "libsecret"):
        module = next(item for item in manifest["modules"] if item["name"] == name)
        source = module["sources"][0]
        match = re.search(rf"/{name}-(\d+\.\d+\.\d+)\.tar\.(?:gz|xz)$", source["url"])
        require(match is not None, f"cannot derive {name} version from pinned source")
        require(re.fullmatch(r"[0-9a-f]{64}", source["sha256"]) is not None, f"invalid {name} source hash")
        result[name] = {"version": match[1], "url": source["url"], "sha256": source["sha256"]}
    if package_format == "debian":
        script = (repository / "packaging/release/build-deb-release.sh").read_text()
        for variable, field in (("libical_version", "version"), ("libical_sha256", "sha256")):
            matches = re.findall(rf"^{variable}=([0-9a-f.]+)$", script, re.MULTILINE)
            require(matches == [result["libical"][field]], f"Debian and Flatpak {variable} pins disagree")
    return result


def safe_name(name: str) -> str:
    path = PurePosixPath(name)
    require(not path.is_absolute() and ".." not in path.parts and str(path) != ".", f"unsafe SPDX file path: {name}")
    return str(path)


def finalize(document: dict, stage: Path, version: str, package_format: str,
             repository: Path = REPOSITORY) -> dict:
    require(package_format in ("arch", "debian", "flatpak"), "unknown package format")
    require(re.fullmatch(r"(?:0|[1-9]\d*)\.(?:0|[1-9]\d*)\.(?:0|[1-9]\d*)(?:-[0-9A-Za-z.-]+)?", version) is not None,
            "invalid release version")
    require(document.get("spdxVersion") == "SPDX-2.3", "expected Syft SPDX-2.3 input")
    require(document.get("SPDXID") == "SPDXRef-DOCUMENT", "missing document identity")
    require(stage.is_dir(), "payload staging directory does not exist")
    require((repository / "LICENSE").read_text().startswith("MIT License\n"), "review application license change")
    stage = stage.resolve()
    prefix = "" if package_format == "flatpak" else "usr/"
    required = [prefix + "bin/omacalendard", prefix + "bin/omacalendarctl", prefix + "bin/omacalendar"]
    if package_format == "flatpak":
        required += ["libexec/omacalendar/omacalendar-ui", "bin/secret-tool"]
    paths = {path.relative_to(stage).as_posix(): path for path in stage.rglob("*")
             if path.is_file() and not path.is_symlink()}
    for name in required:
        require(name in paths, f"missing required application payload: {name}")

    packages = document.setdefault("packages", [])
    roots = [package for package in packages if package["SPDXID"].startswith("SPDXRef-DocumentRoot-")]
    require(len(roots) == 1, "expected one Syft directory-root package")
    root = roots[0]
    root.update(name="omacalendar", versionInfo=version, primaryPackagePurpose="APPLICATION",
                downloadLocation=f"https://github.com/brdweb/omacalendar/releases/tag/v{version}",
                licenseDeclared="MIT", licenseConcluded="NOASSERTION", copyrightText="NOASSERTION")
    scope = (f"Scope: {package_format} application payload regular files and explicitly pinned bundled libraries. "
             "Symlink entries are not represented as independent file contents. Excludes external host packages "
             "and the KDE Flatpak runtime/SDK, including their transitive dependencies. Separate build-package "
             "and Flatpak runtime receipts describe build environments, not a resolved runtime dependency graph. "
             "Source archive hashes in bundled package sourceInfo identify build inputs, not installed file hashes. "
             "NOASSERTION license conclusions are deliberate; consult shipped license texts.")
    root["comment"] = scope
    document["name"] = f"omacalendar-{version}-{package_format}-payload"
    creators = document["creationInfo"]["creators"]
    if TOOL not in creators:
        creators.append(TOOL)
    annotations = [item for item in document.get("annotations", []) if item.get("annotator") != TOOL]
    annotations.append({"annotationDate": document["creationInfo"]["created"], "annotationType": "OTHER",
                        "annotator": TOOL, "comment": scope})
    document["annotations"] = annotations

    old_files = {safe_name(item["fileName"]): item for item in document.get("files", [])}
    require(len(old_files) == len(document.get("files", [])), "duplicate normalized SPDX file paths")
    for name in old_files:
        require(name in paths, f"SPDX describes a missing/non-regular payload file: {name}")
    files = []
    for name, path in sorted(paths.items()):
        data = path.read_bytes()
        item = dict(old_files.get(name, {}))
        item.update(fileName=name, SPDXID=item.get("SPDXID", "SPDXRef-File-" + hashlib.sha256(name.encode()).hexdigest()[:24]),
                    checksums=[{"algorithm": algorithm, "checksumValue": hashlib.new(digest, data).hexdigest()}
                               for algorithm, digest in (("SHA1", "sha1"), ("SHA256", "sha256"))],
                    licenseConcluded="NOASSERTION", licenseInfoInFiles=["NOASSERTION"], copyrightText="NOASSERTION")
        files.append(item)
    document["files"] = files
    by_name = {item["fileName"]: item for item in files}
    bundled = []
    source_pins = pins(repository, package_format)
    for name in (() if package_format == "arch" else ("libical",) if package_format == "debian" else ("libical", "libsecret")):
        pin = source_pins[name]
        library_prefix = "usr/lib/omacalendar/" if package_format == "debian" else "lib/"
        library_files = [item for path, item in by_name.items()
                         if path.startswith(library_prefix + ("libical" if name == "libical" else "libsecret-1.so"))
                         and ".so." in path]
        require(library_files, f"missing bundled {name} shared-library payload")
        if name == "libical":
            require(library_prefix + "libical.so." + pin["version"] in by_name,
                    "bundled libical filename does not match pinned source version")
        if package_format == "debian":
            license_prefix = "usr/share/doc/omacalendar/libical/"
        else:
            license_prefix = "share/licenses/" + name + "/"
        license_files = [item for path, item in by_name.items() if path.startswith(license_prefix)]
        require(license_files, f"missing bundled {name} license payload")
        owned = library_files + license_files
        if name == "libsecret":
            owned.append(by_name["bin/secret-tool"])
        package_id = "SPDXRef-Bundled-" + name
        require(not any(item["SPDXID"] == package_id for item in packages), "input already finalized")
        package = {"name": name, "SPDXID": package_id, "versionInfo": pin["version"],
                   "primaryPackagePurpose": "LIBRARY", "downloadLocation": pin["url"],
                   "sourceInfo": f"Pinned source archive SHA256: {pin['sha256']}; from repository build recipe.",
                   "licenseDeclared": "NOASSERTION", "licenseConcluded": "NOASSERTION", "copyrightText": "NOASSERTION",
                   "comment": f"Bundled shared library; shipped license texts under {license_prefix}",
                   "filesAnalyzed": True, "licenseInfoFromFiles": ["NOASSERTION"]}
        packages.append(package)
        bundled.append((package, owned))

    relationships = document.setdefault("relationships", [])
    for package, owned in [(root, files)] + bundled:
        package["filesAnalyzed"] = True
        package["licenseInfoFromFiles"] = ["NOASSERTION"]
        sha1s = sorted(next(check["checksumValue"] for check in item["checksums"] if check["algorithm"] == "SHA1") for item in owned)
        package["packageVerificationCode"] = {"packageVerificationCodeValue": hashlib.sha1("".join(sha1s).encode()).hexdigest()}
        for item in owned:
            relationships.append({"spdxElementId": package["SPDXID"], "relationshipType": "CONTAINS", "relatedSpdxElement": item["SPDXID"]})
        if package is not root:
            relationships.append({"spdxElementId": root["SPDXID"], "relationshipType": "CONTAINS", "relatedSpdxElement": package["SPDXID"]})
            relationships.append({"spdxElementId": root["SPDXID"], "relationshipType": "DEPENDS_ON", "relatedSpdxElement": package["SPDXID"]})
    document["relationships"] = list({json.dumps(item, sort_keys=True): item for item in relationships}.values())
    ids = [document["SPDXID"]] + [item["SPDXID"] for item in packages + files]
    require(len(ids) == len(set(ids)), "duplicate SPDX identifiers")
    for relationship in document["relationships"]:
        require(relationship["spdxElementId"] in ids and relationship["relatedSpdxElement"] in ids,
                "dangling or unsupported external SPDX relationship")
    return document


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", type=Path, required=True)
    parser.add_argument("--stage", type=Path, required=True)
    parser.add_argument("--version", required=True)
    parser.add_argument("--format", choices=("arch", "debian", "flatpak"), required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    try:
        result = finalize(json.loads(args.input.read_text()), args.stage, args.version, args.format)
    except (ValueError, KeyError, StopIteration, OSError) as error:
        parser.exit(1, f"SBOM finalization failed: {error}\n")
    args.output.write_text(json.dumps(result, indent=2) + "\n")
    print(f"Finalized {args.format} payload SPDX: {len(result['packages'])} packages, {len(result['files'])} hashed regular files")


if __name__ == "__main__":
    main()
