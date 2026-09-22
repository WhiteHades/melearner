#!/usr/bin/env python3
"""Test production staging RPATHs and the ELF audit using tiny compiled fixtures.

RPATH values come from stage-cpp-linux.cmake and are applied by the
version-checked patchelf tool. The audit function is evaluated verbatim in an isolated CMake script.
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

    def plugin_specs(self) -> list[tuple[str, str]]:
        match = re.search(r"(?ms)^set\(_required_qt_plugins\n(.*?)\)\n", self.stage)
        self.assertIsNotNone(match, "staging must declare an explicit Qt plugin manifest")
        return re.findall(r'"([^|]+)\|([^\"]+)"', match.group(1))

    def plugin_copy_helper(self) -> str:
        functions = re.findall(
            r"(?ms)^function\(_stage_qt_plugin .*?^endfunction\(\)\n", self.stage)
        self.assertEqual(len(functions), 1, "Qt plugin staging helper must exist exactly once")
        return functions[0]

    def run_plugin_copy(self, missing: tuple[str, str] | None = None) -> tuple[subprocess.CompletedProcess, list[str]]:
        suffix = "missing" if missing is not None else "complete"
        source = self.root / f"qt plugins {suffix}"
        destination = self.root / f"staged plugins {suffix}"
        specs = self.plugin_specs()
        for group, name in specs:
            path = source / group / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(b"fixture plugin")
        # These host plugins prove that staging does not discover unrelated
        # files merely because they happen to exist in the Qt plugin tree.
        for group, name in (("platforms", "libqminimal.so"),
                            ("platformthemes", "libqgtk3.so"),
                            ("multimedia", "libffmpegmediaplugin.so")):
            path = source / group / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(b"host plugin")
        if missing is not None:
            (source / missing[0] / missing[1]).unlink()

        calls = "\n".join(
            f'_stage_qt_plugin("${{SOURCE_ROOT}}" "${{DEST_ROOT}}" "{group}" "{name}" _sources _groups)'
            for group, name in specs
        )
        script = self.root / "plugin-stage.cmake"
        script.write_text(
            "cmake_minimum_required(VERSION 3.20)\n"
            + self.plugin_copy_helper()
            + "set(_sources)\nset(_groups)\n"
            + calls
            + "\nfile(GLOB_RECURSE _copied LIST_DIRECTORIES false RELATIVE \"${DEST_ROOT}\" \"${DEST_ROOT}/*\")\n"
              "list(SORT _copied)\nstring(REPLACE \";\" \"\\n\" _report \"${_copied}\")\n"
              "file(WRITE \"${REPORT}\" \"${_report}\\n\")\n",
            encoding="utf-8",
        )
        report = self.root / f"plugin-stage-{suffix}.list"
        result = subprocess.run(
            ["cmake", f"-DSOURCE_ROOT={source}", f"-DDEST_ROOT={destination}",
             f"-DREPORT={report}", "-P", str(script)],
            capture_output=True, text=True, timeout=20,
        )
        copied = report.read_text(encoding="utf-8").splitlines() if report.exists() else []
        return result, copied

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

    def test_hexadecimal_addresses_are_not_browser_imports(self) -> None:
        target = self.shared("libnative.so", "int native(void) { return 7; }",
                             self.root, "-Wl,--section-start,.init=0xceff0")
        dynamic = checked("readelf", "-dW", str(target)).stdout.lower()
        self.assertIn("cef", dynamic)
        result = self.audit(target)
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_readelf_failure_is_not_accepted(self) -> None:
        target = self.root / "not-elf"
        target.write_text("not an ELF file", encoding="utf-8")
        result = self.audit(target)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("readelf failed", result.stderr)

    def test_patchelf_gnu_hash_is_preserved(self) -> None:
        fixture = Path(os.environ.get("MELEARNER_GNU_HASH_FIXTURE", "/usr/lib/libleancrypto.so.1"))
        patchelf = None
        for directory in os.environ.get("PATH", "").split(os.pathsep):
            candidate = Path(directory) / "patchelf"
            if not candidate.is_file() or not os.access(candidate, os.X_OK):
                continue
            result = subprocess.run([str(candidate), "--version"], capture_output=True,
                                    text=True, timeout=20)
            version = re.search(r"patchelf\s+([0-9]+)\.([0-9]+)\.([0-9]+)", result.stdout)
            if result.returncode == 0 and version and \
                    tuple(int(part) for part in version.groups()) >= (0, 19, 1):
                patchelf = str(candidate)
                break
        if not patchelf or not fixture.is_file():
            self.skipTest("patchelf >= 0.19.1 and a GNU-hash ELF fixture are required")

        source = self.root / "gnu-hash source.so"
        target = self.root / "gnu-hash target.so"
        shutil.copy2(fixture, source)
        shutil.copy2(fixture, target)

        def section_bytes(path: Path) -> bytes:
            output = checked("readelf", "-x", ".gnu.hash", str(path)).stdout
            chunks: list[str] = []
            for line in output.splitlines():
                match = re.match(r"^\s*0x[0-9a-f]+\s+((?:[0-9a-f]{8}\s*)+)", line)
                if match:
                    chunks.extend(re.findall(r"[0-9a-f]{8}", match.group(1)))
            self.assertTrue(chunks, f"GNU hash section was not readable in {path}")
            return bytes.fromhex("".join(chunks))

        original_hash = section_bytes(source)
        checked(patchelf, "--set-rpath", "$ORIGIN", str(target))
        self.assertEqual(section_bytes(target), original_hash)

    def test_qt_plugin_manifest_is_explicit_and_missing_entries_fail(self) -> None:
        expected = [
            ("platforms", "libqxcb.so"),
            ("platforms", "libqwayland.so"),
            ("platforminputcontexts", "libcomposeplatforminputcontextplugin.so"),
            ("platforminputcontexts", "libibusplatforminputcontextplugin.so"),
            ("imageformats", "libqjpeg.so"),
            ("imageformats", "libqgif.so"),
            ("imageformats", "libqwebp.so"),
            ("xcbglintegrations", "libqxcb-egl-integration.so"),
            ("xcbglintegrations", "libqxcb-glx-integration.so"),
            ("wayland-shell-integration", "libxdg-shell.so"),
            ("wayland-graphics-integration-client", "libqt-plugin-wayland-egl.so"),
        ]
        self.assertEqual(self.plugin_specs(), expected)
        self.assertNotIn("file(GLOB _source_plugins", self.stage)
        self.assertNotIn("set(_plugin_groups", self.stage)

        result, copied = self.run_plugin_copy()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(copied, sorted(f"{group}/{name}" for group, name in expected))

        result, copied = self.run_plugin_copy(missing=expected[1])
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("required Qt plugin is missing", result.stderr)
        self.assertNotIn(f"{expected[1][0]}/{expected[1][1]}", copied)
        self.assertTrue(set(copied).issubset({f"{group}/{name}" for group, name in expected}))

    def test_target_qt_plugin_names_exist_when_local_qt_is_available(self) -> None:
        plugin_root = Path(os.environ.get("MELEARNER_QT_PLUGIN_DIR", "/usr/lib/qt6/plugins"))
        if not plugin_root.is_dir():
            self.skipTest(f"Qt plugin directory is not installed: {plugin_root}")
        for group, name in self.plugin_specs():
            with self.subTest(group=group, name=name):
                self.assertTrue((plugin_root / group / name).is_file())

    def test_gpu_runtime_is_a_system_boundary_and_unknown_vendor_lib_fails(self) -> None:
        functions = re.findall(
            r"(?ms)^function\(_runtime_dependency_is_system_boundary .*?^endfunction\(\)\n",
            self.stage,
        )
        self.assertEqual(len(functions), 1, "runtime boundary classifier must exist exactly once")
        script = self.root / "boundary.cmake"
        script.write_text(
            "cmake_minimum_required(VERSION 3.20)\n"
            + functions[0]
            + "_runtime_dependency_is_system_boundary(\"/opt/cuda/lib64/libOpenCL.so.1\" \"libOpenCL.so.1\" result)\n"
              "if(NOT result)\nmessage(FATAL_ERROR \"OpenCL must be system-bound\")\nendif()\n"
              "_runtime_dependency_is_system_boundary(\"/usr/lib/libvulkan.so.1\" \"libvulkan.so.1\" result)\n"
              "if(NOT result)\nmessage(FATAL_ERROR \"Vulkan must be system-bound\")\nendif()\n"
              "_runtime_dependency_is_system_boundary(\"/usr/lib/libQt6Core.so.6\" \"libQt6Core.so.6\" result)\n"
              "if(result)\nmessage(FATAL_ERROR \"Qt must remain private\")\nendif()\n",
            encoding="utf-8",
        )
        result = subprocess.run(["cmake", "-P", str(script)], capture_output=True, text=True, timeout=20)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

        script.write_text(
            "cmake_minimum_required(VERSION 3.20)\n"
            + functions[0]
            + "_runtime_dependency_is_system_boundary(\"/opt/cuda/lib64/libvendorcodec.so.1\" \"libvendorcodec.so.1\" result)\n",
            encoding="utf-8",
        )
        result = subprocess.run(["cmake", "-P", str(script)], capture_output=True, text=True, timeout=20)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("unlocked vendor GPU dependency", result.stderr)


if __name__ == "__main__":
    required = ("cc", "cmake", "readelf")
    missing = [tool for tool in required if not shutil.which(tool)]
    if os.uname().sysname != "Linux" or missing:
        raise SystemExit(f"Linux and these tools are required: {', '.join(required)}; missing: {missing}")
    unittest.main(verbosity=2)
