#!/usr/bin/env python3
"""Exercise the C++ Arch package gate with a disposable staged tree.

The legal files are temporary test fixtures only. They are not repository
release inputs and this test never installs or publishes a package.
"""
from __future__ import annotations

import json
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "scripts/package-cpp-arch.sh"
PACKAGE_NAME = "melearner-bin-0.1.0-95-x86_64.pkg.tar.zst"


class ArchPackageTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory(prefix="melearner arch package ")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.stage = self.root / "stage"
        self.output = self.root / "output with spaces" / PACKAGE_NAME
        (self.stage / "usr/bin").mkdir(parents=True)
        (self.stage / "usr/share/applications").mkdir(parents=True)
        (self.stage / "usr/share/doc/melearner").mkdir(parents=True)
        (self.stage / "usr/share/licenses/melearner").mkdir(parents=True)
        (self.stage / "usr/lib/melearner").mkdir(parents=True)
        shutil.copy2(shutil.which("true") or "/usr/bin/true", self.stage / "usr/bin/melearner")
        (self.stage / "usr/bin/melearner").chmod(0o755)
        for name in ("libmpv.so.2", "libQt6Pdf.so.6"):
            shutil.copy2(shutil.which("true") or "/usr/bin/true", self.stage / "usr/lib/melearner" / name)
        shutil.copy2(
            ROOT / "packaging/arch/io.github.whitehades.melearner.desktop",
            self.stage / "usr/share/applications/io.github.whitehades.melearner.desktop",
        )
        self.write_json(
            "usr/share/doc/melearner/runtime-stage.json",
            {
                "schemaVersion": 1,
                "version": "0.1.0",
                "architecture": "x86_64",
                "releaseQualified": False,
                "privateLibraries": ["libmpv.so.2", "libQt6Pdf.so.6"],
                "systemRuntimeBoundary": [],
                "qtPluginGroups": [],
                "legalInputs": [
                    "LICENSE",
                    "THIRD_PARTY_NOTICES",
                    "melearner.spdx.json",
                    "runtime-lock.json",
                    "reference-profiles-v1.json",
                ],
            },
        )
        shutil.copy2(ROOT / "LICENSE", self.stage / "usr/share/licenses/melearner/LICENSE")
        (self.stage / "usr/share/doc/melearner/THIRD_PARTY_NOTICES").write_text(
            "temporary test notice\n", encoding="utf-8"
        )
        for name in ("melearner.spdx.json", "runtime-lock.json", "reference-profiles-v1.json"):
            self.write_json(f"usr/share/doc/melearner/{name}", {})

    def write_json(self, relative: str, value: object) -> None:
        path = self.stage / relative
        path.write_text(json.dumps(value) + "\n", encoding="utf-8")

    def run_package(self, output: Path | None = None) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [
                "bash",
                str(SCRIPT),
                "--stage-dir",
                str(self.stage),
                "--output",
                str(output or self.output),
            ],
            cwd=ROOT,
            capture_output=True,
            text=True,
            timeout=60,
        )

    def test_real_makepkg_contains_the_validated_stage(self) -> None:
        result = self.run_package()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertTrue(self.output.is_file())
        listing = subprocess.run(
            ["bsdtar", "--list", "--file", str(self.output)],
            check=True,
            capture_output=True,
            text=True,
        ).stdout.splitlines()
        self.assertIn("usr/bin/melearner", listing)
        self.assertIn(
            "usr/share/doc/melearner/runtime-stage.json",
            listing,
        )

    def test_existing_output_is_not_overwritten(self) -> None:
        first = self.run_package()
        self.assertEqual(first.returncode, 0, first.stdout + first.stderr)
        before = self.output.read_bytes()
        second = self.run_package()
        self.assertNotEqual(second.returncode, 0)
        self.assertIn("refusing to overwrite output", second.stderr)
        self.assertEqual(self.output.read_bytes(), before)

    def test_missing_legal_input_is_rejected_before_makepkg(self) -> None:
        (self.stage / "usr/share/doc/melearner/THIRD_PARTY_NOTICES").unlink()
        result = self.run_package()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("THIRD_PARTY_NOTICES", result.stderr)
        self.assertFalse(self.output.exists())

    def test_release_qualified_stage_is_rejected(self) -> None:
        metadata = self.stage / "usr/share/doc/melearner/runtime-stage.json"
        value = json.loads(metadata.read_text(encoding="utf-8"))
        value["releaseQualified"] = True
        metadata.write_text(json.dumps(value), encoding="utf-8")
        result = self.run_package()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("releaseQualified false", result.stderr)
        self.assertFalse(self.output.exists())


if __name__ == "__main__":
    required = ("bash", "makepkg", "bsdtar", "python3", "file", "readelf")
    missing = [tool for tool in required if shutil.which(tool) is None]
    if missing:
        raise SystemExit(f"required tools are missing: {', '.join(missing)}")
    unittest.main(verbosity=2)
