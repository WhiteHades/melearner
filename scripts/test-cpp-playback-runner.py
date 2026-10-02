#!/usr/bin/env python3
"""Exercise the real private X11/audio runner with small child processes.

Requires the same tools and optional PulseAudio overrides as test-cpp-playback.sh.
The child never opens the user's display or connects to their audio server.
"""
from __future__ import annotations

import json
import os
from pathlib import Path
import subprocess
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
RUNNER = ROOT / "scripts/test-cpp-playback.sh"


class PlaybackRunnerTests(unittest.TestCase):
    def run_child(self, source: str, **overrides: str) -> subprocess.CompletedProcess:
        environment = os.environ.copy()
        environment.update(overrides)
        return subprocess.run(
            ["bash", str(RUNNER), "--", sys.executable, "-c", source],
            cwd=ROOT, env=environment, capture_output=True, text=True, timeout=30,
        )

    def test_host_theme_and_gpu_overrides_do_not_enter_the_child(self):
        names = ["QT_QPA_PLATFORM", "QT_QPA_PLATFORMTHEME", "QT_STYLE_OVERRIDE",
                 "__GLX_VENDOR_LIBRARY_NAME", "LIBGL_ALWAYS_SOFTWARE", "LC_ALL",
                 "WAYLAND_DISPLAY", "PULSE_SERVER", "XDG_RUNTIME_DIR"]
        source = ("import json, os; print('CHILD_ENV=' + json.dumps("
                  + "{key: os.environ.get(key) for key in " + repr(names) + "}))")
        result = self.run_child(
            source, QT_QPA_PLATFORMTHEME="gtk3", QT_STYLE_OVERRIDE="host-style",
            __GLX_VENDOR_LIBRARY_NAME="nvidia", WAYLAND_DISPLAY="host-wayland",
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        lines = [line.removeprefix("CHILD_ENV=") for line in result.stdout.splitlines()
                 if line.startswith("CHILD_ENV=")]
        self.assertEqual(len(lines), 1, result.stdout)
        child = json.loads(lines[0])
        self.assertEqual(child["QT_QPA_PLATFORM"], "xcb")
        self.assertEqual(child["QT_QPA_PLATFORMTHEME"], "xdgdesktopportal")
        self.assertIsNone(child["QT_STYLE_OVERRIDE"])
        self.assertEqual(child["__GLX_VENDOR_LIBRARY_NAME"], "mesa")
        self.assertEqual(child["LIBGL_ALWAYS_SOFTWARE"], "1")
        self.assertEqual(child["LC_ALL"], "C")
        self.assertIsNone(child["WAYLAND_DISPLAY"])
        self.assertTrue(child["PULSE_SERVER"].startswith("unix:" + str(ROOT / ".tmp")))
        self.assertTrue(child["XDG_RUNTIME_DIR"].startswith(str(ROOT / ".tmp")))

    def test_child_failure_is_returned(self):
        result = self.run_child("raise SystemExit(37)")
        self.assertEqual(result.returncode, 37, result.stdout + result.stderr)

    def test_timeout_is_returned(self):
        result = self.run_child("import time; time.sleep(10)",
                                MELEARNER_PLAYBACK_TIMEOUT_SECONDS="1")
        self.assertEqual(result.returncode, 124, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
