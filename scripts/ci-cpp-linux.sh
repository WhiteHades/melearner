#!/usr/bin/env bash
# Run inside the disposable CI container as its unprivileged build user.
set -euo pipefail

repo_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo_root"
if [[ "$(uname -s)" != Linux || "$EUID" == 0 ]]; then
  echo "Run C++ Linux CI on Linux as an unprivileged user" >&2
  exit 1
fi
for tool in cmake ctest ninja c++ pkg-config python3 xvfb-run timeout pulseaudio pactl ffmpeg; do
  command -v "$tool" >/dev/null || { echo "Missing CI tool: $tool" >&2; exit 1; }
done

log_dir="$repo_root/.tmp/cpp-ci"
mkdir -p -- "$log_dir/screenshots"
run_root="$(mktemp -d)"
pulse_started=false
cleanup() {
  if [[ "$pulse_started" == true ]]; then
    pulseaudio --kill >/dev/null 2>&1 || true
  fi
  rm -rf -- "$run_root"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

# No developer configuration, user database, or audio device is required.
export HOME="$run_root/home"
export XDG_DATA_HOME="$run_root/data"
export XDG_CONFIG_HOME="$run_root/config"
export XDG_CACHE_HOME="$run_root/cache"
export XDG_RUNTIME_DIR="$run_root/runtime"
mkdir -p -- "$HOME" "$XDG_DATA_HOME" "$XDG_CONFIG_HOME" "$XDG_CACHE_HOME" "$XDG_RUNTIME_DIR"
chmod 700 "$XDG_RUNTIME_DIR"
unset DESTDIR QT_PLUGIN_PATH QT_QPA_PLATFORM_PLUGIN_PATH QT_QPA_PLATFORM
unset LD_LIBRARY_PATH LD_PRELOAD LD_AUDIT

{
  cmake --version
  c++ --version
  printf 'Available CPUs: '; nproc
  if [[ -r /sys/fs/cgroup/cpu.max ]]; then cat /sys/fs/cgroup/cpu.max; fi
  pkg-config --modversion Qt6Widgets Qt6Pdf sqlite3 mpv libzip md4c
  if command -v pacman >/dev/null; then pacman -Q; fi
} >"$log_dir/toolchain.txt" 2>&1

cmake --preset linux-release 2>&1 | tee "$log_dir/configure.log"
cmake --build --preset linux-release --parallel 2 2>&1 | tee "$log_dir/build.log"
# CTest also plays media. Start the null sink before any player test runs.
unset PULSE_SERVER PULSE_RUNTIME_PATH PULSE_CLIENTCONFIG
pulseaudio --start --exit-idle-time=-1 --log-target="file:$log_dir/pulse.log"
pulse_started=true
pactl load-module module-null-sink sink_name=melearner_ci >"$log_dir/pulse-module.txt"
pactl set-default-sink melearner_ci

ctest --preset linux-release --no-tests=error --output-junit "$log_dir/ctest.xml" \
  2>&1 | tee "$log_dir/ctest.log"

install_prefix="$run_root/install prefix"
cmake --install build/cpp-release --prefix "$install_prefix" 2>&1 | tee "$log_dir/install.log"
test -x "$install_prefix/bin/melearner"
installed_version="$(QT_QPA_PLATFORM=offscreen "$install_prefix/bin/melearner" --version)"
printf '%s\n' "$installed_version" | tee "$log_dir/installed-version.txt"
[[ "$installed_version" == *" 0.1.9" ]] || { echo "Unexpected installed version" >&2; exit 1; }

# Keep the software renderer's worker pool bounded on shared CI runners.
export LIBGL_ALWAYS_SOFTWARE=1
export LP_NUM_THREADS=2
# Independent decoder output distinguishes corpus content from display defects.
ffmpeg -hide_banner -loglevel error -y \
  -i 'fixtures/parity/media/Systems 日本語/01 H264 AAC.mp4' \
  -frames:v 1 "$log_dir/screenshots/reference-h264.png"
ffmpeg -hide_banner -loglevel error -y \
  -i 'fixtures/parity/media/03 HEVC Main 10.mkv' \
  -frames:v 1 "$log_dir/screenshots/reference-hevc.png"
export MELEARNER_TEST_SCREENSHOTS="$log_dir/screenshots"
playback_failed=false
for test in playback_render main_playback; do
  if xvfb-run -a -s '-screen 0 1920x1080x24' \
    env QT_QPA_PLATFORM=xcb timeout 120 "./build/cpp-release/${test}_test" \
    2>&1 | tee "$log_dir/${test}.log"; then
    printf '%s passed\n' "$test"
  else
    printf '%s failed; see %s\n' "$test" "$log_dir/${test}.log" >&2
    playback_failed=true
  fi
done
if [[ "$playback_failed" == true ]]; then
  exit 1
fi
printf '%s\n' 'C++ build, CTest, source-install smoke, and Xvfb playback checks passed.' \
  'This is not AppImage/Arch package, Wayland, GPU, or release qualification.'
