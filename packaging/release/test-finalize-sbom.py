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
        prefix = "usr/"
        paths = [prefix + "bin/" + name for name in ("omacalendar", "omacalendard", "omacalendarctl")]
        paths += [prefix + "share/doc/README.md"]
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

    def test_missing_application_fails(self) -> None:
        self.payload("arch")
        (self.stage / "usr/bin/omacalendard").unlink()
        with self.assertRaisesRegex(ValueError, "missing required application"):
            self.finish("arch")

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
