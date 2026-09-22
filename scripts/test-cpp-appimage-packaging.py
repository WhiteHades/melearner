#!/usr/bin/env python3
"""Exercise AppImage packaging orchestration with isolated CMake/plugin stubs.

The legal files and staged tree are disposable fixtures. This test never runs
the real AppImage tool, installs anything, or publishes a release asset.
"""
from __future__ import annotations

import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "scripts/package-cpp-appimage.sh"
APPIMAGE_NAME = "melearner_0.1.9_amd64.AppImage"
TASK_TMP = ROOT / ".tmp/task"


class AppImagePackageTests(unittest.TestCase):
    def setUp(self) -> None:
        TASK_TMP.mkdir(parents=True, exist_ok=True)
        self.temp = tempfile.TemporaryDirectory(
            prefix="appimage-package-", dir=TASK_TMP
        )
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.build = self.root / "build with spaces"
        self.legal = self.root / "legal with spaces"
        self.tools = self.root / "tools"
        self.output = self.root / "output with spaces" / APPIMAGE_NAME
        self.plugin_log = self.root / "plugin.json"
        self.cmake_log = self.root / "cmake-args.txt"
        self.build.mkdir(parents=True)
        self.legal.mkdir(parents=True)
        self.tools.mkdir(parents=True)
        (self.build / "CMakeCache.txt").write_text(
            "CMAKE_PROJECT_VERSION:STATIC=0.1.9\n", encoding="utf-8"
        )
        shutil.copy2(ROOT / "LICENSE", self.legal / "LICENSE")
        (self.legal / "THIRD_PARTY_NOTICES").write_text(
            "temporary third-party notice\n", encoding="utf-8"
        )
        for name in (
            "melearner.spdx.json",
            "runtime-lock.json",
            "reference-profiles-v1.json",
        ):
            (self.legal / name).write_text("{}\n", encoding="utf-8")
        self.write_tool("cmake", self.cmake_stub())
        self.write_tool("linuxdeploy-plugin-appimage", self.plugin_stub())

    def write_tool(self, name: str, contents: str) -> None:
        path = self.tools / name
        path.write_text(contents, encoding="utf-8")
        path.chmod(0o755)

    def cmake_stub(self) -> str:
        return r'''#!/usr/bin/env python3
import json
import os
from pathlib import Path
import shutil
import sys

args = sys.argv[1:]
Path(os.environ["APPIMAGE_CMAKE_LOG"]).write_text("\n".join(args) + "\n", encoding="utf-8")
stage = Path(next(value.split("=", 1)[1] for value in args if value.startswith("-DMELEARNER_STAGE_DIR=")))
legal = Path(next(value.split("=", 1)[1] for value in args if value.startswith("-DMELEARNER_LEGAL_ROOT=")))
(stage / "usr/bin").mkdir(parents=True)
(stage / "usr/share/applications").mkdir(parents=True)
(stage / "usr/share/pixmaps").mkdir(parents=True)
(stage / "usr/share/doc/melearner").mkdir(parents=True)
shutil.copy2("/usr/bin/true", stage / "usr/bin/melearner")
(stage / "usr/bin/melearner").chmod(0o755)
shutil.copy2(os.environ["APPIMAGE_GENERIC_DESKTOP"], stage / "usr/share/applications/io.github.whitehades.melearner.desktop")
shutil.copy2(os.environ["APPIMAGE_ICON"], stage / "usr/share/pixmaps/io.github.whitehades.melearner.png")
(stage / "usr/share/doc/melearner/runtime-stage.json").write_text(json.dumps({
    "schemaVersion": 1,
    "version": "0.1.9",
    "architecture": "x86_64",
    "releaseQualified": False,
    "privateLibraries": [],
    "systemRuntimeBoundary": [],
    "qtPluginGroups": [],
    "legalInputs": ["LICENSE", "THIRD_PARTY_NOTICES", "melearner.spdx.json", "runtime-lock.json", "reference-profiles-v1.json"],
}) + "\n", encoding="utf-8")
for name in ("THIRD_PARTY_NOTICES", "melearner.spdx.json", "runtime-lock.json", "reference-profiles-v1.json"):
    shutil.copy2(legal / name, stage / "usr/share/doc/melearner" / name)
if os.environ.get("APPIMAGE_STAGE_MODE") == "browser":
    (stage / "usr/share/webview").mkdir(parents=True)
    (stage / "usr/share/webview/index.html").write_text("fixture", encoding="utf-8")
if os.environ.get("APPIMAGE_STAGE_MODE") == "updater":
    (stage / "usr/share/melearner").mkdir(parents=True)
    (stage / "usr/share/melearner/updater").write_text("fixture", encoding="utf-8")
'''

    def plugin_stub(self) -> str:
        return r'''#!/usr/bin/env python3
import json
import os
from pathlib import Path
import shutil
import sys

if sys.argv[1:] == ["--plugin-type"]:
    print("output")
    raise SystemExit(0)
if sys.argv[1:] == ["--plugin-api-version"]:
    print("0")
    raise SystemExit(0)
appdir = Path(next(value.split("=", 1)[1] for value in sys.argv[1:] if value.startswith("--appdir=")))
Path(os.environ["APPIMAGE_PLUGIN_LOG"]).write_text(json.dumps({
    "appdir": str(appdir),
    "app_run": os.readlink(appdir / "AppRun"),
    "desktop": os.readlink(appdir / "io.github.whitehades.melearner.appimage.desktop"),
    "icon": os.readlink(appdir / "io.github.whitehades.melearner.png"),
    "output": os.environ["LDAI_OUTPUT"],
    "version": os.environ.get("LDAI_VERSION"),
    "no_appstream": os.environ.get("LDAI_NO_APPSTREAM"),
    "updates": {key: os.environ.get(key) for key in (
        "LDAI_UPDATE_INFORMATION", "LDAI_GUESS_UPDATE_INFORMATION", "LDAI_SIGN", "LDAI_SIGN_KEY"
    )},
}) + "\n", encoding="utf-8")
output = Path(os.environ["LDAI_OUTPUT"])
shutil.copy2("/usr/bin/true", output)
output.chmod(0o755)
'''

    def run_package(
        self, mode: str = "ok", **extra_env: str
    ) -> subprocess.CompletedProcess[str]:
        env = os.environ.copy()
        env["PATH"] = f"{self.tools}{os.pathsep}{env['PATH']}"
        env.update(
            APPIMAGE_CMAKE_LOG=str(self.cmake_log),
            APPIMAGE_PLUGIN_LOG=str(self.plugin_log),
            APPIMAGE_GENERIC_DESKTOP=str(
                ROOT / "packaging/linux/io.github.whitehades.melearner.desktop"
            ),
            APPIMAGE_ICON=str(ROOT / "cpp-app/assets/melearner-logo.png"),
            APPIMAGE_STAGE_MODE=mode,
            LDAI_UPDATE_INFORMATION="must-not-leak",
            LDAI_GUESS_UPDATE_INFORMATION="1",
            LDAI_SIGN="1",
            LDAI_SIGN_KEY="must-not-leak",
        )
        env.update(extra_env)
        return subprocess.run(
            [
                "bash",
                str(SCRIPT),
                "--build-dir",
                str(self.build),
                "--legal-root",
                str(self.legal),
                "--output",
                str(self.output),
            ],
            cwd=ROOT,
            env=env,
            capture_output=True,
            text=True,
            timeout=60,
        )

    def test_stages_and_publishes_appimage_without_update_inputs(self) -> None:
        result = self.run_package()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertTrue(self.output.is_file())
        self.assertTrue(os.access(self.output, os.X_OK))
        plugin = json.loads(self.plugin_log.read_text(encoding="utf-8"))
        self.assertEqual(plugin["app_run"], "usr/bin/melearner")
        self.assertEqual(
            plugin["desktop"],
            "usr/share/applications/io.github.whitehades.melearner.appimage.desktop",
        )
        self.assertEqual(
            plugin["icon"], "usr/share/pixmaps/io.github.whitehades.melearner.png"
        )
        self.assertEqual(plugin["version"], "0.1.9")
        self.assertEqual(plugin["no_appstream"], "1")
        self.assertEqual(plugin["updates"], {key: None for key in plugin["updates"]})
        self.assertIn("-P", self.cmake_log.read_text(encoding="utf-8").splitlines())

    def test_missing_canonical_legal_input_is_rejected_before_staging(self) -> None:
        (self.legal / "melearner.spdx.json").unlink()
        result = self.run_package()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("SPDX manifest", result.stderr)
        self.assertFalse(self.output.exists())
        self.assertFalse(self.cmake_log.exists())
        self.assertFalse(self.plugin_log.exists())

    def test_existing_output_is_not_overwritten(self) -> None:
        self.output.parent.mkdir(parents=True)
        self.output.write_bytes(b"existing AppImage")
        result = self.run_package()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("refusing to overwrite output", result.stderr)
        self.assertEqual(self.output.read_bytes(), b"existing AppImage")

    def test_browser_and_updater_artifacts_are_rejected(self) -> None:
        for mode, expected in (
            ("browser", "superseded runtime asset staged"),
            ("updater", "auto-updater artifact staged"),
        ):
            with self.subTest(mode=mode):
                result = self.run_package(mode=mode)
                self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertIn(expected, result.stderr)
                self.assertFalse(self.output.exists())
                self.plugin_log.unlink(missing_ok=True)
                self.cmake_log.unlink(missing_ok=True)

    def test_destdir_is_rejected(self) -> None:
        result = self.run_package(DESTDIR=str(self.root / "external-stage"))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("DESTDIR must be empty", result.stderr)
        self.assertFalse(self.output.exists())


if __name__ == "__main__":
    required = (
        "bash",
        "cmake",
        "env",
        "file",
        "find",
        "grep",
        "install",
        "ln",
        "mktemp",
        "patchelf",
        "readelf",
        "readlink",
    )
    missing = [tool for tool in required if shutil.which(tool) is None]
    if os.uname().sysname != "Linux" or missing:
        raise SystemExit(
            f"Linux and these tools are required: {', '.join(required)}; missing: {missing}"
        )
    unittest.main(verbosity=2)
