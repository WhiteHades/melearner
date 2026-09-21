#!/usr/bin/env python3
"""Test diagnostic archive orchestration using real CMake cache and tar/zstd.

The CMake project is disposable. Staging is a controlled substitute and does
not build melearner, exercise Qt/mpv, or qualify an installed release package.
"""
from __future__ import annotations

import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
NAME = "melearner-0.1.0-linux-x86_64.tar.zst"
REAL_CMAKE = shutil.which("cmake")
REAL_TAR = shutil.which("tar")


class ArchiveTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory(prefix="melearner archive ")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.repo = self.root / "source with spaces"
        self.tools = self.root / "tools"
        self.build = self.root / "build with spaces"
        self.legal = self.root / "legal with spaces"
        self.output = self.root / "output with spaces" / NAME
        for path in (self.repo / "scripts", self.tools, self.legal):
            path.mkdir(parents=True)
        shutil.copy2(ROOT / "scripts/package-cpp-linux.sh", self.repo / "scripts")
        (self.repo / "CMakeLists.txt").write_text(
            "cmake_minimum_required(VERSION 3.20)\n"
            "project(melearner VERSION 0.1.0 LANGUAGES NONE)\n", encoding="utf-8")
        subprocess.run([REAL_CMAKE, "-S", str(self.repo), "-B", str(self.build)],
                       check=True, capture_output=True, text=True, timeout=20)
        self.env = os.environ.copy()
        self.env.update(PATH=f"{self.tools}{os.pathsep}{os.environ['PATH']}",
                        REAL_CMAKE=REAL_CMAKE, REAL_TAR=REAL_TAR,
                        ARCHIVE_TEST_OUTPUT=str(self.output),
                        ARCHIVE_TEST_LOG=str(self.root / "stage.log"),
                        ARCHIVE_TEST_MODE="ok")
        self.env.pop("DESTDIR", None)
        # These dependencies are preflight-only here; any unexpected call fails.
        for name in ("file", "readelf", "patchelf"):
            self.tool(name, "#!/bin/sh\nexit 99\n")
        self.tool("cmake", '''#!/usr/bin/env -S python3 -S
import os, pathlib, subprocess, sys
if "-P" not in sys.argv:
    sys.exit(subprocess.call([os.environ["REAL_CMAKE"], *sys.argv[1:]]))
pathlib.Path(os.environ["ARCHIVE_TEST_LOG"]).write_text("staging invoked")
mode = os.environ["ARCHIVE_TEST_MODE"]
if mode == "stage-failure":
    sys.exit(17)
stage = pathlib.Path(next(x.split("=", 1)[1] for x in sys.argv
                          if x.startswith("-DMELEARNER_STAGE_DIR=")))
(stage / "usr/bin").mkdir(parents=True)
if mode != "missing-binary":
    binary = stage / "usr/bin/melearner"
    binary.write_text("#!/bin/sh\\nexit 0\\n")
    binary.chmod(0o755)
if mode == "large":
    docs = stage / "usr/share/doc/melearner"
    docs.mkdir(parents=True)
    for i in range(4000):
        (docs / f"fixture-{i:05d}.txt").write_text("archive fixture\\n")
if mode == "dot-prefix":
    (stage / "..notes").write_text("not a parent path")
output = pathlib.Path(os.environ["ARCHIVE_TEST_OUTPUT"])
if mode == "racing-file":
    output.write_bytes(b"concurrent writer")
elif mode == "racing-directory":
    output.mkdir()
elif mode == "racing-symlink":
    output.symlink_to("missing-target")
''')

    def tool(self, name: str, contents: str) -> None:
        path = self.tools / name
        path.write_text(contents, encoding="utf-8")
        path.chmod(0o755)

    def run_package(self, *extra: str, mode: str = "ok") -> subprocess.CompletedProcess:
        self.env["ARCHIVE_TEST_MODE"] = mode
        result = subprocess.run(
            ["bash", str(self.repo / "scripts/package-cpp-linux.sh"),
             "--build-dir", str(self.build), "--legal-root", str(self.legal),
             "--output", str(self.output), *extra],
            cwd=self.root, env=self.env, capture_output=True, text=True, timeout=30)
        if self.output.parent.exists():
            self.assertEqual(list(self.output.parent.glob(".melearner-package.*")), [])
        return result

    def assert_rejected(self, result: subprocess.CompletedProcess, text: str) -> None:
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn(text, result.stdout + result.stderr)
        self.assertNotIn("Created diagnostic", result.stdout)

    def test_real_static_cache_is_accepted(self) -> None:
        cache = (self.build / "CMakeCache.txt").read_text()
        self.assertIn("CMAKE_PROJECT_VERSION:STATIC=0.1.0", cache)
        listing = subprocess.run([REAL_CMAKE, "-LA", "-N", str(self.build)],
                                 check=True, capture_output=True, text=True, timeout=20)
        self.assertNotIn("CMAKE_PROJECT_VERSION:STATIC=", listing.stdout)
        result = self.run_package()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("Release-qualified: false", result.stdout)
        archive = subprocess.run([REAL_TAR, "--zstd", "-tf", str(self.output)],
                                 check=True, capture_output=True, text=True, timeout=20)
        self.assertIn("melearner-0.1.0/usr/bin/melearner\n", archive.stdout)

    def test_large_archive_does_not_fail_with_sigpipe(self) -> None:
        result = self.run_package(mode="large")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_deterministic_archive(self) -> None:
        first = self.run_package()
        self.assertEqual(first.returncode, 0, first.stderr)
        before = self.output.read_bytes()
        self.output.unlink()
        second = self.run_package()
        self.assertEqual(second.returncode, 0, second.stderr)
        self.assertEqual(self.output.read_bytes(), before)

    def test_invalid_versions(self) -> None:
        cache = self.build / "CMakeCache.txt"
        for entry in ("", "CMAKE_PROJECT_VERSION:STATIC=0.2.0\n",
                      "CMAKE_PROJECT_VERSION:STATIC=0.1.0\n" * 2):
            with self.subTest(entry=entry):
                cache.write_text(entry)
                self.assert_rejected(self.run_package(), "CMake build version must be 0.1.0")
                self.assertFalse((self.root / "stage.log").exists())

    def test_missing_build(self) -> None:
        self.build = self.root / "absent"
        self.assert_rejected(self.run_package(), "configured CMake build directory is missing")

    def test_missing_legal_inputs(self) -> None:
        self.legal = self.root / "absent"
        self.assert_rejected(self.run_package(), "legal input directory is missing")

    def test_invalid_arguments(self) -> None:
        for args in (("--unknown",), ("--output",), ("--build-dir", ""),
                     ("--legal-root", ""), ("--output", ""), ("--output", "--help")):
            with self.subTest(args=args):
                self.assert_rejected(self.run_package(*args), "usage:")
                self.assertFalse((self.root / "stage.log").exists())

    def test_destdir_is_rejected(self) -> None:
        self.env["DESTDIR"] = str(self.root / "external-stage")
        self.assert_rejected(self.run_package(), "DESTDIR must be empty")
        self.assertFalse((self.root / "external-stage").exists())

    def test_wrong_output_name(self) -> None:
        self.output = self.output.with_name("incorrect.tar.zst")
        self.assert_rejected(self.run_package(), "output filename must be")

    def test_existing_output_is_unchanged(self) -> None:
        self.output.parent.mkdir()
        self.output.write_bytes(b"existing archive")
        self.assert_rejected(self.run_package(), "refusing to overwrite output")
        self.assertEqual(self.output.read_bytes(), b"existing archive")

    def test_existing_dangling_symlink_is_unchanged(self) -> None:
        self.output.parent.mkdir()
        self.output.symlink_to("missing-target")
        self.assert_rejected(self.run_package(), "refusing to overwrite output")
        self.assertEqual(os.readlink(self.output), "missing-target")

    def test_concurrent_output_file_is_unchanged(self) -> None:
        self.assert_rejected(self.run_package(mode="racing-file"), "without overwriting output")
        self.assertEqual(self.output.read_bytes(), b"concurrent writer")

    def test_concurrent_directory_is_not_used_as_destination(self) -> None:
        self.assert_rejected(self.run_package(mode="racing-directory"), "without overwriting output")
        self.assertEqual(list(self.output.iterdir()), [])

    def test_concurrent_symlink_is_unchanged(self) -> None:
        self.assert_rejected(self.run_package(mode="racing-symlink"), "without overwriting output")
        self.assertEqual(os.readlink(self.output), "missing-target")

    def test_staging_failure_does_not_publish(self) -> None:
        self.assertNotEqual(self.run_package(mode="stage-failure").returncode, 0)
        self.assertFalse(self.output.exists())

    def test_missing_binary_does_not_publish(self) -> None:
        self.assert_rejected(self.run_package(mode="missing-binary"), "missing the melearner executable")
        self.assertFalse(self.output.exists())

    def test_dot_prefixed_filename_is_safe(self) -> None:
        result = self.run_package(mode="dot-prefix")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def tar_listing(self, listing: str, code: int = 0) -> None:
        self.tool("tar", f'''#!/usr/bin/env -S python3 -S
import os, subprocess, sys
if "--list" in sys.argv:
    sys.stdout.write({listing!r})
    sys.exit({code})
sys.exit(subprocess.call([os.environ["REAL_TAR"], *sys.argv[1:]]))
''')

    def test_listing_failure_does_not_publish(self) -> None:
        self.tar_listing("melearner-0.1.0/usr/bin/melearner\n", 2)
        self.assert_rejected(self.run_package(), "cannot read the complete archive listing")
        self.assertFalse(self.output.exists())

    def test_unsafe_archive_paths(self) -> None:
        for path in ("/absolute", "melearner-0.1.0/../escape", "../escape", "melearner-0.1.0/.."):
            with self.subTest(path=path):
                self.tar_listing("melearner-0.1.0/usr/bin/melearner\n" + path + "\n")
                self.assert_rejected(self.run_package(), "archive contains an unsafe path")
                self.assertFalse(self.output.exists())


if __name__ == "__main__":
    required = ("bash", "cmake", "tar", "zstd", "awk", "grep", "mktemp", "ln")
    missing = [tool for tool in required if not shutil.which(tool)]
    if os.uname().sysname != "Linux" or missing:
        raise SystemExit(f"Linux and these tools are required: {', '.join(required)}; missing: {missing}")
    unittest.main(verbosity=2)
