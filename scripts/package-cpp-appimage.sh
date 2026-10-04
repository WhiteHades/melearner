#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
version="0.1.0"
build_dir="${repo_root}/build/cpp-release"
legal_root="${repo_root}/packaging"
output="${repo_root}/dist/melearner_${version}_amd64.AppImage"
max_glibc_version="2.39"

usage() {
  cat <<'EOF'
usage: scripts/package-cpp-appimage.sh [options]

Create the diagnostic C++ Linux AppImage from the configured release build.
The existing CMake runtime stager owns dependency closure and RPATH auditing;
the installed linuxdeploy AppImage output plugin only turns the staged AppDir
into an AppImage. The Qt WebEngine document viewer is bundled. No updater is bundled.

Options:
  --build-dir <path>  configured CMake release build (default: build/cpp-release)
  --legal-root <path> canonical legal inputs (default: packaging)
  --output <path>     output AppImage (default: dist/melearner_0.1.0_amd64.AppImage)
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

glibc_version_at_most() {
  local required="$1"
  local limit="$max_glibc_version"
  local -a required_parts limit_parts
  local index part_count required_part limit_part
  local LC_ALL=C

  IFS=. read -r -a required_parts <<<"$required"
  IFS=. read -r -a limit_parts <<<"$limit"
  part_count="${#required_parts[@]}"
  if ((${#limit_parts[@]} > part_count)); then
    part_count="${#limit_parts[@]}"
  fi
  for ((index = 0; index < part_count; index++)); do
    required_part="${required_parts[index]:-0}"
    limit_part="${limit_parts[index]:-0}"
    while [[ ${#required_part} -gt 1 && "$required_part" == 0* ]]; do
      required_part="${required_part#0}"
    done
    while [[ ${#limit_part} -gt 1 && "$limit_part" == 0* ]]; do
      limit_part="${limit_part#0}"
    done
    if ((${#required_part} > ${#limit_part})) || \
       { ((${#required_part} == ${#limit_part})) && [[ "$required_part" > "$limit_part" ]]; }; then
      return 1
    fi
    if ((${#required_part} < ${#limit_part})) || \
       { ((${#required_part} == ${#limit_part})) && [[ "$required_part" < "$limit_part" ]]; }; then
      return 0
    fi
  done
  return 0
}

check_glibc_requirements() {
  local path="$1"
  local label="$2"
  local version_info requirements token version
  local LC_ALL=C

  if ! version_info="$(readelf --version-info -W "$path" 2>&1)"; then
    echo "could not inspect GLIBC requirements for $label ($path): $version_info" >&2
    exit 1
  fi
  requirements="$(awk '
    /^Version needs section/ { in_needs = 1; next }
    /^Version / { in_needs = 0 }
    in_needs {
      for (i = 1; i < NF; i++) {
        if ($i == "Name:" && $(i + 1) ~ /^GLIBC_/) print $(i + 1)
      }
    }
  ' <<<"$version_info")"

  while IFS= read -r token; do
    [[ -n "$token" ]] || continue
    if [[ "$token" == GLIBC_PRIVATE ]]; then
      echo "unsupported GLIBC_PRIVATE requirement in $label ($path)" >&2
      exit 1
    fi
    if [[ "$token" == GLIBC_ABI_DT_RELR ]]; then
      version="2.36"
    elif [[ "$token" =~ ^GLIBC_([0-9]+(\.[0-9]+)+)$ ]]; then
      version="${BASH_REMATCH[1]}"
    else
      echo "unknown GLIBC requirement token $token in $label ($path)" >&2
      exit 1
    fi
    if ! glibc_version_at_most "$version"; then
      echo "GLIBC $version requirement in $label ($path) exceeds the AppImage maximum of $max_glibc_version" >&2
      exit 1
    fi
  done <<<"$requirements"
}

build_dir="$(absolute_path "$build_dir")"
legal_root="$(absolute_path "$legal_root")"
output="$(absolute_path "$output")"

if [[ ! -d "$build_dir" || ! -f "$build_dir/CMakeCache.txt" ]]; then
  echo "configured CMake build directory is missing: $build_dir" >&2
  exit 1
fi
if [[ ! -f "$build_dir/melearner" ]]; then
  echo "built application executable is missing: $build_dir/melearner" >&2
  exit 1
fi
check_glibc_requirements "$build_dir/melearner" "application executable"
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

cmake --build "$build_dir" --target melearner_stage_validator
validator="$build_dir/melearner_stage_validator"
if [[ ! -x "$validator" ]]; then
  echo "CMake did not produce the native stage validator: $validator" >&2
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

while IFS= read -r -d '' staged_path; do
  if ! staged_description="$(file -b -- "$staged_path")"; then
    echo "could not inspect staged file: $staged_path" >&2
    exit 1
  fi
  if [[ "$staged_description" == ELF\ * ]]; then
    check_glibc_requirements "$staged_path" "staged ELF ${staged_path#"$appdir"/}"
  fi
done < <(find "$appdir" -type f -print0)

appimage_desktop_name="io.github.whitehades.melearner.appimage.desktop"
appimage_desktop_target="usr/share/applications/${appimage_desktop_name}"
appimage_icon_target="usr/share/icons/hicolor/512x512/apps/io.github.whitehades.melearner.png"
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
if [[ "$desktop_lower" =~ (tauri|native-app|node|zig|rust|webview|qml|electron|chromium) ]]; then
  echo "AppImage desktop launcher references an old or browser runtime" >&2
  exit 1
fi

"$validator" "$appdir" --appimage

while IFS= read -r -d '' staged_path; do
  relative_path="${staged_path#"$appdir"/}"
  relative_lower="${relative_path,,}"
  if [[ "$relative_lower" =~ (^|/)(native-app|src-tauri|node_modules|qml|webview|electron|chromium|zig|rust)(/|$) ]]; then
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
