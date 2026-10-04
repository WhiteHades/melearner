#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
version="0.1.0"
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
for tool in macdeployqt hdiutil codesign otool install_name_tool file shasum jq brew lipo ditto cmake awk sort find cp curl tar git; do
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
# Homebrew splits Qt modules into separate prefixes. Plugins can load modules
# that the main executable never linked, so its original RPATHs are insufficient.
deploy_paths=()
brew_lib_paths=()
while IFS= read -r formula; do
  prefix="$(brew --prefix "$formula")"
  if [[ -d "$prefix/lib" ]]; then
    deploy_paths+=("-libpath=$prefix/lib")
    brew_lib_paths+=("$prefix/lib")
  fi
done < <(brew list --formula)
macdeployqt "$stage/melearner.app" -always-overwrite -codesign=- "${deploy_paths[@]}"

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
  brew deps --installed --union qtbase qttools qtwebengine sqlite mpv libzip md4c ffmpeg
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
    source_revision="$(jq -r '.urls.stable.revision // empty' <<<"$formula_record")"
    [[ -n "$source_url" ]] || { echo "missing source URL for $formula $installed_version" >&2; exit 1; }
    if [[ -n "$source_hash" ]]; then
      [[ "$source_hash" =~ ^[[:xdigit:]]{64}$ ]] || { echo "invalid SHA-256 source checksum for $formula $installed_version" >&2; exit 1; }
      source_kind="archive-sha256"
      source_identity="$(tr '[:upper:]' '[:lower:]' <<<"$source_hash" | tr -d '\n')"
    elif [[ "$source_revision" =~ ^[[:xdigit:]]{40}$ ]]; then
      source_kind="git-commit"
      source_identity="$(tr '[:upper:]' '[:lower:]' <<<"$source_revision" | tr -d '\n')"
    else
      echo "missing SHA-256 or immutable 40-character Git revision for $formula $installed_version" >&2
      exit 1
    fi
    if [[ "$formula" == ca-certificates && "$source_url" == *.pem && "$source_kind" == archive-sha256 ]]; then
      source_kind="pem-sha256"
    fi
    license="$(jq -r '.license // "unspecified"' <<<"$formula_record")"
    printf '%s %s | license: %s | source: %s | source %s: %s\n' \
      "$formula" "$installed_version" "$license" "$source_url" "$source_kind" "$source_identity"

    formula_license_dir="$license_root/homebrew/$formula"
    mkdir -p "$formula_license_dir"
    mkdir -p "$formula_license_dir/.brew"
    cp -p "$receipt" "$formula_license_dir/.brew/INSTALL_RECEIPT.json"
    if [[ -s "$keg/.brew/$formula.rb" ]]; then
      cp -p "$keg/.brew/$formula.rb" "$formula_license_dir/.brew/formula.rb"
    else
      brew cat "$formula" > "$formula_license_dir/.brew/formula.rb"
    fi
    copied=0
    while IFS= read -r -d '' license_file; do
      relative="${license_file#"$keg"/}"
      mkdir -p "$formula_license_dir/$(dirname -- "$relative")"
      cp -p "$license_file" "$formula_license_dir/$relative"
      copied=$((copied + 1))
    done < <(find "$keg" -type f \( \
      -iname 'LICENSE' -o -iname 'LICENSE.*' -o -iname 'LICENCE' -o -iname 'LICENCE.*' -o -iname 'COPYING' -o \
      -iname 'COPYING.*' -o -iname 'NOTICE' -o -iname 'NOTICE.*' -o \
      -iname 'COPYRIGHT*' -o -path '*/licenses/*' -o -path '*/LICENSES/*' -o \
      -path '*/licences/*' -o -path '*/LICENCES/*' \) -print0)
    if (( copied == 0 )) && [[ "$formula" == sqlite && -s "$keg/include/sqlite3.h" ]]; then
      # SQLite's original installed header carries its public domain statement.
      cp -p "$keg/include/sqlite3.h" "$formula_license_dir/sqlite3.h"
      copied=1
    fi
    if (( copied == 0 )); then
      # Some bottles omit their license tree. Retrieve the exact source from
      # matching formula metadata; checksummed archives and immutable Git
      # commits use separate, explicit verification paths.
      source_archive="$work_dir/$formula-$installed_version-source"
      source_tree="$work_dir/$formula-$installed_version-source-tree"
      source_manifest="$work_dir/$formula-$installed_version-source-manifest"
      source_entries="$work_dir/$formula-$installed_version-source-entries"
      source_error="$work_dir/$formula-$installed_version-source-error"
      mkdir -p "$source_tree"
      if [[ "$source_kind" == archive-sha256 || "$source_kind" == pem-sha256 ]]; then
        if ! curl --fail --location --silent --show-error "$source_url" -o "$source_archive"; then
          echo "source download failed for $formula $installed_version ($source_kind): $source_url" >&2
          exit 1
        fi
        actual_source_hash="$(shasum -a 256 "$source_archive" | awk '{print $1}')"
        [[ "$actual_source_hash" == "$source_identity" ]] || {
          echo "source checksum mismatch for $formula $installed_version: expected $source_identity, got $actual_source_hash" >&2
          exit 1
        }
        if [[ "$source_kind" == pem-sha256 ]]; then
          # curl's pinned CA bundle is a standalone PEM file, not a tarball.
          # Keep that exact, hash-verified source with its Mozilla attribution.
          if ! awk '/Certificate data from Mozilla/ { mozilla=1 } /-----BEGIN CERTIFICATE-----/ { certificate=1; exit } END { if (!mozilla || !certificate) exit 1 }' "$source_archive"; then
            echo "verified source for $formula $installed_version is not the expected Mozilla CA PEM: $source_url" >&2
            exit 1
          fi
          pem_name="$(basename -- "$source_url")"
          cp -p "$source_archive" "$formula_license_dir/$pem_name"
          copied=1
        else
          # Source archives such as QtWebEngine are large; extract only legal
          # paths while retaining the verified archive as provenance evidence.
          if ! tar -tf "$source_archive" > "$source_entries" 2> "$source_error"; then
            echo "expected a tar source archive for $formula $installed_version ($source_kind): $source_url" >&2
            sed 's/^/tar: /' "$source_error" >&2
            exit 1
          fi
          awk '
          { path=tolower($0); n=split(path, part, "/"); leaf=part[n]
            if (leaf ~ /^(licen[cs]e[s]?|copying|notice|copyright)([._-].*)?$/ || path ~ /(^|\/)licen[cs]e[s]?(\/|$)/) print $0
          }' "$source_entries" > "$source_manifest"
          [[ -s "$source_manifest" ]] || { echo "no license paths in verified source archive for $formula $installed_version ($source_url)" >&2; exit 1; }
          if ! tar -xf "$source_archive" -C "$source_tree" --strip-components=1 -T "$source_manifest" 2> "$source_error"; then
            echo "failed extracting legal files from verified source archive for $formula $installed_version: $source_url" >&2
            sed 's/^/tar: /' "$source_error" >&2
            exit 1
          fi
        fi
      else
        git init -q "$source_tree"
        git -C "$source_tree" remote add origin "$source_url"
        if ! git -C "$source_tree" fetch --quiet --depth 1 origin "$source_revision"; then
          echo "source fetch failed for $formula $installed_version at immutable commit $source_identity: $source_url" >&2
          exit 1
        fi
        git -C "$source_tree" checkout --quiet --detach FETCH_HEAD
        actual_source_revision="$(git -C "$source_tree" rev-parse HEAD)"
        [[ "$actual_source_revision" == "$source_identity" ]] || {
          echo "source revision mismatch for $formula $installed_version: expected $source_identity, got $actual_source_revision" >&2
          exit 1
        }
      fi
      while IFS= read -r -d '' license_file; do
        relative="${license_file#"$source_tree"/}"
        mkdir -p "$formula_license_dir/$(dirname -- "$relative")"
        cp -p "$license_file" "$formula_license_dir/$relative"
        copied=$((copied + 1))
      done < <(find "$source_tree" -type f \( \
        -iname 'LICENSE' -o -iname 'LICENSE.*' -o -iname 'LICENCE' -o -iname 'LICENCE.*' -o -iname 'COPYING' -o \
        -iname 'COPYING.*' -o -iname 'NOTICE' -o -iname 'NOTICE.*' -o \
        -iname 'COPYRIGHT*' -o -path '*/licenses/*' -o -path '*/LICENSES/*' -o \
        -path '*/licences/*' -o -path '*/LICENCES/*' \) -print0)
    fi
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

bundle_executable_for() {
  local binary="$1" cursor executable_name
  cursor="$(dirname -- "$binary")"
  while [[ "$cursor" == "$stage/melearner.app" || "$cursor" == "$stage/melearner.app/"* ]]; do
    if [[ "$cursor" == *.app && -f "$cursor/Contents/Info.plist" ]]; then
      executable_name="$(/usr/libexec/PlistBuddy -c 'Print :CFBundleExecutable' "$cursor/Contents/Info.plist" 2>/dev/null || true)"
      if [[ -n "$executable_name" && -x "$cursor/Contents/MacOS/$executable_name" ]]; then
        printf '%s\n' "$cursor/Contents/MacOS/$executable_name"
        return 0
      fi
    fi
    [[ "$cursor" == "$stage/melearner.app" ]] && break
    cursor="$(dirname -- "$cursor")"
  done
  printf '%s\n' "$stage/melearner.app/Contents/MacOS/melearner"
}

dependency_from_homebrew() {
  local dependency="$1" rpaths="$2" loader_dir="$3" executable_dir="$4" rpath candidate suffix
  brew_dependency_source=""
  brew_dependency_suffix=""
  case "$dependency" in
    @rpath/*) suffix="${dependency#@rpath/}" ;;
    /opt/homebrew/*|/usr/local/*)
      [[ -f "$dependency" ]] || return 1
      brew_dependency_source="$dependency"
      case "$dependency" in
        *.framework/*) brew_dependency_suffix="${dependency#*/lib/}" ;;
        *) brew_dependency_suffix="$(basename -- "$dependency")" ;;
      esac
      return 0
      ;;
    *) return 1 ;;
  esac
  while IFS= read -r rpath; do
    [[ -n "$rpath" ]] || continue
    case "$rpath" in
      @loader_path/*) candidate="$loader_dir/${rpath#@loader_path/}/$suffix" ;;
      @executable_path/*) candidate="$executable_dir/${rpath#@executable_path/}/$suffix" ;;
      /*) candidate="$rpath/$suffix" ;;
      *) continue ;;
    esac
    if [[ -f "$candidate" && "$candidate" != "$stage/melearner.app"/* ]]; then
      case "$candidate" in
        /opt/homebrew/*|/usr/local/*)
          brew_dependency_source="$candidate"
          brew_dependency_suffix="$suffix"
          return 0
          ;;
      esac
    fi
  done <<<"$rpaths"
  for lib_dir in "${brew_lib_paths[@]}"; do
    if [[ -f "$lib_dir/$suffix" ]]; then
      brew_dependency_source="$lib_dir/$suffix"
      brew_dependency_suffix="$suffix"
      return 0
    fi
  done
  return 1
}

framework_rpath_for() {
  local binary_dir="$1" framework_dir="$stage/melearner.app/Contents/Frameworks"
  awk -v from="$binary_dir" -v to="$framework_dir" 'BEGIN {
    n=split(from, a, "/"); m=split(to, b, "/"); i=1
    while (i<=n && i<=m && a[i]==b[i]) i++
    path=""
    for (j=i; j<=n; j++) if (a[j]!="") path=path "../"
    for (j=i; j<=m; j++) if (b[j]!="") path=path b[j] "/"
    sub(/\/$/, "", path)
    if (path=="") path="."
    print "@loader_path/" path
  }'
}

# macdeployqt can miss non-Qt transitive dependencies (and split Qt modules
# loaded only by plugins). Copy those exact Homebrew files into Frameworks,
# rewrite their install names, and give each image a relocatable Frameworks
# search path. Repeat because a copied dylib can itself have Homebrew loads.
while :; do
  copied_dependency=false
  while IFS= read -r -d '' binary; do
    file -b "$binary" | grep -q 'Mach-O' || continue
    main_executable="$(bundle_executable_for "$binary")"
    main_rpaths="$(otool -l "$main_executable" | awk '$1 == "cmd" && $2 == "LC_RPATH" { nextline = 1; next } nextline && $1 == "path" { print $2; nextline = 0 }')"
    rpaths="$(otool -l "$binary" | awk '$1 == "cmd" && $2 == "LC_RPATH" { nextline = 1; next } nextline && $1 == "path" { print $2; nextline = 0 }')"
    framework_rpath="$(framework_rpath_for "$(dirname -- "$binary")")"
    if ! grep -Fxq "$framework_rpath" <<<"$rpaths"; then
      install_name_tool -add_rpath "$framework_rpath" "$binary"
      copied_dependency=true
    fi
    while IFS= read -r dependency; do
      if [[ "$dependency" == @rpath/* ]]; then
        suffix="${dependency#@rpath/}"
        if resolve_rpath_dependency "$suffix" "$rpaths" "$(dirname -- "$binary")" "$(dirname -- "$main_executable")" || \
           resolve_rpath_dependency "$suffix" "$main_rpaths" "$(dirname -- "$main_executable")" "$(dirname -- "$main_executable")"; then
          continue
        fi
      fi
      search_rpaths="$rpaths
$main_rpaths"
      dependency_from_homebrew "$dependency" "$search_rpaths" "$(dirname -- "$binary")" "$(dirname -- "$main_executable")" || continue
      source="$brew_dependency_source"
      suffix="$brew_dependency_suffix"

      framework_path=""
      case "$suffix" in
        *.framework/*) framework_path="${suffix%%.framework/*}.framework" ;;
      esac
      if [[ -n "$framework_path" ]]; then
        framework_name="$(basename -- "$framework_path")"
        framework_source="${source%%.framework/*}.framework"
        framework_target="$stage/melearner.app/Contents/Frameworks/$framework_name"
        if [[ ! -e "$framework_target" ]]; then
          ditto "$framework_source" "$framework_target"
          copied_dependency=true
        fi
        target_suffix="$framework_name/${suffix#"${framework_path}"/}"
        install_name_tool -id "@rpath/$target_suffix" "$framework_target/${suffix#"${framework_path}"/}"
      else
        target_name="$(basename -- "$suffix")"
        target="$stage/melearner.app/Contents/Frameworks/$target_name"
        if [[ ! -e "$target" ]]; then
          cp -pL "$source" "$target"
          copied_dependency=true
        fi
        target_suffix="$target_name"
        install_name_tool -id "@rpath/$target_suffix" "$target"
      fi
      if [[ "$dependency" != "@rpath/$target_suffix" ]]; then
        install_name_tool -change "$dependency" "@rpath/$target_suffix" "$binary"
        copied_dependency=true
      fi
    done < <(otool -L "$binary" | awk 'NR > 1 { sub(/^[[:space:]]+/, ""); sub(/[[:space:]]+\(compatibility version.*/, ""); print }')
    while IFS= read -r rpath; do
      case "$rpath" in
        /opt/homebrew/*|/usr/local/*)
          install_name_tool -delete_rpath "$rpath" "$binary"
          copied_dependency=true
          ;;
      esac
    done <<<"$rpaths"
  done < <(find "$stage/melearner.app" -type f -print0)
  [[ "$copied_dependency" == true ]] || break
done

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
codesign --force --deep --sign - --preserve-metadata=entitlements,requirements,flags,runtime \
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
