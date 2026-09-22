#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
version="0.1.9"
build_dir="${repo_root}/build/cpp-release"
legal_root="${repo_root}/packaging"
output="${repo_root}/dist/melearner_${version}_amd64.AppImage"

usage() {
  cat <<'EOF'
usage: scripts/package-cpp-appimage.sh [options]

Create the diagnostic C++ Linux AppImage from the configured release build.
The existing CMake runtime stager owns dependency closure and RPATH auditing;
the installed linuxdeploy AppImage output plugin only turns the staged AppDir
into an AppImage. No browser runtime or updater is bundled.

Options:
  --build-dir <path>  configured CMake release build (default: build/cpp-release)
  --legal-root <path> canonical legal inputs (default: packaging)
  --output <path>     output AppImage (default: dist/melearner_0.1.9_amd64.AppImage)
  -h, --help          show this help
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --build-dir|--legal-root|--output)
      [[ $# -ge 2 && -n "$2" && "$2" != --* ]] || { usage >&2; exit 2; }
      case "$1" in
        --build-dir) build_dir="$2" ;;
        --legal-root) legal_root="$2" ;;
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
  echo "C++ Linux AppImage packaging is only supported on Linux" >&2
  exit 1
fi
if [[ -n "${DESTDIR:-}" ]]; then
  echo "DESTDIR must be empty; the packager manages its own AppDir" >&2
  exit 1
fi

for tool in awk cmake env file find grep install ln mktemp patchelf readelf readlink; do
  if ! command -v "$tool" >/dev/null 2>&1; then
    echo "required AppImage packaging tool is missing: $tool" >&2
    exit 1
  fi
done

plugin="${LINUXDEPLOY_PLUGIN_APPIMAGE:-}"
if [[ -z "$plugin" ]]; then
  plugin="$(command -v linuxdeploy-plugin-appimage || true)"
fi
if [[ -z "$plugin" || ! -x "$plugin" ]]; then
  echo "installed linuxdeploy-plugin-appimage is missing or not executable" >&2
  exit 1
fi
if [[ "$plugin" != /* ]]; then
  plugin="$(command -v "$plugin" || true)"
fi
if [[ -z "$plugin" ]]; then
  echo "could not resolve linuxdeploy-plugin-appimage to an absolute path" >&2
  exit 1
fi

plugin_type="$("$plugin" --plugin-type 2>/dev/null)" || {
  echo "linuxdeploy-plugin-appimage does not support --plugin-type: $plugin" >&2
  exit 1
}
if [[ "$plugin_type" != output ]]; then
  echo "unexpected linuxdeploy-plugin-appimage type: ${plugin_type:-missing}" >&2
  exit 1
fi
plugin_api_version="$("$plugin" --plugin-api-version 2>/dev/null)" || {
  echo "linuxdeploy-plugin-appimage does not support --plugin-api-version: $plugin" >&2
  exit 1
}
if [[ "$plugin_api_version" != 0 ]]; then
  echo "unsupported linuxdeploy-plugin-appimage API version: $plugin_api_version" >&2
  exit 1
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
output="$(absolute_path "$output")"

if [[ ! -d "$build_dir" || ! -f "$build_dir/CMakeCache.txt" ]]; then
  echo "configured CMake build directory is missing: $build_dir" >&2
  exit 1
fi
if [[ ! -d "$legal_root" ]]; then
  echo "legal input directory is missing: $legal_root" >&2
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

require_input_file "$repo_root/LICENSE" "project license"
require_input_file "$legal_root/THIRD_PARTY_NOTICES" "third-party notices"
require_input_file "$legal_root/melearner.spdx.json" "SPDX manifest"
require_input_file "$legal_root/runtime-lock.json" "runtime lock"
require_input_file "$legal_root/reference-profiles-v1.json" "reference profiles"

project_version="$(awk -F= '$1 == "CMAKE_PROJECT_VERSION:STATIC" { print $2 }' "$build_dir/CMakeCache.txt")"
if [[ "$project_version" != "$version" ]]; then
  echo "CMake build version must be $version, got ${project_version:-missing}; reconfigure the release build" >&2
  exit 1
fi

expected_name="melearner_${version}_amd64.AppImage"
if [[ "$(basename -- "$output")" != "$expected_name" ]]; then
  echo "output filename must be $expected_name" >&2
  exit 1
fi
if [[ -e "$output" || -L "$output" ]]; then
  echo "refusing to overwrite output: $output" >&2
  exit 1
fi
output_dir="$(dirname -- "$output")"
mkdir -p -- "$output_dir"

# Keep staging and the plugin output on the destination filesystem. The final
# hard link is atomic and cannot replace a file created by a concurrent caller.
work_dir="$(mktemp -d "$output_dir/.melearner-appimage.XXXXXX")"
cleanup() {
  rm -rf -- "$work_dir"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
trap 'exit 129' HUP

appdir="$work_dir/melearner-${version}.AppDir"
cmake \
  -DMELEARNER_SOURCE_DIR="$repo_root" \
  -DMELEARNER_BUILD_DIR="$build_dir" \
  -DMELEARNER_STAGE_DIR="$appdir" \
  -DMELEARNER_LEGAL_ROOT="$legal_root" \
  -DMELEARNER_VERSION="$version" \
  -P "$repo_root/scripts/stage-cpp-linux.cmake"

appimage_desktop_name="io.github.whitehades.melearner.appimage.desktop"
appimage_desktop_target="usr/share/applications/${appimage_desktop_name}"
appimage_icon_target="usr/share/pixmaps/io.github.whitehades.melearner.png"
install -Dm644 \
  "$repo_root/packaging/linux/${appimage_desktop_name}" \
  "$appdir/$appimage_desktop_target"
ln -s -- "usr/bin/melearner" "$appdir/AppRun"
ln -s -- "$appimage_desktop_target" "$appdir/$appimage_desktop_name"
ln -s -- "$appimage_icon_target" "$appdir/io.github.whitehades.melearner.png"

if [[ ! -L "$appdir/AppRun" || "$(readlink -- "$appdir/AppRun")" != usr/bin/melearner ]]; then
  echo "AppDir AppRun must link to usr/bin/melearner" >&2
  exit 1
fi
if [[ ! -L "$appdir/$appimage_desktop_name" || \
      "$(readlink -- "$appdir/$appimage_desktop_name")" != "$appimage_desktop_target" ]]; then
  echo "AppDir desktop launcher link is invalid" >&2
  exit 1
fi
if [[ ! -L "$appdir/io.github.whitehades.melearner.png" || \
      "$(readlink -- "$appdir/io.github.whitehades.melearner.png")" != "$appimage_icon_target" ]]; then
  echo "AppDir icon link is invalid" >&2
  exit 1
fi

desktop="$appdir/$appimage_desktop_target"
if ! grep -Fxq 'Exec=melearner' "$desktop" || \
   ! grep -Fxq 'Icon=io.github.whitehades.melearner' "$desktop"; then
  echo "AppImage desktop launcher has unexpected Exec or Icon" >&2
  exit 1
fi
desktop_lower="$(<"$desktop")"
desktop_lower="${desktop_lower,,}"
if [[ "$desktop_lower" =~ (tauri|native-app|node|zig|rust|webview|webengine|qml|electron|chromium) ]]; then
  echo "AppImage desktop launcher references an old or browser runtime" >&2
  exit 1
fi

while IFS= read -r -d '' staged_path; do
  relative_path="${staged_path#"$appdir"/}"
  relative_lower="${relative_path,,}"
  if [[ "$relative_lower" =~ (^|/)(native-app|src-tauri|node_modules|qml|webengine|webview|electron|chromium|zig|rust)(/|$) ]]; then
    echo "superseded runtime asset staged: $relative_path" >&2
    exit 1
  fi
  case "${relative_lower##*/}" in
    *.zsync|appimageupdate*|updater*|update*)
      echo "auto-updater artifact staged: $relative_path" >&2
      exit 1
      ;;
  esac
done < <(find "$appdir" -mindepth 1 -print0)

plugin_output="$work_dir/$expected_name"
env \
  -u LDAI_UPDATE_INFORMATION \
  -u LDAI_GUESS_UPDATE_INFORMATION \
  -u LDAI_SIGN \
  -u LDAI_SIGN_KEY \
  -u LDAI_RUNTIME_FILE \
  -u LINUXDEPLOY_OUTPUT_APP_NAME \
  LDAI_OUTPUT="$plugin_output" \
  LDAI_VERSION="$version" \
  LDAI_NO_APPSTREAM=1 \
  "$plugin" --appdir="$appdir"

if [[ ! -s "$plugin_output" || ! -x "$plugin_output" ]]; then
  echo "linuxdeploy-plugin-appimage did not produce an executable AppImage" >&2
  exit 1
fi
plugin_description="$(file -b "$plugin_output")"
if [[ "$plugin_description" != ELF\ 64-bit*x86-64* ]]; then
  echo "linuxdeploy-plugin-appimage output is not an x86_64 ELF AppImage: $plugin_description" >&2
  exit 1
fi
if find "$work_dir" -type f -name '*.zsync' -print -quit | grep -q .; then
  echo "auto-updater zsync output was produced unexpectedly" >&2
  exit 1
fi

if ! ln -T -- "$plugin_output" "$output"; then
  echo "could not publish AppImage without overwriting output: $output" >&2
  exit 1
fi

printf 'Created diagnostic C++ Linux AppImage: %s\n' "$output"
printf 'Release-qualified: false; signing, legal, and installed acceptance remain separate gates.\n'
