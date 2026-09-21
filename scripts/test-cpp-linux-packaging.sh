#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
package_script="$repo_root/scripts/package-cpp-linux.sh"
stage_script="$repo_root/scripts/stage-cpp-linux.cmake"

bash -n "$package_script"
"$package_script" --help >/dev/null

test_root="$repo_root/.tmp/cpp-linux-packaging-test"
mkdir -p "$test_root"
work_dir="$(mktemp -d "$test_root/work.XXXXXX")"
cleanup() {
  rm -rf -- "$work_dir"
  rmdir -- "$test_root" 2>/dev/null || true
}
trap cleanup EXIT

if "$package_script" \
    --build-dir "$work_dir/missing-build" \
    --output "$work_dir/melearner-0.1.0-linux-x86_64.tar.zst" \
    >"$work_dir/package.log" 2>&1; then
  echo "packager accepted a missing CMake build" >&2
  exit 1
fi
grep -Fq "configured CMake build directory is missing" "$work_dir/package.log"

if cmake \
    -DMELEARNER_SOURCE_DIR="$repo_root" \
    -DMELEARNER_BUILD_DIR="$work_dir/missing-build" \
    -DMELEARNER_STAGE_DIR="$work_dir/stage" \
    -DMELEARNER_LEGAL_ROOT="$repo_root/packaging" \
    -P "$stage_script" \
    >"$work_dir/stage.log" 2>&1; then
  echo "stager accepted a missing CMake build" >&2
  exit 1
fi
grep -Fq "build directory is missing" "$work_dir/stage.log"

valid_source="$work_dir/valid-source"
valid_build="$work_dir/valid-build"
valid_legal="$work_dir/valid-legal"
mkdir -p "$valid_source" "$valid_build" "$valid_legal"
printf '%s\n' \
  'cmake_minimum_required(VERSION 4.4)' \
  'project(melearner VERSION 0.1.0 LANGUAGES CXX)' \
  >"$valid_source/CMakeLists.txt"
printf '%s\n' 'CMAKE_PROJECT_VERSION:STATIC=0.1.0' >"$valid_build/CMakeCache.txt"
printf '%s\n' '# valid enough for the stager to reach its legal-input checks' \
  >"$valid_build/cmake_install.cmake"

if cmake \
    -DMELEARNER_SOURCE_DIR="$valid_source" \
    -DMELEARNER_BUILD_DIR="$valid_build" \
    -DMELEARNER_STAGE_DIR="$work_dir/valid-stage" \
    -DMELEARNER_LEGAL_ROOT="$valid_legal" \
    -P "$stage_script" \
    >"$work_dir/valid-stage.log" 2>&1; then
  echo "stager unexpectedly completed an incomplete fixture" >&2
  exit 1
fi
if grep -Fq "configured CMake build version must be 0.1.0" "$work_dir/valid-stage.log"; then
  echo "stager rejected a valid 0.1.0 CMake cache" >&2
  exit 1
fi
grep -Fq "missing project license" "$work_dir/valid-stage.log"

echo "C++ Linux packaging script checks passed"
