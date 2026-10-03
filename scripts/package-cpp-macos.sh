#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
version="0.1.9"
build_dir="${repo_root}/build/macos"
output="${repo_root}/dist/melearner-${version}-macos-arm64.dmg"

usage() {
  cat <<'EOF'
usage: scripts/package-cpp-macos.sh [--build-dir <path>] [--output <path>]

Create an ad hoc-signed diagnostic arm64 macOS DMG. It has no trusted publisher
signature and is not notarized or release-qualified.
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --build-dir|--output)
      [[ $# -ge 2 && -n "$2" && "$2" != --* ]] || { usage >&2; exit 2; }
      case "$1" in
        --build-dir) build_dir="$2" ;;
        --output) output="$2" ;;
      esac
      shift 2
      ;;
    -h|--help) usage; exit 0 ;;
    *) usage >&2; exit 2 ;;
  esac
done

[[ "$(uname -s)" == Darwin ]] || { echo "macOS packaging requires macOS" >&2; exit 1; }
for tool in macdeployqt hdiutil codesign otool file shasum jq brew lipo ditto cmake awk sort find cp; do
  command -v "$tool" >/dev/null 2>&1 || { echo "required packaging tool is missing: $tool" >&2; exit 1; }
done

[[ "$build_dir" == /* ]] || build_dir="$repo_root/$build_dir"
[[ "$output" == /* ]] || output="$repo_root/$output"
bundle="$build_dir/melearner.app"
[[ -f "$build_dir/CMakeCache.txt" ]] || { echo "configured CMake build directory is missing: $build_dir" >&2; exit 1; }
project_version="$(awk -F= '$1 == "CMAKE_PROJECT_VERSION:STATIC" { print $2 }' "$build_dir/CMakeCache.txt")"
[[ "$project_version" == "$version" ]] || { echo "CMake build version must be $version, got ${project_version:-missing}" >&2; exit 1; }
[[ "$(basename -- "$output")" == "melearner-${version}-macos-arm64.dmg" ]] || { echo "output filename must be melearner-${version}-macos-arm64.dmg" >&2; exit 1; }
[[ ! -e "$output" && ! -L "$output" && ! -e "${output}.sha256" ]] || { echo "refusing to overwrite DMG or checksum: $output" >&2; exit 1; }

app_arch="$(lipo -archs "$bundle/Contents/MacOS/melearner")"
[[ "$app_arch" == arm64 ]] || { echo "expected arm64-only app, got architectures: $app_arch" >&2; exit 1; }
output_dir="$(dirname -- "$output")"
mkdir -p -- "$output_dir"
work_dir="$(mktemp -d "$output_dir/.melearner-macos.XXXXXX")"
trap 'rm -rf -- "$work_dir"' EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

stage="$work_dir/stage"
mkdir -p "$stage"
install_root="$work_dir/install"
cmake --install "$build_dir" --prefix "$install_root" --config Release
[[ -d "$install_root/melearner.app" ]] || { echo "CMake install did not produce melearner.app" >&2; exit 1; }
ditto "$install_root/melearner.app" "$stage/melearner.app"
macdeployqt "$stage/melearner.app" -always-overwrite -codesign=-

# CMake installs Lexbor and shadcn-cpp's original notices alongside the app.
installed_licenses="$install_root/share/licenses/melearner"
for notice in \
  lexbor/LICENSE lexbor/NOTICE \
  shadcn-cpp/LICENSE shadcn-cpp/shadcn-MIT.txt \
  shadcn-cpp/ui-components-MIT.txt shadcn-cpp/Geist-OFL.txt; do
  [[ -s "$installed_licenses/$notice" ]] || { echo "missing bundled dependency notice: $installed_licenses/$notice" >&2; exit 1; }
done
license_root="$stage/melearner.app/Contents/Resources/Licenses"
mkdir -p "$license_root"
ditto "$repo_root/LICENSE" "$license_root/melearner-LICENSE"
ditto "$installed_licenses" "$license_root/cmake-installed"

# Record the exact installed Homebrew keg receipt, and copy its real license,
# copyright, and notice files. Formula metadata must agree with that receipt;
# this prevents labeling an installed old keg with today's stable version.
formula_list="$work_dir/formulas"
{
  printf '%s\n' qtbase qttools qtwebengine sqlite mpv libzip md4c ffmpeg
  brew deps --installed --recursive qtbase qttools qtwebengine sqlite mpv libzip md4c ffmpeg
} | sort -u > "$formula_list"
notices="$stage/melearner.app/Contents/Resources/THIRD_PARTY_NOTICES.txt"
{
  echo "melearner ${version} macOS runtime dependency notices"
  echo
  echo "Ad hoc-signed diagnostic arm64 build; no trusted publisher signature, not notarized or release-qualified."
  echo "The original dependency license, copyright and notice texts are under Licenses/homebrew/."
  echo "Formula source provenance below comes from Homebrew metadata and each keg's INSTALL_RECEIPT.json."
  echo ""
  while IFS= read -r formula; do
    [[ -n "$formula" ]] || continue
    prefix="$(brew --prefix "$formula")"
    keg="$(cd -- "$prefix" && pwd -P)"
    receipt="$keg/INSTALL_RECEIPT.json"
    [[ -s "$receipt" ]] || { echo "missing Homebrew receipt: $receipt" >&2; exit 1; }
    installed_version="$(jq -r '.source.versions.stable // empty' "$receipt")"
    [[ -n "$installed_version" ]] || { echo "Homebrew receipt has no installed version for $formula: $receipt" >&2; exit 1; }
    metadata="$(brew info --json=v2 "$formula")"
    formula_record="$(jq -c '.formulae[0]' <<<"$metadata")"
    metadata_version="$(jq -r '.versions.stable // empty' <<<"$formula_record")"
    [[ "$metadata_version" == "$installed_version" ]] || {
      echo "Homebrew metadata does not describe installed $formula $installed_version (metadata=${metadata_version:-missing})" >&2
      exit 1
    }
    source_url="$(jq -r '.urls.stable.url // empty' <<<"$formula_record")"
    source_hash="$(jq -r '.urls.stable.checksum // empty' <<<"$formula_record")"
    [[ -n "$source_url" && -n "$source_hash" ]] || { echo "missing source URL/hash for $formula $installed_version" >&2; exit 1; }
    license="$(jq -r '.license // "unspecified"' <<<"$formula_record")"
    printf '%s %s | license: %s | source: %s | source sha256: %s\n' \
      "$formula" "$installed_version" "$license" "$source_url" "$source_hash"

    formula_license_dir="$license_root/homebrew/$formula"
    mkdir -p "$formula_license_dir"
    copied=0
    while IFS= read -r -d '' license_file; do
      relative="${license_file#"$keg"/}"
      mkdir -p "$formula_license_dir/$(dirname -- "$relative")"
      cp -p "$license_file" "$formula_license_dir/$relative"
      copied=$((copied + 1))
    done < <(find "$keg" -type f \( \
      -iname 'LICENSE' -o -iname 'LICENSE.*' -o -iname 'COPYING' -o \
      -iname 'COPYING.*' -o -iname 'NOTICE' -o -iname 'NOTICE.*' -o \
      -iname 'COPYRIGHT*' -o -path '*/licenses/*' -o -path '*/LICENSES/*' \) -print0)
    if (( copied == 0 )); then
      case "$formula" in
        qtbase|qttools|qtwebengine|sqlite|mpv|libzip|md4c|ffmpeg)
          echo "no installed license/copyright/notice text found for core formula $formula at $keg" >&2
          exit 1
          ;;
      esac
    fi
  done < "$formula_list"
} > "$notices"

# Reject Homebrew load references and any non-system dependency that cannot be
# resolved within the staged app. macdeployqt does not collect every non-Qt lib.
resolve_rpath_dependency() {
  local suffix="$1" rpaths="$2" loader_dir="$3" executable_dir="$4" rpath candidate
  while IFS= read -r rpath; do
    [[ -n "$rpath" ]] || continue
    case "$rpath" in
      @loader_path/*) candidate="$loader_dir/${rpath#@loader_path/}/$suffix" ;;
      @executable_path/*) candidate="$executable_dir/${rpath#@executable_path/}/$suffix" ;;
      /*) candidate="$rpath/$suffix" ;;
      *) continue ;;
    esac
    if [[ -e "$candidate" ]]; then
      case "$candidate" in
        "$stage/melearner.app"/*|/System/Library/*|/usr/lib/*) return 0 ;;
      esac
    fi
  done <<<"$rpaths"
  return 1
}

while IFS= read -r -d '' binary; do
  if file -b "$binary" | grep -q 'Mach-O'; then
    load_paths="$(otool -L "$binary" | awk 'NR > 1 { sub(/^[[:space:]]+/, ""); sub(/[[:space:]]+\(compatibility version.*/, ""); print }')"
    rpaths="$(otool -l "$binary" | awk '$1 == "cmd" && $2 == "LC_RPATH" { nextline = 1; next } nextline && $1 == "path" { print $2; nextline = 0 }')"
    main_executable="$stage/melearner.app/Contents/MacOS/melearner"
    bundle_cursor="$(dirname -- "$binary")"
    while [[ "$bundle_cursor" == "$stage/melearner.app" || "$bundle_cursor" == "$stage/melearner.app/"* ]]; do
      if [[ "$bundle_cursor" == *.app && -f "$bundle_cursor/Contents/Info.plist" ]]; then
        executable_name="$(/usr/libexec/PlistBuddy -c 'Print :CFBundleExecutable' "$bundle_cursor/Contents/Info.plist" 2>/dev/null || true)"
        if [[ -n "$executable_name" && -x "$bundle_cursor/Contents/MacOS/$executable_name" ]]; then
          main_executable="$bundle_cursor/Contents/MacOS/$executable_name"
          break
        fi
      fi
      [[ "$bundle_cursor" == "$stage/melearner.app" ]] && break
      bundle_cursor="$(dirname -- "$bundle_cursor")"
    done
    main_rpaths="$(otool -l "$main_executable" | awk '$1 == "cmd" && $2 == "LC_RPATH" { nextline = 1; next } nextline && $1 == "path" { print $2; nextline = 0 }')"
    if grep -Eq '/(opt/homebrew|usr/local)/(Cellar|opt)/' <<<"$load_paths $rpaths"; then
      echo "Homebrew load path remains in packaged binary: $binary" >&2
      grep -E '/(opt/homebrew|usr/local)/(Cellar|opt)/' <<<"$load_paths $rpaths" >&2
      exit 1
    fi
    while IFS= read -r dependency; do
      [[ -n "$dependency" ]] || continue
      case "$dependency" in
        /System/Library/*|/usr/lib/*) continue ;;
        @loader_path/*)
          [[ -e "$(dirname -- "$binary")/${dependency#@loader_path/}" ]] && continue
          ;;
        @executable_path/*)
          [[ -e "$(dirname -- "$main_executable")/${dependency#@executable_path/}" ]] && continue
          ;;
        @rpath/*)
          suffix="${dependency#@rpath/}"
          if resolve_rpath_dependency "$suffix" "$rpaths" "$(dirname -- "$binary")" "$(dirname -- "$main_executable")" || \
             resolve_rpath_dependency "$suffix" "$main_rpaths" "$(dirname -- "$main_executable")" "$(dirname -- "$main_executable")"; then
            continue
          fi
          ;;
      esac
      echo "unresolved bundled dependency '$dependency' in $binary" >&2
      exit 1
    done <<<"$load_paths"
  fi
done < <(find "$stage/melearner.app" -type f -print0)
# macdeployqt signs before this script adds legal resources. Re-seal the root
# app ad hoc while preserving the deployment signature's entitlements/metadata.
codesign --force --sign - --preserve-metadata=entitlements,requirements,flags,runtime \
  --timestamp=none "$stage/melearner.app"
codesign --verify --deep --strict --verbose=2 "$stage/melearner.app"

ln -s /Applications "$stage/Applications"
cat > "$stage/README.txt" <<EOF
melearner ${version} for macOS (arm64)

Drag melearner.app to Applications to install.

This diagnostic build has an ad hoc signature only. It has no trusted Developer ID publisher signature, is not notarized, and is not release-qualified. Intel Macs are not supported by this arm64-only artifact.
EOF

image="$work_dir/melearner-${version}-macos-arm64.dmg"
hdiutil create -quiet -volname "melearner ${version}" -srcfolder "$stage" -ov -format UDZO "$image"
test -s "$image"
checksum="$(shasum -a 256 "$image" | awk '{print $1}')"
ln "$image" "$output"
printf '%s  %s\n' "$checksum" "$(basename -- "$output")" > "$work_dir/checksum"
ln "$work_dir/checksum" "${output}.sha256"

printf 'Created diagnostic macOS DMG: %s\n' "$output"
printf 'Architecture: arm64; ad hoc signed: true; trusted publisher signature: false; notarized: false; release-qualified: false\n'
