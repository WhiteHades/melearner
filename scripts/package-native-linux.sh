#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat >&2 <<'EOF'
usage: scripts/package-native-linux.sh --format appimage|arch-stage \
  --binary <path> --resources <path> --pdfium <path> --output <path>
EOF
}

format=""
binary=""
resources=""
pdfium=""
output=""

while [[ $# -gt 0 ]]; do
  case "$1" in
    --format) format="${2:-}"; shift 2 ;;
    --binary) binary="${2:-}"; shift 2 ;;
    --resources) resources="${2:-}"; shift 2 ;;
    --pdfium) pdfium="${2:-}"; shift 2 ;;
    --output) output="${2:-}"; shift 2 ;;
    *) usage; exit 2 ;;
  esac
done

if [[ "$format" != "appimage" && "$format" != "arch-stage" ]]; then
  usage
  exit 2
fi
for required in binary resources pdfium output; do
  if [[ -z "${!required}" ]]; then
    usage
    exit 2
  fi
done
if [[ ! -f native-app/app.zon ]]; then
  echo "run this script from the melearner repository root" >&2
  exit 1
fi
if [[ ! -x "$binary" ]]; then
  echo "native binary is missing or not executable: $binary" >&2
  exit 1
fi
if [[ ! -d "$resources" ]]; then
  echo "native package resources are missing: $resources" >&2
  exit 1
fi
if [[ -n "$pdfium" && ! -f "$pdfium" ]]; then
  echo "PDFium library is missing: $pdfium" >&2
  exit 1
fi
if ! command -v linuxdeploy >/dev/null; then
  echo "linuxdeploy is required" >&2
  exit 1
fi
if ! command -v patchelf >/dev/null; then
  echo "patchelf is required" >&2
  exit 1
fi
if [[ "$format" == "appimage" ]]; then
  if ! command -v appimagetool >/dev/null; then
    echo "appimagetool is required for AppImage output" >&2
    exit 1
  fi
  if [[ -z "${APPIMAGE_RUNTIME_FILE:-}" || ! -f "$APPIMAGE_RUNTIME_FILE" ]]; then
    echo "APPIMAGE_RUNTIME_FILE must point to the pinned type-2 runtime" >&2
    exit 1
  fi
fi

mkdir -p .tmp
work_dir="$(mktemp -d "$PWD/.tmp/native-linux-package.XXXXXX")"
trap 'rm -rf "$work_dir"' EXIT
app_dir="$work_dir/AppDir"
install -d \
  "$app_dir/usr/bin" \
  "$app_dir/usr/share/applications" \
  "$app_dir/usr/share/icons/hicolor/512x512/apps" \
  "$app_dir/usr/share/melearner/resources" \
  "$app_dir/usr/share/licenses/melearner"
install -Dm755 "$binary" "$app_dir/usr/bin/melearner"
install -Dm644 packaging/linux/io.github.whitehades.melearner.appimage.desktop \
  "$app_dir/usr/share/applications/io.github.whitehades.melearner.desktop"
install -Dm644 native-app/assets/icon.png \
  "$app_dir/usr/share/icons/hicolor/512x512/apps/io.github.whitehades.melearner.png"
cp -a "$resources"/. "$app_dir/usr/share/melearner/resources/"
install -Dm644 LICENSE "$app_dir/usr/share/licenses/melearner/LICENSE"

linuxdeploy_args=(
  --appdir "$app_dir"
  --executable "$app_dir/usr/bin/melearner"
  --desktop-file "$app_dir/usr/share/applications/io.github.whitehades.melearner.desktop"
  --icon-file "$app_dir/usr/share/icons/hicolor/512x512/apps/io.github.whitehades.melearner.png"
)
if [[ -n "$pdfium" ]]; then
  linuxdeploy_args+=(--library "$pdfium")
fi
NO_STRIP=true linuxdeploy "${linuxdeploy_args[@]}"

scripts/audit-native-linux-package.sh "$app_dir"

case "$format" in
  appimage)
    if [[ -e "$output" ]]; then
      echo "refusing to overwrite output: $output" >&2
      exit 1
    fi
    output_dir="$(dirname "$output")"
    mkdir -p "$output_dir"
    output_dir="$(cd "$output_dir" && pwd)"
    output="$output_dir/$(basename "$output")"
    ARCH=x86_64 appimagetool --runtime-file "$APPIMAGE_RUNTIME_FILE" "$app_dir" "$output"
    test -s "$output"
    ;;
  arch-stage)
    if [[ -e "$output" ]]; then
      echo "refusing to overwrite output: $output" >&2
      exit 1
    fi
    mkdir -p "$app_dir/usr/lib/melearner"
    if [[ -d "$app_dir/usr/lib" ]]; then
      find "$app_dir/usr/lib" -mindepth 1 -maxdepth 1 ! -name melearner -exec mv -t "$app_dir/usr/lib/melearner" {} +
    fi
    patchelf --set-rpath '$ORIGIN/../lib/melearner' "$app_dir/usr/bin/melearner"
    find "$app_dir/usr/lib/melearner" -type f -exec sh -c '
      for file do
        if file "$file" | grep -q "ELF"; then
          patchelf --set-rpath '"'"'$ORIGIN'"'"' "$file"
        fi
      done
    ' sh {} +
    install -Dm644 packaging/arch/io.github.whitehades.melearner.desktop \
      "$app_dir/usr/share/applications/io.github.whitehades.melearner.desktop"
    mkdir -p "$(dirname "$output")"
    mv "$app_dir" "$output"
    ;;
esac
