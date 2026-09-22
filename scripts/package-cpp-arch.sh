#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
version="0.1.9"
pkgrel="1"
build_dir="${repo_root}/build/cpp-release"
legal_root="${repo_root}/packaging"
stage_dir=""
output="${repo_root}/dist/melearner-bin-${version}-${pkgrel}-x86_64.pkg.tar.zst"

usage() {
  cat <<'EOF'
usage: scripts/package-cpp-arch.sh [options]

Create the C++ Linux Arch package from a validated staged usr tree. Without
--stage-dir, the configured release build is staged with stage-cpp-linux.cmake.

Options:
  --build-dir <path>  configured CMake release build (default: build/cpp-release)
  --legal-root <path> legal inputs (default: packaging)
  --stage-dir <path>  existing final C++ package stage (default: stage release build)
  --output <path>     output package (default: dist/melearner-bin-0.1.9-1-x86_64.pkg.tar.zst)
  -h, --help          show this help
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --build-dir|--legal-root|--stage-dir|--output)
      [[ $# -ge 2 && -n "$2" && "$2" != --* ]] || { usage >&2; exit 2; }
      case "$1" in
        --build-dir) build_dir="$2" ;;
        --legal-root) legal_root="$2" ;;
        --stage-dir) stage_dir="$2" ;;
        --output) output="$2" ;;
      esac
      shift 2
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      usage >&2
      exit 2
      ;;
  esac
done

if [[ "$(uname -s)" != Linux ]]; then
  echo "C++ Arch packaging is only supported on Linux" >&2
  exit 1
fi
if [[ -n "${DESTDIR:-}" ]]; then
  echo "DESTDIR must be empty; the packager manages its own stage" >&2
  exit 1
fi

for tool in makepkg python3 file readelf grep find mktemp ln cp install bsdtar; do
  if ! command -v "$tool" >/dev/null 2>&1; then
    echo "required Arch packaging tool is missing: $tool" >&2
    exit 1
  fi
done
if [[ -z "$stage_dir" ]]; then
  for tool in cmake awk; do
    if ! command -v "$tool" >/dev/null 2>&1; then
      echo "required C++ staging tool is missing: $tool" >&2
      exit 1
    fi
  done
fi

absolute_path() {
  if [[ "$1" == /* ]]; then
    printf '%s\n' "$1"
  else
    printf '%s/%s\n' "$repo_root" "$1"
  fi
}

build_dir="$(absolute_path "$build_dir")"
legal_root="$(absolute_path "$legal_root")"
if [[ -n "$stage_dir" ]]; then
  stage_dir="$(absolute_path "$stage_dir")"
fi
output="$(absolute_path "$output")"
expected_name="melearner-bin-${version}-${pkgrel}-x86_64.pkg.tar.zst"
if [[ "$(basename -- "$output")" != "$expected_name" ]]; then
  echo "output filename must be $expected_name" >&2
  exit 1
fi
if [[ -e "$output" || -L "$output" ]]; then
  echo "refusing to overwrite output: $output" >&2
  exit 1
fi

require_input_file() {
  local path="$1"
  local label="$2"
  if [[ ! -f "$path" || -L "$path" || ! -s "$path" ]]; then
    echo "required legal input is missing or empty: $label ($path)" >&2
    exit 1
  fi
}

generated_stage=false
if [[ -n "$stage_dir" ]]; then
  if [[ ! -d "$stage_dir/usr" || -L "$stage_dir/usr" ]]; then
    echo "C++ stage must contain a real usr directory: $stage_dir" >&2
    exit 1
  fi
else
  if [[ ! -d "$build_dir" || ! -f "$build_dir/CMakeCache.txt" ]]; then
    echo "configured CMake release build directory is missing: $build_dir" >&2
    exit 1
  fi
  project_version="$(awk -F= '$1 == "CMAKE_PROJECT_VERSION:STATIC" { print $2 }' "$build_dir/CMakeCache.txt")"
  if [[ "$project_version" != "$version" ]]; then
    echo "CMake build version must be $version, got ${project_version:-missing}; reconfigure the release build" >&2
    exit 1
  fi
  if [[ ! -d "$legal_root" ]]; then
    echo "legal input directory is missing: $legal_root" >&2
    exit 1
  fi
  require_input_file "$repo_root/LICENSE" "project license"
  require_input_file "$legal_root/THIRD_PARTY_NOTICES" "third-party notices"
  require_input_file "$legal_root/melearner.spdx.json" "SPDX manifest"
  require_input_file "$legal_root/runtime-lock.json" "runtime lock"
  require_input_file "$legal_root/reference-profiles-v1.json" "reference profiles"
  generated_stage=true
fi

output_dir="$(dirname -- "$output")"
mkdir -p -- "$output_dir"
output_dir="$(cd -- "$output_dir" && pwd -P)"
output="$output_dir/$expected_name"
work_dir="$(mktemp -d "$output_dir/.melearner-arch-package.XXXXXX")"
cleanup() {
  rm -rf -- "$work_dir"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
trap 'exit 129' HUP

if [[ "$generated_stage" == true ]]; then
  stage_dir="$work_dir/cpp-stage"
  cmake \
    -DMELEARNER_SOURCE_DIR="$repo_root" \
    -DMELEARNER_BUILD_DIR="$build_dir" \
    -DMELEARNER_STAGE_DIR="$stage_dir" \
    -DMELEARNER_LEGAL_ROOT="$legal_root" \
    -DMELEARNER_VERSION="$version" \
    -P "$repo_root/scripts/stage-cpp-linux.cmake"
fi

package_stage="$work_dir/package-stage"
mkdir -p -- "$package_stage"
cp -a "$stage_dir/." "$package_stage/"
if [[ "$generated_stage" == true ]]; then
  install -Dm644 "$repo_root/packaging/arch/io.github.whitehades.melearner.desktop" \
    "$package_stage/usr/share/applications/io.github.whitehades.melearner.desktop"
fi

binary="$package_stage/usr/bin/melearner"
desktop="$package_stage/usr/share/applications/io.github.whitehades.melearner.desktop"
if [[ ! -f "$binary" || -L "$binary" || ! -x "$binary" ]]; then
  echo "staged C++ executable is missing, not regular, or not executable: $binary" >&2
  exit 1
fi
binary_description="$(file -b "$binary")"
if [[ "$binary_description" != ELF\ 64-bit*x86-64* ]]; then
  echo "staged melearner must be an x86_64 ELF executable: $binary_description" >&2
  exit 1
fi
if [[ ! -f "$desktop" || -L "$desktop" ]]; then
  echo "staged Arch desktop launcher is missing: $desktop" >&2
  exit 1
fi

python3 - "$package_stage" <<'PY'
import json
import os
from pathlib import Path
import sys

stage = Path(sys.argv[1])
usr = stage / "usr"
expected_legal = [
    "LICENSE",
    "THIRD_PARTY_NOTICES",
    "melearner.spdx.json",
    "runtime-lock.json",
    "reference-profiles-v1.json",
]


def fail(message: str) -> None:
    print(message, file=sys.stderr)
    raise SystemExit(1)


def require_file(relative: str, label: str) -> Path:
    path = stage / relative
    if path.is_symlink() or not path.is_file() or path.stat().st_size < 1:
        fail(f"required staged file is missing or empty: {label} ({path})")
    return path


if not usr.is_dir() or usr.is_symlink():
    fail(f"C++ stage must contain a real usr directory: {usr}")

binary = require_file("usr/bin/melearner", "melearner executable")
if not os.access(binary, os.X_OK):
    fail(f"staged melearner is not executable: {binary}")
bin_dir = binary.parent
for candidate in bin_dir.iterdir():
    if candidate.name != "melearner" and candidate.is_file() and os.access(candidate, os.X_OK):
        fail(f"unexpected staged executable: {candidate}")

desktop = require_file(
    "usr/share/applications/io.github.whitehades.melearner.desktop",
    "Arch desktop launcher",
)
desktop_lines = desktop.read_text(encoding="utf-8").splitlines()
exec_lines = [line for line in desktop_lines if line.startswith("Exec=")]
if exec_lines != ["Exec=/usr/bin/melearner"]:
    fail("desktop launcher must contain exactly Exec=/usr/bin/melearner")
desktop_lower = desktop.read_text(encoding="utf-8").lower()
if any(token in desktop_lower for token in ("tauri", "native-app", "node", "zig", "rust", "webview", "webengine", "qml", "electron", "chromium")):
    fail("desktop launcher references an old or browser runtime")

metadata_path = require_file(
    "usr/share/doc/melearner/runtime-stage.json", "runtime stage metadata"
)
try:
    metadata = json.loads(metadata_path.read_text(encoding="utf-8"))
except (OSError, json.JSONDecodeError) as error:
    fail(f"runtime-stage.json is not valid JSON: {error}")
if not isinstance(metadata, dict):
    fail("runtime-stage.json must be a JSON object")
if metadata.get("schemaVersion") != 1:
    fail("runtime-stage.json schemaVersion must be 1")
if metadata.get("version") != "0.1.9":
    fail("runtime-stage.json version must be 0.1.9")
if metadata.get("architecture") != "x86_64":
    fail("runtime-stage.json architecture must be x86_64")
if metadata.get("releaseQualified") is not False:
    fail("runtime-stage.json must keep releaseQualified false")
if metadata.get("legalInputs") != expected_legal:
    fail("runtime-stage.json legalInputs do not match the required legal inputs")
for key in ("privateLibraries", "systemRuntimeBoundary", "qtPluginGroups"):
    if not isinstance(metadata.get(key), list):
        fail(f"runtime-stage.json {key} must be an array")

legal_paths = {
    "LICENSE": "usr/share/licenses/melearner/LICENSE",
    "THIRD_PARTY_NOTICES": "usr/share/doc/melearner/THIRD_PARTY_NOTICES",
    "melearner.spdx.json": "usr/share/doc/melearner/melearner.spdx.json",
    "runtime-lock.json": "usr/share/doc/melearner/runtime-lock.json",
    "reference-profiles-v1.json": "usr/share/doc/melearner/reference-profiles-v1.json",
}
for name, relative in legal_paths.items():
    path = require_file(relative, name)
    if name.endswith(".json"):
        try:
            parsed = json.loads(path.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError) as error:
            fail(f"staged legal JSON is invalid ({name}): {error}")
        if not isinstance(parsed, dict):
            fail(f"staged legal JSON must be an object: {name}")

usr_root = usr.resolve()
for path in usr.rglob("*"):
    if path.is_symlink():
        target = os.readlink(path)
        resolved = (path.parent / target).resolve(strict=False)
        if target.startswith("/"):
            fail(f"absolute staged symlink is not portable: {path}")
        try:
            resolved.relative_to(usr_root)
        except ValueError:
            fail(f"staged symlink escapes usr/: {path}")
    relative_parts = tuple(part.lower() for part in path.relative_to(usr).parts)
    forbidden_parts = {
        "native-app", "src-tauri", "node_modules", "qml", "webengine",
        "webview", "electron", "chromium", "zig", "rust",
    }
    if forbidden_parts.intersection(relative_parts):
        fail(f"old or browser runtime asset is staged: {path}")
    if path.is_file() and path.name.lower() in {
        "node", "nodejs", "chromium", "chrome", "electron", "mpv",
        "ffmpeg", "ffprobe",
    }:
        fail(f"old or browser runtime executable is staged: {path}")
    if (
        len(relative_parts) >= 3
        and relative_parts[:3] == ("share", "melearner", "resources")
        and path.is_file()
        and path.suffix.lower() in {".js", ".mjs", ".cjs", ".html", ".htm"}
    ):
        fail(f"browser application asset is staged: {path}")

private_dir = usr / "lib/melearner"
private_libraries = metadata["privateLibraries"]
if not private_dir.is_dir() or private_dir.is_symlink():
    fail(f"private runtime directory is missing: {private_dir}")
for name in private_libraries:
    if not isinstance(name, str) or "/" in name or not (private_dir / name).exists():
        fail(f"declared private runtime library is missing: {name}")
if not any(path.name.startswith("libmpv.so") for path in private_dir.iterdir()):
    fail("private runtime closure does not contain libmpv")
if not any(path.name.startswith("libQt6Pdf.so") for path in private_dir.iterdir()):
    fail("private runtime closure does not contain Qt6Pdf")
PY

while IFS= read -r -d '' candidate; do
  if [[ "$(file -b "$candidate")" == ELF\ * ]]; then
    if ! dynamic="$(readelf -dW "$candidate" 2>&1)"; then
      echo "readelf failed for staged ELF: $candidate" >&2
      exit 1
    fi
    if grep -Eiq '\((NEEDED|SONAME)\).*\[[^]]*(webkit|javascriptcore|webview2|cef|electron|tauri|qwebengine|qt6webengine|qt6qml|qt6quick)' <<<"$dynamic"; then
      echo "old or browser runtime import in ELF: $candidate" >&2
      exit 1
    fi
    if grep -Eq '(RPATH|RUNPATH).*(/home/|/opt/|/usr/local/|/nix/store/)' <<<"$dynamic"; then
      echo "host build path in staged ELF RPATH/RUNPATH: $candidate" >&2
      exit 1
    fi
  fi
done < <(find "$package_stage/usr" -type f -print0)

makepkg_dir="$work_dir/makepkg"
mkdir -p -- "$makepkg_dir"
cp -- "$repo_root/packaging/arch/PKGBUILD" "$makepkg_dir/PKGBUILD"
(
  cd -- "$makepkg_dir"
  PKGDEST="$makepkg_dir" MELEARNER_CPP_STAGE="$package_stage" \
    makepkg --nodeps --noconfirm --force
)

package_path="$makepkg_dir/$expected_name"
if [[ ! -s "$package_path" ]]; then
  echo "makepkg did not produce $expected_name" >&2
  exit 1
fi
package_listing="$work_dir/package.list"
if ! bsdtar --list --file "$package_path" >"$package_listing"; then
  echo "cannot read the complete Arch package listing" >&2
  exit 1
fi
if ! grep -Fxq "usr/bin/melearner" "$package_listing"; then
  echo "Arch package is missing usr/bin/melearner" >&2
  exit 1
fi
if ! grep -Fxq "usr/share/applications/io.github.whitehades.melearner.desktop" "$package_listing"; then
  echo "Arch package is missing the desktop launcher" >&2
  exit 1
fi
if grep -Eq '(^/|(^|/)\.\.(/|$))' "$package_listing"; then
  echo "Arch package contains an unsafe path" >&2
  exit 1
fi

if ! ln -T -- "$package_path" "$output"; then
  echo "could not publish Arch package without overwriting output: $output" >&2
  exit 1
fi

printf 'Created C++ Arch package: %s\n' "$output"
printf 'Release-qualified: false; legal approval, signing, and installed acceptance remain separate gates.\n'
