#!/usr/bin/env bash
set -euo pipefail

if [[ ! -f native-app/app.zon || ! -d crates/melearner-core ]]; then
  echo "run this script from the melearner repository root" >&2
  exit 1
fi

if [[ $# -ne 2 ]]; then
  echo "usage: scripts/fetch-pdfium.sh <linux-x64|mac-univ|win-x64> <repo-relative-output-directory>" >&2
  exit 1
fi

target="$1"
output="$2"
case "$output" in
  "" | /* | *"/../"* | ../* | */..)
    echo "PDFium output must be a safe repository-relative directory" >&2
    exit 1
    ;;
esac

case "$target" in
  linux-x64)
    archive_name="pdfium-linux-x64.tgz"
    expected_sha256="019665c8877d46fe65f625f80fd714ab07aac68554b0636acf2a2adf9288adb2"
    ;;
  mac-univ)
    archive_name="pdfium-mac-univ.tgz"
    expected_sha256="432ba1831a4581cb0d52550fdad977d0ebf4f31188223b5ddd99b9b89d7124fe"
    ;;
  win-x64)
    archive_name="pdfium-win-x64.tgz"
    expected_sha256="88276459349b291c41f10422dad0210f007c04d919c8fa56472b6b7c6406adf4"
    ;;
  *)
    echo "unsupported PDFium target: $target" >&2
    exit 1
    ;;
esac

if [[ -d "$output" ]]; then
  first_output_entry="$(find "$output" -mindepth 1 -print -quit)"
  if [[ -n "$first_output_entry" ]]; then
    echo "PDFium output directory must be empty: $output" >&2
    exit 1
  fi
fi

release="chromium/7961"
work_root="$PWD/.tmp/fetch-pdfium"
archive="$work_root/$archive_name"
rm -rf "$work_root"
mkdir -p "$work_root" "$output"
trap 'rm -rf "$work_root"' EXIT

curl \
  --fail \
  --location \
  --proto '=https' \
  --retry 3 \
  --show-error \
  --silent \
  --tlsv1.2 \
  "https://github.com/bblanchon/pdfium-binaries/releases/download/$release/$archive_name" \
  --output "$archive"

if command -v sha256sum >/dev/null 2>&1; then
  printf '%s  %s\n' "$expected_sha256" "$archive" | sha256sum --check --strict
elif command -v shasum >/dev/null 2>&1; then
  actual_sha256="$(shasum -a 256 "$archive" | awk '{ print $1 }')"
  if [[ "$actual_sha256" != "$expected_sha256" ]]; then
    echo "PDFium archive checksum mismatch" >&2
    exit 1
  fi
else
  echo "sha256sum or shasum is required to verify PDFium" >&2
  exit 1
fi
tar -tzf "$archive" | awk '
  /(^\/|(^|\/)\.\.(\/|$))/ {
    print "unsafe PDFium archive path: " $0 > "/dev/stderr"
    invalid = 1
  }
  END { exit invalid ? 1 : 0 }
'
tar --extract --gzip --file "$archive" --directory "$output"

grep -Fxq "MAJOR=152" "$output/VERSION"
grep -Fxq "BUILD=7961" "$output/VERSION"
test -s "$output/LICENSE"
test -d "$output/licenses"
