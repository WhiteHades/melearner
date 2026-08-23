#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 1 ]]; then
  echo "usage: scripts/audit-native-linux-package.sh <AppDir>" >&2
  exit 2
fi

app_dir="${1%/}"
binary="$app_dir/usr/bin/melearner"
if [[ ! -d "$app_dir" || ! -x "$binary" ]]; then
  echo "native AppDir or executable is missing: $app_dir" >&2
  exit 1
fi
for command in file ldd readelf; do
  if ! command -v "$command" >/dev/null; then
    echo "$command is required for the native package audit" >&2
    exit 1
  fi
done

required_files=(
  "usr/share/applications/io.github.whitehades.melearner.desktop"
  "usr/share/icons/hicolor/512x512/apps/io.github.whitehades.melearner.png"
  "usr/share/licenses/melearner/LICENSE"
)
for required in "${required_files[@]}"; do
  if [[ ! -s "$app_dir/$required" ]]; then
    echo "required package file is missing: $required" >&2
    exit 1
  fi
done

unexpected_executable="$(find "$app_dir/usr/bin" -mindepth 1 -maxdepth 1 -type f ! -name melearner -print -quit)"
if [[ -n "$unexpected_executable" ]]; then
  echo "unexpected package executable: $unexpected_executable" >&2
  exit 1
fi
for helper in mpv ffmpeg ffprobe node nodejs chromium chrome; do
  if find "$app_dir" -type f -name "$helper" -print -quit | grep -q .; then
    echo "forbidden helper executable is staged: $helper" >&2
    exit 1
  fi
done
if find "$app_dir/usr/share/melearner/resources" -type f \
  \( -iname '*.js' -o -iname '*.mjs' -o -iname '*.cjs' -o -iname '*.html' -o -iname '*.htm' \) \
  -print -quit | grep -q .; then
  echo "browser application assets are staged in the native package" >&2
  exit 1
fi

mapfile -d '' elf_files < <(
  find "$app_dir" -type f -print0 | while IFS= read -r -d '' candidate; do
    if file -b "$candidate" | grep -q '^ELF '; then
      printf '%s\0' "$candidate"
    fi
  done
)
if [[ ${#elf_files[@]} -eq 0 ]]; then
  echo "native package contains no ELF files" >&2
  exit 1
fi

dynamic_inventory=""
for elf in "${elf_files[@]}"; do
  dynamic="$(readelf -dW "$elf" 2>/dev/null || true)"
  dynamic_inventory+=$'\n'"$elf"$'\n'"$dynamic"
  if grep -Eiq 'webkit|javascriptcore|webview2|cef|electron|tauri' <<<"$dynamic"; then
    echo "forbidden browser or transitional-shell import in $elf" >&2
    exit 1
  fi
  if grep -Eiq '\((RPATH|RUNPATH)\).*(/home/|/opt/|/usr/local/|/nix/store/|/var/)' <<<"$dynamic"; then
    echo "non-portable build-host path in $elf" >&2
    exit 1
  fi
done

if ! grep -Eq 'libmpv\.so' <<<"$dynamic_inventory"; then
  echo "native package does not contain a linked libmpv runtime" >&2
  exit 1
fi
if ! find "$app_dir" -type f -name 'libpdfium.so*' -print -quit | grep -q .; then
  echo "native package does not contain the PDFium runtime" >&2
  exit 1
fi

library_path="$app_dir/usr/lib:$app_dir/usr/lib/melearner"
for elf in "${elf_files[@]}"; do
  linkage="$(LD_LIBRARY_PATH="$library_path" ldd "$elf" 2>&1 || true)"
  if grep -Fq 'not found' <<<"$linkage"; then
    printf 'unresolved package dependency for %s:\n%s\n' "$elf" "$linkage" >&2
    exit 1
  fi
  while IFS= read -r dependency; do
    [[ -z "$dependency" ]] && continue
    resolved="$(awk '{ print $3 }' <<<"$dependency")"
    case "$resolved" in
      "$app_dir"/*) ;;
      *)
        printf 'private runtime dependency escaped AppDir for %s: %s\n' "$elf" "$dependency" >&2
        exit 1
        ;;
    esac
  done < <(grep -E 'lib(mpv|pdfium|avcodec|avformat|avutil|avfilter|avdevice|swresample|swscale)\.so' <<<"$linkage" || true)
done

echo "native Linux package audit passed: ${#elf_files[@]} ELF files"
