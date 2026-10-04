#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
version="0.1.2"
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
All paths require a configured CMake release build containing the native stage
validator target; that target is built before package work begins.

Options:
  --build-dir <path>  configured CMake release build (default: build/cpp-release)
  --legal-root <path> legal inputs (default: packaging)
  --stage-dir <path>  existing final C++ package stage (default: stage release build)
  --output <path>     output package (default: dist/melearner-bin-0.1.2-1-x86_64.pkg.tar.zst)
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

for tool in makepkg file readelf grep find mktemp ln cp install bsdtar cmake awk; do
  if ! command -v "$tool" >/dev/null 2>&1; then
    echo "required Arch packaging tool is missing: $tool" >&2
    exit 1
  fi
done
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
if [[ ! -d "$build_dir" || ! -f "$build_dir/CMakeCache.txt" ]]; then
  echo "configured CMake release build directory is missing: $build_dir" >&2
  exit 1
fi
project_version="$(awk -F= '$1 == "CMAKE_PROJECT_VERSION:STATIC" { print $2 }' "$build_dir/CMakeCache.txt")"
if [[ "$project_version" != "$version" ]]; then
  echo "CMake build version must be $version, got ${project_version:-missing}; reconfigure the release build" >&2
  exit 1
fi
if [[ -n "$stage_dir" ]]; then
  if [[ ! -d "$stage_dir/usr" || -L "$stage_dir/usr" ]]; then
    echo "C++ stage must contain a real usr directory: $stage_dir" >&2
    exit 1
  fi
else
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

cmake --build "$build_dir" --target melearner_stage_validator
validator="$build_dir/melearner_stage_validator"
if [[ ! -x "$validator" ]]; then
  echo "CMake did not produce the native stage validator: $validator" >&2
  exit 1
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

"$validator" "$package_stage"

while IFS= read -r -d '' candidate; do
  if [[ "$(file -b "$candidate")" == ELF\ * ]]; then
    if ! dynamic="$(readelf -dW "$candidate" 2>&1)"; then
      echo "readelf failed for staged ELF: $candidate" >&2
      exit 1
    fi
    if grep -Eiq '\((NEEDED|SONAME)\).*\[[^]]*(webkit|javascriptcore|webview2|cef|electron|tauri)' <<<"$dynamic"; then
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
