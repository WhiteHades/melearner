#!/usr/bin/env python3
"""Exercise the Linux source installer without building or installing the app.

All commands and install destinations are isolated fixtures. Three integration
cases use the host's CTest, when available, on tiny disposable test projects.
These tests do not qualify the C++ application or any release package.
"""

import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest


INSTALLER = Path(__file__).with_name("install-cpp-linux.sh")
BASH = shutil.which("bash")
DIRNAME = shutil.which("dirname")
CTEST = shutil.which("ctest")
TOOLS = ("uname", "cmake", "ctest", "ninja", "c++", "pkg-config", "update-desktop-database")
MOCK = r'''
import json
import os
from pathlib import Path
import sys

name = Path(sys.argv[0]).name
args = sys.argv[1:]
with open(os.environ["MOCK_LOG"], "a", encoding="utf-8") as log:
    log.write(json.dumps([name, *args]) + "\n")
if name == "uname":
    print(os.environ.get("MOCK_OS", "Linux"))
    sys.exit(0)
if name == "pkg-config":
    missing = os.environ.get("MOCK_MISSING_LIBRARY")
    if missing and missing in args:
        print("Missing library: " + missing, file=sys.stderr)
        sys.exit(1)
    sys.exit(0)
phase = name
if name == "cmake":
    phase = "install" if "--install" in args else "build" if "--build" in args else "configure"
elif name == "ctest":
    phase = "test"
if os.environ.get("MOCK_FAIL_PHASE") == phase:
    sys.exit(17)
if phase == "install":
    prefix = Path(args[args.index("--prefix") + 1])
    # Never let a regression in the test harness write outside its fixture.
    if not prefix.resolve().is_relative_to(Path(os.environ["MOCK_ROOT"]).resolve()):
        raise RuntimeError("Non-fixture installation destination")
    mode = os.environ.get("MOCK_INSTALL_OUTPUT", "executable")
    if mode != "missing":
        executable = prefix / "bin" / "melearner"
        executable.parent.mkdir(parents=True, exist_ok=True)
        executable.write_text("#!/bin/sh\nexit 0\n", encoding="utf-8")
        executable.chmod(0o755 if mode == "executable" else 0o644)
'''


@unittest.skipUnless(sys.platform == "linux" and BASH and DIRNAME, "Linux, Bash and dirname required")
class InstallerTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="melearner-installer-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.repo = self.root / "repo with spaces"
        scripts = self.repo / "scripts"
        scripts.mkdir(parents=True)
        self.script = scripts / INSTALLER.name
        shutil.copyfile(INSTALLER, self.script)
        self.bin = self.root / "mock-bin"
        self.bin.mkdir()
        self.mock = self.root / "command.py"
        self.mock.write_text(f"#!{sys.executable} -S\n" + MOCK, encoding="utf-8")
        self.mock.chmod(0o755)
        for tool in TOOLS:
            (self.bin / tool).symlink_to(self.mock)
        (self.bin / "dirname").symlink_to(DIRNAME)
        self.home = self.root / "home with spaces"
        self.home.mkdir()
        self.prefix = self.home / ".local"
        self.log = self.root / "calls.jsonl"
        self.env = {"PATH": str(self.bin), "HOME": str(self.home), "LC_ALL": "C",
                    "MOCK_LOG": str(self.log), "MOCK_ROOT": str(self.root)}

    def run_installer(self, *args, **environment):
        self.log.unlink(missing_ok=True)
        env = self.env | environment
        return subprocess.run([BASH, str(self.script), *map(str, args)],
                              cwd=self.root, env=env, capture_output=True,
                              text=True, timeout=10, check=False)

    def calls(self):
        if not self.log.exists():
            return []
        return [json.loads(line) for line in self.log.read_text(encoding="utf-8").splitlines()]

    def assert_failure(self, result):
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertNotIn("Installed melearner", result.stdout)

    def assert_not_installed(self, result):
        self.assert_failure(result)
        self.assertFalse(any(call[:2] == ["cmake", "--install"] for call in self.calls()))

    def test_default_install_orders_configure_build_test_install(self):
        result = self.run_installer()
        self.assertEqual(result.returncode, 0, result.stderr)
        relevant = [c for c in self.calls() if c[0] in ("cmake", "ctest")]
        self.assertEqual(relevant, [
            ["cmake", "--preset", "linux-release"],
            ["cmake", "--build", "--preset", "linux-release", "--parallel", "4"],
            ["ctest", "--preset", "linux-release", "--no-tests=error"],
            ["cmake", "--install", "build/cpp-release", "--prefix", str(self.prefix)]])
        self.assertTrue(os.access(self.prefix / "bin/melearner", os.X_OK))

    def test_custom_prefix_preserves_spaces_and_shell_characters(self):
        prefix = self.root / "custom prefix; $(not-a-command) ' quoted"
        result = self.run_installer(prefix)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn(["cmake", "--install", "build/cpp-release", "--prefix", str(prefix)], self.calls())
        self.assertIn(["update-desktop-database", str(prefix / "share/applications")], self.calls())
        self.assertIn("Run: ", result.stdout)
        command = result.stdout.split("Run: ", 1)[1].strip()
        launched = subprocess.run([BASH, "-c", command], env=self.env, capture_output=True,
                                  text=True, timeout=10, check=False)
        self.assertEqual(launched.returncode, 0, launched.stderr)

    def test_non_linux_rejected(self):
        result = self.run_installer(MOCK_OS="Darwin")
        self.assert_not_installed(result)
        self.assertIn("Linux only", result.stderr)

    def test_relative_and_empty_prefixes_rejected_before_build(self):
        for prefix in ("relative/path", ""):
            with self.subTest(prefix=prefix):
                result = self.run_installer(prefix)
                self.assert_not_installed(result)
                self.assertIn("absolute path", result.stderr)
                self.assertEqual(self.calls(), [])

    def test_extra_arguments_rejected(self):
        result = self.run_installer(self.prefix, "unexpected")
        self.assert_not_installed(result)
        self.assertIn("Usage:", result.stderr)
        self.assertEqual(self.calls(), [])

    def test_help_needs_no_build_tools_or_home(self):
        self.env.pop("HOME")
        for option in ("-h", "--help"):
            with self.subTest(option=option):
                result = self.run_installer(option)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertIn("Usage:", result.stdout)
                self.assertEqual(self.calls(), [])

    def test_missing_or_empty_home_requires_explicit_prefix(self):
        self.env.pop("HOME")
        for overrides in ({}, {"HOME": ""}):
            with self.subTest(overrides=overrides):
                result = self.run_installer(**overrides)
                self.assert_not_installed(result)
                self.assertIn("HOME", result.stderr)
                self.assertEqual(self.calls(), [])

    def test_explicit_prefix_works_without_home(self):
        self.env.pop("HOME")
        result = self.run_installer(self.prefix)
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_destdir_rejected_before_any_work(self):
        result = self.run_installer(DESTDIR=str(self.root / "stage"))
        self.assert_not_installed(result)
        self.assertIn("DESTDIR", result.stderr)
        self.assertEqual(self.calls(), [])

    def test_missing_build_tools_rejected_before_configure(self):
        for tool in ("cmake", "ctest", "ninja", "c++", "pkg-config"):
            with self.subTest(tool=tool):
                path = self.bin / tool
                path.unlink()
                try:
                    result = self.run_installer()
                    self.assert_not_installed(result)
                    self.assertIn("Missing build tool: " + tool, result.stderr)
                    self.assertFalse(any(c[0] == "cmake" for c in self.calls()))
                finally:
                    path.symlink_to(self.mock)

    def test_missing_libraries_including_qt_test_rejected_before_configure(self):
        for library in ("Qt6Widgets", "Qt6OpenGLWidgets", "Qt6Network", "Qt6Pdf", "Qt6Test",
                        "sqlite3", "mpv", "libzip", "md4c"):
            with self.subTest(library=library):
                result = self.run_installer(MOCK_MISSING_LIBRARY=library)
                self.assert_not_installed(result)
                self.assertIn(library, result.stderr)
                self.assertFalse(any(c[0] == "cmake" for c in self.calls()))

    def test_pipeline_failure_never_reports_success(self):
        for phase in ("configure", "build", "test", "install"):
            with self.subTest(phase=phase):
                result = self.run_installer(MOCK_FAIL_PHASE=phase)
                self.assertEqual(result.returncode, 17, result.stderr)
                self.assert_failure(result)
                if phase != "install":
                    self.assert_not_installed(result)
                self.assertFalse(any(c[0] == "update-desktop-database" for c in self.calls()))

    def test_desktop_database_update_is_optional(self):
        (self.bin / "update-desktop-database").unlink()
        result = self.run_installer()
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_desktop_database_failure_does_not_report_success(self):
        result = self.run_installer(MOCK_FAIL_PHASE="update-desktop-database")
        self.assert_failure(result)
        self.assertTrue((self.prefix / "bin/melearner").exists())

    def test_missing_installed_executable_rejected(self):
        result = self.run_installer(MOCK_INSTALL_OUTPUT="missing")
        self.assert_failure(result)
        self.assertIn("did not produce an executable", result.stderr)

    def test_nonexecutable_installed_file_rejected(self):
        result = self.run_installer(MOCK_INSTALL_OUTPUT="nonexecutable")
        self.assert_failure(result)
        self.assertIn("did not produce an executable", result.stderr)

    def test_failure_preserves_existing_installation_and_data(self):
        executable = self.prefix / "bin/melearner"
        executable.parent.mkdir(parents=True)
        executable.write_text("existing executable", encoding="utf-8")
        data = self.home / "library-v1.sqlite3"
        data.write_bytes(b"existing fixture data")
        result = self.run_installer(MOCK_FAIL_PHASE="test")
        self.assert_not_installed(result)
        self.assertEqual(executable.read_text(encoding="utf-8"), "existing executable")
        self.assertEqual(data.read_bytes(), b"existing fixture data")

    def use_real_ctest(self, test_file):
        (self.bin / "ctest").unlink()
        (self.bin / "ctest").symlink_to(CTEST)
        build = self.repo / "build/cpp-release"
        build.mkdir(parents=True)
        (build / "CTestTestfile.cmake").write_text(test_file, encoding="utf-8")
        presets = {"version": 2, "configurePresets": [
            {"name": "linux-release", "generator": "Ninja", "binaryDir": "${sourceDir}/build/cpp-release"}],
            "testPresets": [{"name": "linux-release", "configurePreset": "linux-release"}]}
        (self.repo / "CMakePresets.json").write_text(json.dumps(presets), encoding="utf-8")

    @unittest.skipUnless(CTEST, "CTest unavailable")
    def test_real_ctest_empty_suite_blocks_installation(self):
        self.use_real_ctest("")
        self.assert_not_installed(self.run_installer())

    @unittest.skipUnless(CTEST, "CTest unavailable")
    def test_real_ctest_failed_test_blocks_installation(self):
        self.use_real_ctest(f'add_test(intentional_failure "{BASH}" "-c" "exit 1")\n')
        self.assert_not_installed(self.run_installer())

    @unittest.skipUnless(CTEST, "CTest unavailable")
    def test_real_ctest_passing_test_allows_installation(self):
        self.use_real_ctest(f'add_test(intentional_success "{BASH}" "-c" "exit 0")\n')
        result = self.run_installer()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue((self.prefix / "bin/melearner").exists())


if __name__ == "__main__":
    unittest.main(verbosity=2)
