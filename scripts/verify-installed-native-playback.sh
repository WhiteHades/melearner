#!/usr/bin/env bash
set -euo pipefail

app_bin="${MELEARNER_APP_BIN:-/usr/bin/melearner}"
db_path="${MELEARNER_DB_PATH:-$HOME/.local/share/melearner/melearner.db}"
frontend_log="${MELEARNER_FRONTEND_LOG:-$HOME/.melearner/frontend.log}"
surface_log="${MELEARNER_NATIVE_SURFACE_LOG:-$HOME/.melearner/native-surface.log}"
course_id="${1:-${MELEARNER_OPEN_COURSE_ID:-}}"
lesson_id="${2:-${MELEARNER_OPEN_LESSON_ID:-}}"

if [[ ! -x "$app_bin" ]]; then
  echo "installed app is not executable: $app_bin" >&2
  exit 1
fi

if [[ -z "$course_id" || -z "$lesson_id" ]]; then
  if [[ ! -f "$db_path" ]]; then
    echo "database is missing and no course/lesson ids were provided: $db_path" >&2
    exit 1
  fi
  if ! command -v sqlite3 >/dev/null 2>&1; then
    echo "sqlite3 is required when course/lesson ids are not provided" >&2
    exit 1
  fi
  row="$(sqlite3 -separator $'\t' "$db_path" "select c.id, l.id from lessons l join courses c on c.id = l.course_id where l.type = 'video' order by c.name collate nocase, l.order_index limit 1;")"
  course_id="${row%%$'\t'*}"
  lesson_id="${row#*$'\t'}"
fi

if [[ -z "$course_id" || -z "$lesson_id" || "$course_id" == "$lesson_id" ]]; then
  echo "could not resolve a playable course/lesson pair" >&2
  exit 1
fi

mkdir -p "$(dirname "$frontend_log")" "$(dirname "$surface_log")"
touch "$frontend_log" "$surface_log"
frontend_start_lines="$(wc -l < "$frontend_log")"
surface_start_lines="$(wc -l < "$surface_log")"

pkill -x melearner >/dev/null 2>&1 || true

if command -v hyprctl >/dev/null 2>&1; then
  hyprctl dispatch exec "[workspace 2 silent] env MELEARNER_OPEN_COURSE_ID=$course_id MELEARNER_OPEN_LESSON_ID=$lesson_id MELEARNER_NATIVE_SURFACE_LOG=$surface_log $app_bin" >/dev/null
else
  MELEARNER_OPEN_COURSE_ID="$course_id" \
    MELEARNER_OPEN_LESSON_ID="$lesson_id" \
    MELEARNER_NATIVE_SURFACE_LOG="$surface_log" \
    "$app_bin" >/dev/null 2>&1 &
fi

cleanup() {
  pkill -x melearner >/dev/null 2>&1 || true
}
trap cleanup EXIT

ready_line=""
for _ in {1..90}; do
  ready_line="$(tail -n +"$((frontend_start_lines + 1))" "$frontend_log" | rg 'native\.player\.load\.ready' | tail -1 || true)"
  if [[ -n "$ready_line" ]]; then
    break
  fi
  if tail -n +"$((frontend_start_lines + 1))" "$frontend_log" | rg -q 'native\.player\.load\.failed|native-player://error|app\.error|app\.unhandledRejection'; then
    echo "native playback failed before ready:" >&2
    tail -n +"$((frontend_start_lines + 1))" "$frontend_log" | tail -80 >&2
    exit 1
  fi
  sleep 1
done

if [[ -z "$ready_line" ]]; then
  echo "native player did not report ready within 90s" >&2
  tail -n +"$((frontend_start_lines + 1))" "$frontend_log" | tail -80 >&2
  exit 1
fi

for expected in '"surfaceAttached":true' '"surfaceBackend":"render-api:gtk-opengl"' '"surfaceRenderApi":true' '"surfaceRenderThreadAlive":true' '"surfaceRenderError":null'; do
  if [[ "$ready_line" != *"$expected"* ]]; then
    echo "native player ready line is missing $expected" >&2
    echo "$ready_line" >&2
    exit 1
  fi
done

frames="$(sed -n 's/.*"surfaceRenderedFrames":\([0-9][0-9]*\).*/\1/p' <<<"$ready_line")"
width="$(sed -n 's/.*"surfaceRenderWidth":\([0-9][0-9]*\).*/\1/p' <<<"$ready_line")"
height="$(sed -n 's/.*"surfaceRenderHeight":\([0-9][0-9]*\).*/\1/p' <<<"$ready_line")"

if [[ -z "$frames" || -z "$width" || -z "$height" || "$frames" -lt 1 || "$width" -lt 2 || "$height" -lt 2 ]]; then
  echo "native player reported invalid render diagnostics" >&2
  echo "$ready_line" >&2
  exit 1
fi

if ! tail -n +"$((surface_start_lines + 1))" "$surface_log" | rg -q 'native gtk render-api submitted first frame'; then
  echo "native surface log did not record a submitted first frame" >&2
  tail -n +"$((surface_start_lines + 1))" "$surface_log" >&2
  exit 1
fi

if command -v hyprctl >/dev/null 2>&1 && command -v jq >/dev/null 2>&1; then
  melearner_clients="$(hyprctl clients -j | jq -r '.[] | select((.class | test("melearner"; "i")) or (.title | test("^(melearner|melearner video|local course learner)$"; "i"))) | [.workspace.name, .class, .title] | @tsv')"
  melearner_count="$(wc -l <<<"$melearner_clients" | tr -d ' ')"
  if [[ "$melearner_count" -ne 1 ]] || rg -qi $'\t.*video' <<<"$melearner_clients"; then
    echo "expected one melearner app window and no separate video window" >&2
    echo "$melearner_clients" >&2
    exit 1
  fi
fi

echo "native playback verified: course=$course_id lesson=$lesson_id frames=$frames surface=${width}x${height}"
