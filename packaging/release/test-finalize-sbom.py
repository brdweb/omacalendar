#!/usr/bin/env python3
"""Offline regression tests for the release payload SPDX finalizer."""
from __future__ import annotations

import hashlib
import importlib.util
from pathlib import Path
import tempfile
import unittest

spec = importlib.util.spec_from_file_location("finalize_sbom", Path(__file__).with_name("finalize-sbom.py"))
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


def document() -> dict:
    return {"spdxVersion": "SPDX-2.3", "dataLicense": "CC0-1.0", "SPDXID": "SPDXRef-DOCUMENT",
            "name": "stage", "documentNamespace": "https://example.test/sbom/fixture",
            "creationInfo": {"created": "2026-09-08T00:00:00Z", "creators": ["Tool: syft-test"]},
            "packages": [{"name": "stage", "SPDXID": "SPDXRef-DocumentRoot-stage", "downloadLocation": "NOASSERTION"}],
            "relationships": [{"spdxElementId": "SPDXRef-DOCUMENT", "relationshipType": "DESCRIBES", "relatedSpdxElement": "SPDXRef-DocumentRoot-stage"}],
            "files": []}


class FinalizerTest(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory(prefix="omacalendar-sbom-test-")
        self.addCleanup(self.tmp.cleanup)
        self.stage = Path(self.tmp.name)

    def payload(self, package_format: str) -> None:
        prefix = "" if package_format == "flatpak" else "usr/"
        paths = [prefix + "bin/" + name for name in ("omacalendar", "omacalendard", "omacalendarctl")]
        paths += [prefix + "share/doc/README.md"]
        if package_format == "flatpak":
            paths += ["libexec/omacalendar/omacalendar-ui", "bin/secret-tool", "lib/libsecret-1.so.0.0.0", "share/licenses/libsecret/COPYING"]
        if package_format != "arch":
            paths += [("usr/lib/omacalendar/" if package_format == "debian" else "lib/") + "libical.so.4.0.5",
                      ("usr/share/doc/omacalendar/libical/" if package_format == "debian" else "share/licenses/libical/") + "LICENSE"]
        for name in paths:
            path = self.stage / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(("fixture: " + name).encode())

    def finish(self, package_format: str, source: dict | None = None) -> dict:
        return module.finalize(source or document(), self.stage, "1.0.0-rc.1", package_format)

    def test_native_payload_hashes_and_identity(self) -> None:
        self.payload("arch")
        source = document()
        source["files"] = [{"fileName": "usr/bin/omacalendar", "SPDXID": "SPDXRef-existing",
                            "checksums": [{"algorithm": "SHA1", "checksumValue": "0" * 40}]}]
        result = self.finish("arch", source)
        self.assertEqual(len(result["packages"]), 1)
        self.assertEqual(result["packages"][0]["name"], "omacalendar")
        self.assertEqual(result["packages"][0]["versionInfo"], "1.0.0-rc.1")
        self.assertEqual(len(result["files"]), 4)
        for item in result["files"]:
            digest = next(check["checksumValue"] for check in item["checksums"] if check["algorithm"] == "SHA256")
            self.assertEqual(digest, hashlib.sha256((self.stage / item["fileName"]).read_bytes()).hexdigest())
        self.assertIn("Excludes external host packages", result["annotations"][-1]["comment"])

    def test_bundled_dependencies_and_graph(self) -> None:
        for package_format, expected in (("debian", {"omacalendar", "libical"}), ("flatpak", {"omacalendar", "libical", "libsecret"})):
            with self.subTest(package_format=package_format):
                self.payload(package_format)
                result = self.finish(package_format)
                self.assertEqual({item["name"] for item in result["packages"]}, expected)
                for package in result["packages"]:
                    self.assertEqual(len(package["packageVerificationCode"]["packageVerificationCodeValue"]), 40)
                self.assertEqual(len([item for item in result["relationships"] if item["relationshipType"] == "DEPENDS_ON"]), len(expected) - 1)

    def test_missing_library_fails(self) -> None:
        self.payload("debian")
        (self.stage / "usr/lib/omacalendar/libical.so.4.0.5").unlink()
        with self.assertRaisesRegex(ValueError, "missing bundled libical"):
            self.finish("debian")

    def test_debian_recipe_pin_drift_fails(self) -> None:
        self.payload("debian")
        with tempfile.TemporaryDirectory(prefix="omacalendar-sbom-pins-") as directory:
            repository = Path(directory)
            (repository / "packaging/flatpak").mkdir(parents=True)
            (repository / "packaging/release").mkdir()
            (repository / "LICENSE").write_text("MIT License\n")
            manifest = module.REPOSITORY / "packaging/flatpak/org.omacalendar.OmaCalendar.json"
            (repository / "packaging/flatpak/org.omacalendar.OmaCalendar.json").write_text(manifest.read_text())
            recipe = module.REPOSITORY / "packaging/release/build-deb-release.sh"
            (repository / "packaging/release/build-deb-release.sh").write_text(
                recipe.read_text().replace("libical_version=4.0.5", "libical_version=4.0.4"))
            with self.assertRaisesRegex(ValueError, "pins disagree"):
                module.finalize(document(), self.stage, "1.0.0-rc.1", "debian", repository)

    def test_missing_application_fails(self) -> None:
        self.payload("arch")
        (self.stage / "usr/bin/omacalendard").unlink()
        with self.assertRaisesRegex(ValueError, "missing required application"):
            self.finish("arch")

    def test_wrong_library_version_fails(self) -> None:
        self.payload("debian")
        original = self.stage / "usr/lib/omacalendar/libical.so.4.0.5"
        original.rename(original.with_name("libical.so.4.0.4"))
        with self.assertRaisesRegex(ValueError, "does not match pinned"):
            self.finish("debian")

    def test_missing_license_fails(self) -> None:
        self.payload("flatpak")
        (self.stage / "share/licenses/libsecret/COPYING").unlink()
        with self.assertRaisesRegex(ValueError, "missing bundled libsecret license"):
            self.finish("flatpak")

    def test_input_cannot_escape_stage_or_invent_files(self) -> None:
        self.payload("arch")
        for name in ("../outside", "/etc/passwd", "absent"):
            with self.subTest(name=name), self.assertRaises(ValueError):
                source = document()
                source["files"] = [{"fileName": name, "SPDXID": "SPDXRef-bad"}]
                self.finish("arch", source)

    def test_symlinks_are_not_hashed_as_external_contents(self) -> None:
        self.payload("arch")
        (self.stage / "outside-link").symlink_to("/etc/passwd")
        self.assertNotIn("outside-link", {item["fileName"] for item in self.finish("arch")["files"]})

    def test_dangling_relationship_fails(self) -> None:
        self.payload("arch")
        source = document()
        source["relationships"].append({"spdxElementId": "SPDXRef-DOCUMENT", "relationshipType": "DESCRIBES", "relatedSpdxElement": "SPDXRef-missing"})
        with self.assertRaisesRegex(ValueError, "dangling"):
            self.finish("arch", source)


if __name__ == "__main__":
    unittest.main()
