#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="${repo_root}/build/linux"
output_dir="${repo_root}/.tmp/manual-ci"

usage() {
  printf 'usage: %s [--build-dir path] [--output-dir path]\n' "$0"
}
while [[ $# -gt 0 ]]; do
  case "$1" in
    --build-dir|--output-dir)
      [[ $# -ge 2 && -n "$2" && "$2" != --* ]] || { usage >&2; exit 2; }
      if [[ "$1" == --build-dir ]]; then build_dir="$2"; else output_dir="$2"; fi
      shift 2
      ;;
    -h|--help) usage; exit 0 ;;
    *) usage >&2; exit 2 ;;
  esac
done
[[ "$build_dir" == /* ]] || build_dir="$repo_root/$build_dir"
[[ "$output_dir" == /* ]] || output_dir="$repo_root/$output_dir"
legal_dir="$output_dir/legal"
evidence_dir="$output_dir/notice-evidence"
mkdir -p "$legal_dir" "$evidence_dir"
packages_jsonl="$output_dir/packages.jsonl"
evidence_jsonl="$output_dir/evidence.jsonl"
missing_jsonl="$output_dir/missing.jsonl"
: > "$packages_jsonl"
: > "$evidence_jsonl"
: > "$missing_jsonl"

add_package() {
  local name="$1" version="$2" provider="$3" source_url="$4" source_hash="$5" bottle_hash="${6:-}"
  jq -cn --arg name "$name" --arg version "$version" --arg provider "$provider" \
    --arg sourceURL "$source_url" --arg sourceHash "$source_hash" --arg bottleHash "$bottle_hash" \
    '{name:$name,version:$version,provider:$provider,sourceURL:(if $sourceURL=="" then null else $sourceURL end),sourceHash:(if $sourceHash=="" then null else $sourceHash end),bottleHash:(if $bottleHash=="" then null else $bottleHash end),licenseDeclared:"NOASSERTION",licenseConcluded:"NOASSERTION",copyrightText:"NOASSERTION"}' >> "$packages_jsonl"
}

collect_file() {
  local package="$1" source="$2" label="$3" dest relative hash
  [[ -f "$source" && -s "$source" ]] || return 1
  relative="${source#/}"
  dest="$evidence_dir/${package//[^A-Za-z0-9_.+-]/_}/${relative//\//_}"
  mkdir -p "$(dirname "$dest")"
  cp -- "$source" "$dest"
  hash="$(sha256sum "$dest" | awk '{print $1}')"
  jq -cn --arg package "$package" --arg label "$label" --arg path "${dest#"$output_dir"/}" --arg sha256 "$hash" \
    '{package:$package,label:$label,path:$path,sha256:$sha256}' >> "$evidence_jsonl"
  cat -- "$source" >> "$legal_dir/THIRD_PARTY_NOTICES"
  printf '\n\n--- %s: %s ---\n\n' "$package" "$source" >> "$legal_dir/THIRD_PARTY_NOTICES"
  return 0
}

: > "$legal_dir/THIRD_PARTY_NOTICES"
cp -- "$repo_root/LICENSE" "$legal_dir/LICENSE"

# Homebrew's installed formula versions and bottle metadata are queried from
# the runner; package evidence is copied from each installed keg's license tree.
brew --prefix >/dev/null
while IFS= read -r formula; do
  [[ -n "$formula" ]] || continue
  version="$(brew list --versions "$formula" | awk '{$1=""; sub(/^ /, ""); print}')"
  info="$(brew info --json=v2 "$formula")"
  prefix="$(cd -- "$(brew --prefix "$formula")" && pwd -P)"
  receipt="$prefix/INSTALL_RECEIPT.json"
  source_url=""
  source_hash=""
  bottle_hash=""
  if [[ -s "$receipt" ]]; then
    installed_version="$(jq -r '.source.versions.stable // empty' "$receipt")"
    stable_version="$(jq -r '.formulae[0].versions.stable // empty' <<<"$info")"
    if [[ -n "$installed_version" && "$installed_version" == "$stable_version" ]]; then
      source_url="$(jq -r '.formulae[0].urls.stable.url // empty' <<<"$info")"
      source_hash="$(jq -r '.formulae[0].urls.stable.checksum // empty' <<<"$info")"
      if [[ "$(jq -r '.poured_from_bottle // false' "$receipt")" == true ]]; then
        bottle_hash="$(jq -r '.formulae[0].bottle.stable.files.x86_64_linux.sha256 // empty' <<<"$info")"
      fi
    fi
  fi
  add_package "$formula" "$version" homebrew "$source_url" "$source_hash" "$bottle_hash"
  found=false
  while IFS= read -r -d '' file; do
    case "${file,,}" in
      */license|*/license.*|*/copying|*/copying.*|*/notice|*/notice.*|*/copyright|*/copyright.*|*/licenses/*|*/share/doc/*/copyright)
        if collect_file "$formula" "$file" installed-license; then found=true; fi
        ;;
    esac
  done < <(find "$prefix" \( -type f -o -type l \) -print0 2>/dev/null)
  if [[ "$found" != true ]]; then jq -cn --arg package "$formula" '{package:$package,reason:"No non-empty installed license/copyright/NOTICE file found under Homebrew prefix"}' >> "$missing_jsonl"; fi
done < <(brew list --formula)

# Capture only explicit apt build dependencies and dpkg providers for shared
# libraries in the built application's resolved ELF dependency closure.
declare -A dpkg_packages=()
add_dpkg_package() {
  local package="$1" version
  [[ -n "$package" ]] || return 0
  version="$(dpkg-query -W -f='${Version}' "$package" 2>/dev/null || true)"
  [[ -n "$version" ]] && dpkg_packages["$package"]="$version"
}
for package in libffmpeg-nvenc-dev libdisplay-info-dev; do
  add_dpkg_package "$package"
done
if [[ -x "$build_dir/melearner" ]]; then
  while IFS= read -r library; do
    [[ -n "$library" && -e "$library" ]] || continue
    while IFS= read -r owner; do add_dpkg_package "$owner"; done < <(
      dpkg-query -S "$library" 2>/dev/null | sed 's/: \/.*$//'
    )
  done < <(ldd "$build_dir/melearner" 2>/dev/null | awk '/=> \// {print $3} /^[[:space:]]*\// {print $1}')
fi
for package in "${!dpkg_packages[@]}"; do
  version="${dpkg_packages[$package]}"
  [[ -n "$package" ]] || continue
  add_package "$package" "$version" dpkg "" ""
  found=false
  while IFS= read -r file; do
    [[ "$file" == */copyright && -s "$file" ]] || continue
    if collect_file "$package" "$file" installed-copyright; then found=true; fi
  done < <(dpkg-query -L "$package" 2>/dev/null || true)
  if [[ "$found" != true ]]; then jq -cn --arg package "$package" '{package:$package,reason:"No installed dpkg copyright file found"}' >> "$missing_jsonl"; fi
done

# CMake-built dependencies use pinned upstream inputs. Hashes are the archive
# hashes verified by CMake; installed source-tree notices are copied verbatim.
add_package lexbor 3.0.0 cmake-fetchcontent https://github.com/lexbor/lexbor/archive/refs/tags/v3.0.0.tar.gz eafaa79ef9871f0bbb1978eda8677d184f7ecdcaa203d7cd25b3f86e32c014c2
add_package shadcn-cpp 5cc52a0edfc27a70c1d4e4b8e5586f21fba4860d cmake-fetchcontent https://codeload.github.com/WhiteHades/shadcn-cpp/tar.gz/5cc52a0edfc27a70c1d4e4b8e5586f21fba4860d a9e2555ee036ef42c02156dc04d5d2e837e53b9fad5f2c23523da8a0f4549992
add_package mpv 0.41.0 cmake-externalproject https://codeload.github.com/mpv-player/mpv/tar.gz/refs/tags/v0.41.0 ee21092a5ee427353392360929dc64645c54479aefdb5babc5cfbb5fad626209

for spec in \
  "lexbor:$build_dir/_deps/lexbor-src:LICENSE NOTICE" \
  "shadcn-cpp:$build_dir/_deps/shadcn_cpp-src:LICENSE LICENSES/shadcn-MIT.txt LICENSES/ui-components-MIT.txt LICENSES/Geist-OFL.txt" \
  "mpv:$build_dir/_deps/mpv/source:Copyright LICENSE.GPL LICENSE.LGPL"; do
  package="${spec%%:*}"; rest="${spec#*:}"; source_dir="${rest%%:*}"; files="${rest#*:}"
  for relative in $files; do
    if ! collect_file "$package" "$source_dir/$relative" upstream-notice; then
      jq -cn --arg package "$package" --arg path "$source_dir/$relative" '{package:$package,path:$path,reason:"Expected source notice file missing or empty"}' >> "$missing_jsonl"
    fi
  done
done

jq -s '{packages:.}' "$packages_jsonl" > "$legal_dir/runtime-lock.json"
namespace_suffix="$(date -u +%Y%m%dT%H%M%SZ)-${GITHUB_RUN_ID:-local}-$$"
jq -s --arg namespace "https://github.com/WhiteHades/melearner/spdx/linux/$namespace_suffix" \
  --arg created "$(date -u +%Y-%m-%dT%H:%M:%SZ)" \
  '{spdxVersion:"SPDX-2.3",dataLicense:"CC0-1.0",SPDXID:"SPDXRef-DOCUMENT",name:"melearner Linux diagnostic runtime evidence",documentNamespace:$namespace,creationInfo:{created:$created,creators:["Tool: collect-cpp-linux-notices.sh"]},packages:map(( {name:.name,SPDXID:("SPDXRef-" + (.name|gsub("[^A-Za-z0-9.-]";"-"))),versionInfo:.version,downloadLocation:(.sourceURL // "NOASSERTION"),filesAnalyzed:false,licenseConcluded:"NOASSERTION",licenseDeclared:"NOASSERTION",copyrightText:"NOASSERTION"} + (if .sourceHash then {checksums:[{algorithm:"SHA256",checksumValue:.sourceHash}]} else {} end) ))}' \
  "$packages_jsonl" > "$legal_dir/melearner.spdx.json"
jq -n --slurpfile packages "$packages_jsonl" --slurpfile evidence "$evidence_jsonl" --slurpfile missing "$missing_jsonl" \
  '{schemaVersion:1,releaseQualified:false,evidenceStatus:"observed-with-gaps",packages:$packages,noticeFiles:$evidence,missingEvidence:$missing,legalApproval:"not-provided"}' \
  > "$legal_dir/reference-profiles-v1.json"

printf 'Collected %s package records, %s notice files, %s evidence gaps. Release-qualified: false.\n' \
  "$(jq '.packages | length' "$legal_dir/runtime-lock.json")" \
  "$(jq '.noticeFiles | length' "$legal_dir/reference-profiles-v1.json")" \
  "$(wc -l < "$missing_jsonl")"
