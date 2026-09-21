#!/usr/bin/env python3
"""Test production staging RPATHs and the ELF audit using tiny compiled fixtures.

RPATH values come from stage-cpp-linux.cmake and are applied by the linker, not
patchelf. The audit function is evaluated verbatim in an isolated CMake script.
This does not run the complete stager, Qt, libmpv, or an installed melearner app.
"""
from __future__ import annotations

import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


def checked(*args: str, **kwargs) -> subprocess.CompletedProcess:
    result = subprocess.run(args, capture_output=True, text=True, timeout=20, **kwargs)
    if result.returncode:
        raise AssertionError(f"Command failed: {args!r}\n{result.stdout}{result.stderr}")
    return result


class RuntimeTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory(prefix="melearner runtime ")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.stage = (ROOT / "scripts/stage-cpp-linux.cmake").read_text(encoding="utf-8")
        self.env = os.environ.copy()
        for name in ("LD_LIBRARY_PATH", "LD_PRELOAD", "LD_AUDIT"):
            self.env.pop(name, None)
        self.cc = shutil.which("cc")

    def rpath(self, variable: str) -> str:
        pattern = r'_run_patchelf\("\$\{' + re.escape(variable) + r'\}" "([^"\n]+)"\)'
        values = re.findall(pattern, self.stage)
        self.assertEqual(len(values), 1, f"Expected one production RPATH for {variable}")
        return values[0]

    def shared(self, name: str, source: str, directory: Path, *flags: str) -> Path:
        directory.mkdir(parents=True, exist_ok=True)
        src = self.root / (name + ".c")
        src.write_text(source, encoding="utf-8")
        library = directory / name
        checked(self.cc, "-shared", "-fPIC", "-Wall", "-Wextra", "-Werror",
                str(src), f"-Wl,-soname,{name}", *flags, "-o", str(library))
        return library

    def audit(self, elf: Path, readelf: str | None = None) -> subprocess.CompletedProcess:
        functions = re.findall(r"(?ms)^function\(_audit_elf .*?^endfunction\(\)\n", self.stage)
        self.assertEqual(len(functions), 1, "Production ELF audit must exist exactly once")
        script = self.root / "audit.cmake"
        script.write_text("cmake_minimum_required(VERSION 3.20)\n" + functions[0]
                          + '\n_audit_elf("${TEST_ELF}" "fixture")\n', encoding="utf-8")
        return subprocess.run(["cmake", f"-D_readelf_tool={readelf or shutil.which('readelf')}",
                               f"-DTEST_ELF={elf}", "-P", str(script)],
                              capture_output=True, text=True, timeout=20)

    def test_relocated_executable_plugin_and_transitive_library(self) -> None:
        original = self.root / "original package"
        runtime = original / "usr/lib/melearner"
        plugins = original / "usr/lib/qt6/plugins/platforms"
        bin_dir = original / "usr/bin"
        bin_dir.mkdir(parents=True)
        # The executable does not load the plugin's dependencies. Otherwise a
        # wrong plugin RUNPATH could be hidden by an already-loaded library.
        self.shared("libmelearner_fixture_main.so", "int main_value(void) { return 40; }", runtime)
        self.shared("libmelearner_fixture_leaf.so", "int leaf(void) { return 2; }", runtime)
        self.shared("libmelearner_fixture_chain.so",
                    "int leaf(void); int chain(void) { return leaf(); }", runtime,
                    f"-L{runtime}", "-lmelearner_fixture_leaf",
                    f"-Wl,-rpath,{self.rpath('_runtime_file')}", "-Wl,--enable-new-dtags")
        self.shared("libmelearner_fixture_plugin.so",
                    "int chain(void); int plugin_value(void) { return chain(); }", plugins,
                    f"-L{runtime}", "-lmelearner_fixture_chain",
                    f"-Wl,-rpath,{self.rpath('_plugin')}", "-Wl,--enable-new-dtags")
        source = self.root / "loader.c"
        source.write_text('''#include <dlfcn.h>
#include <stdio.h>
int main_value(void);
int main(int argc, char **argv) {
  if (argc != 2) return 2;
  void *plugin = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
  if (!plugin) { fprintf(stderr, "%s\\n", dlerror()); return 3; }
  int (*value)(void) = (int (*)(void))dlsym(plugin, "plugin_value");
  if (!value) { dlclose(plugin); return 4; }
  int result = main_value() + value();
  dlclose(plugin);
  printf("%d\\n", result);
  return result == 42 ? 0 : 5;
}
''', encoding="utf-8")
        checked(self.cc, "-Wall", "-Wextra", "-Werror", str(source), f"-L{runtime}",
                "-lmelearner_fixture_main", "-ldl", f"-Wl,-rpath,{self.rpath('_binary')}",
                "-Wl,--enable-new-dtags", "-o", str(bin_dir / "loader"))
        moved = self.root / "relocated package"
        original.rename(moved)
        self.assertFalse(original.exists())
        result = checked(str(moved / "usr/bin/loader"),
                         str(moved / "usr/lib/qt6/plugins/platforms/libmelearner_fixture_plugin.so"),
                         cwd="/", env=self.env)
        self.assertEqual(result.stdout.strip(), "42")

    def test_browser_dependencies_are_rejected(self) -> None:
        for name in ("libQt6WebEngineCore.so.6", "libQt6WebEngineWidgets.so.6",
                     "libQt6WebEngineQuick.so.6", "libQt6Qml.so.6", "libQt6Quick.so.6",
                     "libwebkit2gtk-4.1.so.0", "libjavascriptcoregtk-4.1.so.0", "libcef.so"):
            with self.subTest(library=name):
                self.shared(name, "int prohibited(void) { return 7; }", self.root)
                target = self.shared("libaudit_fixture.so",
                                     "int prohibited(void); int call(void) { return prohibited(); }",
                                     self.root, f"-L{self.root}", f"-l:{name}")
                result = self.audit(target)
                self.assertNotEqual(result.returncode, 0, name)
                self.assertIn("superseded browser/runtime import", result.stderr)

    def test_native_pdf_dependency_is_allowed(self) -> None:
        self.shared("libQt6Pdf.so.6", "int native_pdf(void) { return 7; }", self.root)
        target = self.shared("libaudit_fixture.so",
                             "int native_pdf(void); int call(void) { return native_pdf(); }",
                             self.root, f"-L{self.root}", "-l:libQt6Pdf.so.6", "-Wl,-rpath,$ORIGIN")
        result = self.audit(target)
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_host_build_rpaths_are_rejected(self) -> None:
        for path in ("/home/builder/lib", "/opt/qt/lib", "/usr/local/lib", "/nix/store/fixture/lib"):
            with self.subTest(path=path):
                target = self.shared("libaudit_fixture.so", "int fixture(void) { return 7; }",
                                     self.root, f"-Wl,-rpath,{path}")
                result = self.audit(target)
                self.assertNotEqual(result.returncode, 0, path)
                self.assertIn("host build path", result.stderr)

    def test_readelf_failure_is_not_accepted(self) -> None:
        target = self.root / "not-elf"
        target.write_text("not an ELF file", encoding="utf-8")
        result = self.audit(target)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("readelf failed", result.stderr)


if __name__ == "__main__":
    required = ("cc", "cmake", "readelf")
    missing = [tool for tool in required if not shutil.which(tool)]
    if os.uname().sysname != "Linux" or missing:
        raise SystemExit(f"Linux and these tools are required: {', '.join(required)}; missing: {missing}")
    unittest.main(verbosity=2)
