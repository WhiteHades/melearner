#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)"
build_dir="$repo_root/build/cpp-windows"
jobs="${MELEARNER_BUILD_JOBS:-}"
run_playback=false

usage() {
  cat <<'EOF'
usage: bash scripts/build-cpp-windows.sh [--build-dir <path>] [--jobs <count>] [--run-playback]

Configure and build melearner in the MSYS2 UCRT64 shell, then run every test
registered by CMake. The OpenGL playback binaries are build targets only and
are launched separately with --run-playback.
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --build-dir)
      [[ $# -ge 2 && -n "$2" && "$2" != --* ]] || { usage >&2; exit 2; }
      build_dir="$2"
      shift 2
      ;;
    --jobs)
      [[ $# -ge 2 && "$2" =~ ^[1-9][0-9]*$ ]] || { usage >&2; exit 2; }
      jobs="$2"
      shift 2
      ;;
    --run-playback)
      run_playback=true
      shift
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

fail() {
  printf '%s\n' "$*" >&2
  exit 1
}

if [[ -n "$jobs" && ! "$jobs" =~ ^[1-9][0-9]*$ ]]; then
  fail "MELEARNER_BUILD_JOBS/--jobs must be a positive integer"
fi

[[ "${MSYSTEM:-}" == UCRT64 ]] ||
  fail "Run this entrypoint from an MSYS2 UCRT64 shell (MSYSTEM=UCRT64)."

for tool in cmake ctest ninja gcc g++ pkg-config; do
  command -v "$tool" >/dev/null 2>&1 ||
    fail "Missing UCRT64 build tool: $tool"
done

pkg-config --print-errors --exists mpv libzip md4c ||
  fail "Missing UCRT64 pkg-config dependencies: mpv, libzip, or md4c"

if [[ "$build_dir" != /* && "$build_dir" != [A-Za-z]:/* ]]; then
  build_dir="$repo_root/$build_dir"
fi

cmake -S "$repo_root" -B "$build_dir" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=ON

build_args=(--build "$build_dir" --parallel)
if [[ -n "$jobs" ]]; then
  build_args+=("$jobs")
fi
cmake "${build_args[@]}"

ctest --test-dir "$build_dir" --output-on-failure --no-tests=error

if [[ "$run_playback" == true ]]; then
  (
    cd "$build_dir"
    for test in playback_render_test main_playback_test; do
      executable="./${test}.exe"
      [[ -x "$executable" ]] ||
        fail "Missing playback test executable: $build_dir/${test}.exe"
      "$executable"
    done
  )
fi

if [[ "$run_playback" == true ]]; then
  printf 'Windows build, registered tests, and playback targets passed: %s\n' \
    "$build_dir/melearner.exe"
else
  printf 'Windows build and registered tests passed: %s\n' "$build_dir/melearner.exe"
fi
