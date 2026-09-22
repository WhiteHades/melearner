#!/usr/bin/env python3
"""Exercise the Windows build entrypoint without a Windows toolchain.

The command stubs create disposable executable targets and record every call.
These tests cover the entrypoint's ordering and playback opt-in contract. They
do not compile C++, load Qt, or claim Windows qualification.
"""

from __future__ import annotations

import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest


ENTRYPOINT = Path(__file__).with_name("build-cpp-windows.sh")
ROOT = ENTRYPOINT.parents[1]
BASH = shutil.which("bash")

MOCK = r'''#!/usr/bin/env python3
import json
import os
from pathlib import Path
import sys

name = Path(sys.argv[0]).name
args = sys.argv[1:]
with open(os.environ["MOCK_LOG"], "a", encoding="utf-8") as log:
    log.write(json.dumps([name, *args]) + "\n")

if name == "pkg-config" or name in ("gcc", "g++", "ninja"):
    raise SystemExit(0)

if name == "cmake":
    if "--build" in args:
        phase = "build"
        build_dir = Path(args[args.index("--build") + 1])
    else:
        phase = "configure"
        build_dir = Path(args[args.index("-B") + 1])
        build_dir.mkdir(parents=True, exist_ok=True)
    if os.environ.get("MOCK_FAIL_PHASE") == phase:
        raise SystemExit(17)
    if phase == "build":
        build_dir.mkdir(parents=True, exist_ok=True)
        for target in ("playback_render_test", "main_playback_test"):
            executable = build_dir / (target + ".exe")
            executable.write_text(
                "#!/bin/sh\n"
                "test \"$PWD\" = \"$EXPECTED_PLAYBACK_CWD\" || exit 19\n"
                "printf '%s:%s:%s\\n' '" + target + "' "
                "\"${QT_QPA_PLATFORM-unset}\" \"$PWD\" >> \"$PLAYBACK_LOG\"\n",
                encoding="utf-8",
            )
            executable.chmod(0o755)
    raise SystemExit(0)

if name == "ctest":
    if os.environ.get("MOCK_FAIL_PHASE") == "test":
        raise SystemExit(17)
    raise SystemExit(0)

raise SystemExit(0)
'''


@unittest.skipUnless(sys.platform == "linux" and BASH, "Linux and Bash required")
class WindowsBuildEntrypointTests(unittest.TestCase):
    def setUp(self) -> None:
        self.task_root = ROOT / ".tmp" / "windows-build-entrypoint"
        self.task_root.mkdir(parents=True, exist_ok=True)
        self.addCleanup(self._remove_task_root)
        temporary = tempfile.TemporaryDirectory(prefix="fixture-", dir=self.task_root)
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.repo = self.root / "repo with spaces"
        scripts = self.repo / "scripts"
        scripts.mkdir(parents=True)
        self.script = scripts / ENTRYPOINT.name
        shutil.copyfile(ENTRYPOINT, self.script)
        self.bin = self.root / "mock-bin"
        self.bin.mkdir()
        self.mock = self.root / "command.py"
        self.mock.write_text(MOCK, encoding="utf-8")
        self.mock.chmod(0o755)
        for tool in ("cmake", "ctest", "ninja", "gcc", "g++", "pkg-config"):
            (self.bin / tool).symlink_to(self.mock)
        self.log = self.root / "calls.jsonl"
        self.playback_log = self.root / "playback.log"
        self.build_dir = self.repo / "build/cpp-windows"
        self.env = os.environ.copy()
        self.env.update({
            "PATH": str(self.bin) + os.pathsep + self.env.get("PATH", ""),
            "MSYSTEM": "UCRT64",
            "MOCK_LOG": str(self.log),
            "PLAYBACK_LOG": str(self.playback_log),
            "EXPECTED_PLAYBACK_CWD": str(self.build_dir),
            "LC_ALL": "C",
        })
        self.env.pop("MELEARNER_BUILD_JOBS", None)

    def _remove_task_root(self) -> None:
        try:
            self.task_root.rmdir()
        except OSError:
            pass

    def run_entrypoint(self, *args: str, **environment: str) -> subprocess.CompletedProcess:
        self.log.unlink(missing_ok=True)
        self.playback_log.unlink(missing_ok=True)
        env = self.env | environment
        return subprocess.run(
            [BASH, str(self.script), *args],
            cwd=self.root,
            env=env,
            capture_output=True,
            text=True,
            timeout=10,
            check=False,
        )

    def calls(self) -> list[list[str]]:
        if not self.log.exists():
            return []
        return [json.loads(line) for line in self.log.read_text(encoding="utf-8").splitlines()]

    def test_wrong_msys_environment_is_rejected_before_tools(self) -> None:
        result = self.run_entrypoint(MSYSTEM="MINGW64")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("UCRT64", result.stderr)
        self.assertEqual(self.calls(), [])

    def test_invalid_jobs_are_rejected_from_option_and_environment(self) -> None:
        for args, environment in (
            (("--jobs", "0"), {}),
            (("--jobs", "workers"), {}),
            ((), {"MELEARNER_BUILD_JOBS": "0"}),
            ((), {"MELEARNER_BUILD_JOBS": "workers"}),
        ):
            with self.subTest(args=args, environment=environment):
                result = self.run_entrypoint(*args, **environment)
                self.assertNotEqual(result.returncode, 0)
                if "MELEARNER_BUILD_JOBS" in environment:
                    self.assertIn("positive integer", result.stderr)
                self.assertEqual(self.calls(), [])

    def test_failed_build_does_not_start_ctest_or_playback(self) -> None:
        result = self.run_entrypoint(MOCK_FAIL_PHASE="build")
        self.assertEqual(result.returncode, 17, result.stderr)
        self.assertEqual([call[0] for call in self.calls()], ["pkg-config", "cmake", "cmake"])
        self.assertFalse(any(call[0] == "ctest" for call in self.calls()))
        self.assertFalse(self.playback_log.exists())

    def test_failed_ctest_does_not_start_playback(self) -> None:
        result = self.run_entrypoint("--run-playback", MOCK_FAIL_PHASE="test")
        self.assertEqual(result.returncode, 17, result.stderr)
        self.assertTrue(any(call[0] == "ctest" for call in self.calls()))
        self.assertFalse(self.playback_log.exists())

    def test_default_runs_registered_tests_without_playback_targets(self) -> None:
        result = self.run_entrypoint(MELEARNER_BUILD_JOBS="7")
        self.assertEqual(result.returncode, 0, result.stderr)
        calls = self.calls()
        self.assertEqual([call[0] for call in calls], ["pkg-config", "cmake", "cmake", "ctest"])
        self.assertIn("--parallel", calls[2])
        self.assertEqual(calls[2][calls[2].index("--parallel") + 1], "7")
        self.assertNotIn("-E", calls[3])
        self.assertFalse(self.playback_log.exists())

    def test_opt_in_runs_both_playback_targets_from_build_directory(self) -> None:
        result = self.run_entrypoint("--run-playback", QT_QPA_PLATFORM="windows")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(
            self.playback_log.read_text(encoding="utf-8").splitlines(),
            [
                f"playback_render_test:windows:{self.build_dir}",
                f"main_playback_test:windows:{self.build_dir}",
            ],
        )


if __name__ == "__main__":
    unittest.main(verbosity=2)
