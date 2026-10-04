#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd -- "$script_dir/.." && pwd)"
build_dir=${MELEARNER_BUILD_DIR:-"$repo_root/build/cpp-dev"}
sizes=${MELEARNER_PERF_SIZES:-"10000 30000"}
label=${MELEARNER_PERF_LABEL:-baseline}
task_dir="$repo_root/.tmp/012/performance"
pulse_root="$repo_root/.tmp/player-keyboard/pulseaudio/usr"
if [[ -x "$pulse_root/bin/pulseaudio" && -f "$pulse_root/lib/pulseaudio/modules/module-null-sink.so" ]]; then
  export PULSEAUDIO_BIN=${PULSEAUDIO_BIN:-"$pulse_root/bin/pulseaudio"}
  export PULSEAUDIO_MODULE_DIR=${PULSEAUDIO_MODULE_DIR:-"$pulse_root/lib/pulseaudio/modules"}
  export PULSEAUDIO_LIBRARY_PATH=${PULSEAUDIO_LIBRARY_PATH:-"$pulse_root/lib/pulseaudio/modules:$pulse_root/lib/pulseaudio"}
fi

while (($#)); do
  case "$1" in
    --build-dir) (($# >= 2)) || { echo '--build-dir requires a path' >&2; exit 2; }; build_dir=$2; shift 2 ;;
    --sizes) (($# >= 2)) || { echo '--sizes requires a list' >&2; exit 2; }; sizes=${2//,/ }; shift 2 ;;
    -h|--help) echo 'Usage: scripts/measure-cpp-library-performance.sh [--build-dir DIR] [--sizes "10000 30000"]'; exit 0 ;;
    *) echo "Unexpected argument: $1" >&2; exit 2 ;;
  esac
done
[[ "$build_dir" == /* ]] || build_dir="$repo_root/$build_dir"
build_dir="$(cd -- "$build_dir" && pwd)"
test_exe="$build_dir/library_performance_test"
[[ -x "$test_exe" ]] || { echo "Missing $test_exe. Build library_performance_test first." >&2; exit 1; }
for size in $sizes; do
  [[ "$size" == 10000 || "$size" == 30000 ]] || { echo "Supported workload sizes are 10000 and 30000, got $size" >&2; exit 2; }
done

umask 077
mkdir -p "$task_dir/results" "$task_dir/tmp"
chmod 700 "$task_dir" "$task_dir/results" "$task_dir/tmp"
printf 'Environment: %s\n' "$(uname -a)"
printf 'Qt test: %s\n' "$test_exe"
printf 'PulseAudio binary: %s\n' "${PULSEAUDIO_BIN:-$(command -v pulseaudio || true)}"

for size in $sizes; do
  result_dir="$task_dir/results/${label}-${size}-$(date -u +%Y%m%dT%H%M%SZ)"
  mkdir -m 700 -p "$result_dir"
  perf_tmp="$task_dir/tmp/$size"
  mkdir -m 700 -p "$perf_tmp"
  {
    uname -a
    lscpu
    free -h
    printf 'compiler: '
    awk -F= '$1 == "CMAKE_CXX_COMPILER:FILEPATH" { print $2 }' "$build_dir/CMakeCache.txt"
    printf 'build type: '
    awk -F= '$1 == "CMAKE_BUILD_TYPE:STRING" { print $2 }' "$build_dir/CMakeCache.txt"
    if command -v qmake6 >/dev/null; then qmake6 -query QT_VERSION; fi
  } >"$result_dir/environment.txt"
  export MELEARNER_PERF_TMPDIR="$perf_tmp"
  export MELEARNER_PERF_LESSONS="$size"
  export MELEARNER_PLAYBACK_RUN_BASE="$task_dir/playback"
  export MELEARNER_PLAYBACK_TIMEOUT_SECONDS=${MELEARNER_PLAYBACK_TIMEOUT_SECONDS:-600}
  echo "Running $size lessons. Results: $result_dir"
  "$script_dir/test-cpp-playback.sh" -- \
    python3 "$script_dir/sample-process-tree-rss.py" --csv "$result_dir/rss.csv" -- \
      "$test_exe" >"$result_dir/test.log" 2>&1 || {
      status=$?
      cat "$result_dir/test.log"
      exit "$status"
    }
  cat "$result_dir/test.log"
  printf 'RSS samples: %s\n' "$result_dir/rss.csv"
done
