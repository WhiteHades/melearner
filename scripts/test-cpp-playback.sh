#!/usr/bin/env bash
set -euo pipefail

script_path="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)/$(basename -- "${BASH_SOURCE[0]}")"
repo_root="$(cd -- "$(dirname -- "$script_path")/.." && pwd)"

if [[ ${MELEARNER_PLAYBACK_CHILD-} == 1 ]]; then
  [[ ${1-} == --run-one ]] || { printf 'Invalid private runner invocation.\n' >&2; exit 2; }
  shift
  run_dir=$1
  shift
  (( $# > 0 )) || { printf 'Missing playback command.\n' >&2; exit 2; }

  umask 077
  for socket_path in "$run_dir/b" "$run_dir/p"; do
    if (( ${#socket_path} > 107 )); then
      printf 'Private socket path exceeds the Linux Unix-socket limit: %s\n' "$socket_path" >&2
      exit 1
    fi
  done
  runtime="$run_dir/runtime"
  state="$run_dir/state"
  mkdir -m 700 -p "$run_dir"
  chmod 700 "$run_dir"
  mkdir -m 700 -p "$runtime/pulse" "$state" "$run_dir/config/pulse" \
    "$run_dir/cache" "$run_dir/data" "$run_dir/tmp"
  export XDG_RUNTIME_DIR="$runtime"
  export XDG_CONFIG_HOME="$run_dir/config" XDG_CACHE_HOME="$run_dir/cache" XDG_DATA_HOME="$run_dir/data"
  export TMPDIR="$run_dir/tmp"
  export PULSE_RUNTIME_PATH="$runtime/pulse" PULSE_STATE_PATH="$state"
  export PULSE_CONFIG_PATH="$run_dir/no-server-config" PULSE_CLIENTCONFIG="$run_dir/no-client-config"
  export PULSE_COOKIE="$run_dir/config/pulse/cookie" PULSE_SERVER="unix:$run_dir/p"
  export DBUS_SESSION_BUS_ADDRESS="unix:path=$run_dir/b"
  export DBUS_SYSTEM_BUS_ADDRESS="$DBUS_SESSION_BUS_ADDRESS"
  unset NOTIFY_SOCKET WAYLAND_DISPLAY PULSE_SCRIPT PULSE_DLPATH MELEARNER_TEST_SCREENSHOTS QT_STYLE_OVERRIDE
  export QT_QPA_PLATFORM=xcb QT_OPENGL=software QT_QUICK_BACKEND=software
  # Do not inherit the desktop's GTK plugin or force-load a proprietary GLX
  # driver before main. The private display uses Mesa's software renderer.
  export QT_QPA_PLATFORMTHEME=xdgdesktopportal __GLX_VENDOR_LIBRARY_NAME=mesa
  export LIBGL_ALWAYS_SOFTWARE=1 LP_NUM_THREADS=2
  head -c 256 /dev/urandom > "$PULSE_COOKIE"

  dbus_pid=
  openbox_pid=
  pulse_pid=
  command_pid=
  cleanup() {
    local pid
    for pid in "$command_pid" "$pulse_pid" "$openbox_pid" "$dbus_pid"; do
      [[ -n "$pid" ]] && kill "$pid" 2>/dev/null || true
    done
    for pid in "$command_pid" "$pulse_pid" "$openbox_pid" "$dbus_pid"; do
      [[ -n "$pid" ]] && wait "$pid" 2>/dev/null || true
    done
  }
  trap cleanup EXIT
  trap 'exit 130' INT
  trap 'exit 143' TERM

  dbus-daemon --session --nofork --nopidfile --address="$DBUS_SESSION_BUS_ADDRESS" \
    >"$run_dir/dbus.log" 2>&1 &
  dbus_pid=$!
  for _ in {1..100}; do
    [[ -S "$run_dir/b" ]] && break
    kill -0 "$dbus_pid" 2>/dev/null || { printf 'Private D-Bus exited during startup.\n' >&2; exit 1; }
    sleep 0.05
  done
  [[ -S "$run_dir/b" ]] || { printf 'Private D-Bus socket failed to appear.\n' >&2; exit 1; }

  openbox >"$run_dir/openbox.log" 2>&1 &
  openbox_pid=$!
  sleep 0.2
  kill -0 "$openbox_pid" 2>/dev/null || { printf 'Openbox exited during startup.\n' >&2; exit 1; }

  module_native=module-native-protocol-unix
  module_null=module-null-sink
  pulse_env=(env -u PULSE_DLPATH)
  if [[ -n ${PULSEAUDIO_MODULE_DIR:-} ]]; then
    module_native="$PULSEAUDIO_MODULE_DIR/module-native-protocol-unix.so"
    module_null="$PULSEAUDIO_MODULE_DIR/module-null-sink.so"
    pulse_env+=("PULSE_DLPATH=$PULSEAUDIO_MODULE_DIR")
  fi
  if [[ -n ${PULSEAUDIO_LIBRARY_PATH:-} ]]; then
    server_library_path="$PULSEAUDIO_LIBRARY_PATH"
    [[ -z ${LD_LIBRARY_PATH:-} ]] || server_library_path="$server_library_path:$LD_LIBRARY_PATH"
    pulse_env+=("LD_LIBRARY_PATH=$server_library_path")
  fi
  "${pulse_env[@]}" "$PULSEAUDIO_BIN" -n --daemonize=no --use-pid-file=no \
    --disable-shm=yes --exit-idle-time=-1 --log-target=stderr --log-level=info \
    --load="$module_native socket=$run_dir/p auth-anonymous=1" \
    --load="$module_null sink_name=melearner_test_null" \
    >"$run_dir/pulseaudio.log" 2>&1 &
  pulse_pid=$!

  sink_ready=0
  for _ in {1..100}; do
    if kill -0 "$pulse_pid" 2>/dev/null && timeout 2 pactl list short sinks >"$run_dir/sinks.log" 2>&1; then
      mapfile -t sinks < "$run_dir/sinks.log"
      if (( ${#sinks[@]} == 1 )) && [[ ${sinks[0]} == *melearner_test_null* ]]; then
        sink_ready=1
        break
      fi
    fi
    kill -0 "$pulse_pid" 2>/dev/null || break
    sleep 0.1
  done
  if (( sink_ready != 1 )); then
    printf 'Private null sink failed to become ready; inspect logs in %s\n' "$run_dir" >&2
    exit 1
  fi

  printf 'Private null sink confirmed; running:' >&2
  printf ' %q' "$@" >&2
  printf '\n' >&2
  set +e
  LC_ALL=C timeout --signal=TERM --kill-after=1s "${MELEARNER_PLAYBACK_TIMEOUT_SECONDS}s" \
    "$@" >"$run_dir/command.log" 2>&1 &
  command_pid=$!
  wait "$command_pid"
  status=$?
  command_pid=
  set -e
  if (( status == 0 )); then
    cat "$run_dir/command.log"
  else
    tail -n 120 "$run_dir/command.log"
  fi
  exit "$status"
fi

usage() {
  cat <<'USAGE'
Usage: scripts/test-cpp-playback.sh [--build-dir DIR] [-- COMMAND [ARG...]]

Runs main_window_test and main_playback_test from DIR. Supplying a command
after -- runs only that command in the same private X11/audio environment.
MELEARNER_PLAYBACK_TIMEOUT_SECONDS defaults to 90 seconds per command.
Set that environment variable to override the limit.
USAGE
}

build_dir=${MELEARNER_BUILD_DIR:-"$repo_root/build/cpp-dev"}
explicit_command=0
command_args=()
while (( $# > 0 )); do
  case "$1" in
    --build-dir)
      (( $# >= 2 )) || { printf -- '--build-dir requires a directory.\n' >&2; exit 2; }
      build_dir=$2
      shift 2
      ;;
    --)
      explicit_command=1
      shift
      command_args=("$@")
      break
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      printf 'Unexpected argument: %s\n' "$1" >&2
      usage >&2
      exit 2
      ;;
  esac
done

if (( explicit_command )); then
  (( ${#command_args[@]} > 0 )) || { printf 'Missing command after --.\n' >&2; exit 2; }
else
  if [[ "$build_dir" != /* ]]; then build_dir="$repo_root/$build_dir"; fi
  build_dir="$(cd -- "$build_dir" 2>/dev/null && pwd)" || {
    printf 'Build directory does not exist: %s\n' "$build_dir" >&2
    exit 1
  }
  test_paths=()
  for test_name in main_window_test main_playback_test; do
    test_path="$build_dir/$test_name"
    [[ -x "$test_path" ]] || { printf 'Missing playback test executable: %s\n' "$test_path" >&2; exit 1; }
    test_paths+=("$test_path")
  done
fi

MELEARNER_PLAYBACK_TIMEOUT_SECONDS=${MELEARNER_PLAYBACK_TIMEOUT_SECONDS:-90}
[[ $MELEARNER_PLAYBACK_TIMEOUT_SECONDS =~ ^[1-9][0-9]*$ ]] || {
  printf 'MELEARNER_PLAYBACK_TIMEOUT_SECONDS must be a positive integer.\n' >&2
  exit 2
}
export MELEARNER_PLAYBACK_TIMEOUT_SECONDS

if [[ -z ${PULSEAUDIO_BIN:-} ]]; then
  PULSEAUDIO_BIN="$(command -v pulseaudio || true)"
elif [[ "$PULSEAUDIO_BIN" != */* ]]; then
  PULSEAUDIO_BIN="$(command -v "$PULSEAUDIO_BIN" || true)"
fi
[[ -n ${PULSEAUDIO_BIN:-} && -x $PULSEAUDIO_BIN ]] || {
  printf 'Set PULSEAUDIO_BIN to an executable PulseAudio server.\n' >&2
  exit 1
}
if [[ -n ${PULSEAUDIO_MODULE_DIR:-} ]]; then
  PULSEAUDIO_MODULE_DIR="$(cd -- "$PULSEAUDIO_MODULE_DIR" 2>/dev/null && pwd)" || {
    printf 'PulseAudio module directory does not exist: %s\n' "$PULSEAUDIO_MODULE_DIR" >&2
    exit 1
  }
  for module in module-native-protocol-unix.so module-null-sink.so; do
    [[ -f "$PULSEAUDIO_MODULE_DIR/$module" ]] || {
      printf 'Missing required PulseAudio module: %s/%s\n' "$PULSEAUDIO_MODULE_DIR" "$module" >&2
      exit 1
    }
  done
fi
export PULSEAUDIO_BIN PULSEAUDIO_MODULE_DIR PULSEAUDIO_LIBRARY_PATH

for executable in Xvfb xvfb-run xauth openbox dbus-daemon pactl timeout; do
  command -v "$executable" >/dev/null || {
    printf 'Required executable not found: %s\n' "$executable" >&2
    exit 127
  }
done

run_base="$repo_root/.tmp/cpp-playback"
socket_label=main_playback_test
(( explicit_command )) && socket_label=custom
socket_probe="$run_base/run.XXXXXX/$socket_label/p"
if (( ${#socket_probe} > 107 )); then
  printf 'Playback socket path would exceed the Linux Unix-socket limit: %s\n' "$socket_probe" >&2
  printf 'Use a checkout path short enough for private sockets under .tmp.\n' >&2
  exit 1
fi
umask 077
mkdir -p "$run_base"
chmod 700 "$run_base"
run_root="$(mktemp -d "$run_base/run.XXXXXX")"
printf 'Playback logs: %s\n' "$run_root" >&2

run_one() {
  local name=$1
  shift
  local run_dir="$run_root/$name"
  mkdir -m 700 "$run_dir"
  local status
  if xvfb-run -a -s '-screen 0 1600x1000x24 -nolisten tcp -noreset' \
    env MELEARNER_PLAYBACK_CHILD=1 bash "$script_path" --run-one "$run_dir" "$@" \
    >"$run_dir/runner.log" 2>&1; then
    status=0
  else
    status=$?
  fi
  cat "$run_dir/runner.log"
  printf 'Logs for %s: %s\n' "$name" "$run_dir" >&2
  return "$status"
}

if (( explicit_command )); then
  run_one custom "${command_args[@]}" || exit $?
else
  for index in "${!test_paths[@]}"; do
    test_name=main_window_test
    (( index == 0 )) || test_name=main_playback_test
    run_one "$test_name" "${test_paths[$index]}" || exit $?
  done
fi
