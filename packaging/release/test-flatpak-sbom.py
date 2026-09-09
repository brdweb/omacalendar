#!/usr/bin/env python3
"""Offline regressions for checking SPDX against exported Flatpak content."""
from __future__ import annotations

import hashlib
import importlib.util
from pathlib import Path
import tempfile
import unittest

spec = importlib.util.spec_from_file_location("verify_flatpak_sbom", Path(__file__).with_name("verify-flatpak-sbom.py"))
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
VERSION = "1.0.0-rc.3"


class BundleInventoryTest(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory(prefix="omacalendar-flatpak-sbom-test-")
        self.addCleanup(self.tmp.cleanup)
        self.stage = Path(self.tmp.name)
        self.files = []
        for number, name in enumerate(("bin/omacalendar", "lib/libical.so.4.0.5", "lib/libsecret-1.so.0.0.0")):
            path = self.stage / name
            path.parent.mkdir(parents=True, exist_ok=True)
            data = f"fixture {name}".encode()
            path.write_bytes(data)
            self.files.append({"fileName": name, "SPDXID": f"SPDXRef-File-{number}", "checksums": [
                {"algorithm": "SHA1", "checksumValue": hashlib.sha1(data).hexdigest()},
                {"algorithm": "SHA256", "checksumValue": hashlib.sha256(data).hexdigest()}]})
        self.document = {"spdxVersion": "SPDX-2.3", "name": f"omacalendar-{VERSION}-flatpak-payload",
                         "files": self.files, "packages": [], "relationships": []}
        for name, package_id, owned in (("omacalendar", "SPDXRef-DocumentRoot-fixture", self.files),
                                         ("libical", "SPDXRef-Bundled-libical", [self.files[1]]),
                                         ("libsecret", "SPDXRef-Bundled-libsecret", [self.files[2]])):
            code = hashlib.sha1("".join(sorted(item["checksums"][0]["checksumValue"] for item in owned)).encode()).hexdigest()
            self.document["packages"].append({"name": name, "SPDXID": package_id, "versionInfo": VERSION,
                                               "filesAnalyzed": True, "packageVerificationCode": {"packageVerificationCodeValue": code}})
            self.document["relationships"] += [{"spdxElementId": package_id, "relationshipType": "CONTAINS",
                                                 "relatedSpdxElement": item["SPDXID"]} for item in owned]

    def verify(self) -> int:
        return module.verify_payload(self.document, self.stage, VERSION)

    def test_exact_payload_passes(self) -> None:
        self.assertEqual(self.verify(), 3)

    def test_preexport_locale_overclaim_fails(self) -> None:
        self.files.append({"fileName": "share/runtime/locale/de/share/de/LC_MESSAGES/libsecret.mo",
                           "SPDXID": "SPDXRef-File-excluded-locale", "checksums": []})
        with self.assertRaisesRegex(ValueError, "bundle inventory mismatch: missing=.*libsecret.mo"):
            self.verify()

    def test_unlisted_shipped_file_fails(self) -> None:
        (self.stage / "unlisted").write_text("shipped but unaccounted")
        with self.assertRaisesRegex(ValueError, "unlisted=.*unlisted"):
            self.verify()

    def test_modified_file_fails(self) -> None:
        (self.stage / "bin/omacalendar").write_text("modified")
        with self.assertRaisesRegex(ValueError, "SHA1 mismatch"):
            self.verify()

    def test_wrong_sha256_fails(self) -> None:
        self.files[0]["checksums"][1]["checksumValue"] = "0" * 64
        with self.assertRaisesRegex(ValueError, "SHA256 mismatch"):
            self.verify()

    def test_missing_package_file_coverage_fails(self) -> None:
        self.document["relationships"].pop(0)
        with self.assertRaisesRegex(ValueError, "does not contain every shipped file"):
            self.verify()

    def test_wrong_package_code_fails(self) -> None:
        self.document["packages"][1]["packageVerificationCode"]["packageVerificationCodeValue"] = "0" * 40
        with self.assertRaisesRegex(ValueError, "package verification code mismatch: libical"):
            self.verify()

    def test_unsafe_path_fails(self) -> None:
        self.files[0]["fileName"] = "../outside"
        with self.assertRaisesRegex(ValueError, "unsafe SPDX file path"):
            self.verify()

    def test_duplicate_file_fails(self) -> None:
        self.files.append(dict(self.files[0]))
        with self.assertRaisesRegex(ValueError, "duplicate SPDX file"):
            self.verify()

    def test_symlink_does_not_invent_regular_content(self) -> None:
        (self.stage / "symlink").symlink_to("bin/omacalendar")
        self.assertEqual(self.verify(), 3)


if __name__ == "__main__":
    unittest.main()
