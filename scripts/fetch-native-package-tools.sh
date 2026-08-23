#!/usr/bin/env bash
set -euo pipefail

if [[ ! -f native-app/app.zon ]]; then
  echo "run this script from the melearner repository root" >&2
  exit 1
fi

if [[ $# -ne 2 ]]; then
  echo "usage: scripts/fetch-native-package-tools.sh <linux-x64> <repo-relative-output-directory>" >&2
  exit 1
fi

target="$1"
output="$2"
if [[ "$target" != "linux-x64" ]]; then
  echo "unsupported package-tool target: $target" >&2
  exit 1
fi
case "$output" in
  "" | /* | *"/../"* | ../* | */..)
    echo "package-tool output must be a safe repository-relative directory" >&2
    exit 1
    ;;
esac

if [[ -d "$output" ]]; then
  first_output_entry="$(find "$output" -mindepth 1 -print -quit)"
  if [[ -n "$first_output_entry" ]]; then
    echo "package-tool output directory must be empty: $output" >&2
    exit 1
  fi
fi

work_root="$PWD/.tmp/fetch-native-package-tools"
rm -rf "$work_root"
mkdir -p "$work_root" "$output"
trap 'rm -rf "$work_root"' EXIT

fetch() {
  local name="$1"
  local url="$2"
  local expected_sha256="$3"
  local archive="$work_root/$name"

  curl \
    --fail \
    --location \
    --proto '=https' \
    --retry 3 \
    --show-error \
    --silent \
    --tlsv1.2 \
    "$url" \
    --output "$archive"
  printf '%s  %s\n' "$expected_sha256" "$archive" | sha256sum --check --strict
  install -Dm755 "$archive" "$output/$name"
}

fetch \
  linuxdeploy \
  "https://github.com/linuxdeploy/linuxdeploy/releases/download/1-alpha-20251107-1/linuxdeploy-x86_64.AppImage" \
  "c20cd71e3a4e3b80c3483cef793cda3f4e990aca14014d23c544ca3ce1270b4d"
fetch \
  appimagetool \
  "https://github.com/AppImage/appimagetool/releases/download/1.9.1/appimagetool-x86_64.AppImage" \
  "ed4ce84f0d9caff66f50bcca6ff6f35aae54ce8135408b3fa33abfc3cb384eb0"
fetch \
  runtime-x86_64 \
  "https://github.com/AppImage/type2-runtime/releases/download/20251108/runtime-x86_64" \
  "2fca8b443c92510f1483a883f60061ad09b46b978b2631c807cd873a47ec260d"
