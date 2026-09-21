#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
version="0.1.0"
build_dir="${repo_root}/build/cpp-release"
legal_root="${repo_root}/packaging"
output="${repo_root}/dist/melearner-${version}-linux-x86_64.tar.zst"

usage() {
  cat <<'EOF'
usage: scripts/package-cpp-linux.sh [options]

Create the diagnostic C++ Linux runtime archive. The archive is not an
AppImage or Arch package and is never marked release-qualified.

Options:
  --build-dir <path>  configured CMake release build (default: build/cpp-release)
  --legal-root <path> legal inputs (default: packaging)
  --output <path>     output archive (default: dist/melearner-0.1.0-linux-x86_64.tar.zst)
  -h, --help          show this help
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --build-dir)
      [[ $# -ge 2 && -n "$2" && "$2" != --* ]] || { usage >&2; exit 2; }
      build_dir="$2"
      shift 2
      ;;
    --legal-root)
      [[ $# -ge 2 && -n "$2" && "$2" != --* ]] || { usage >&2; exit 2; }
      legal_root="$2"
      shift 2
      ;;
    --output)
      [[ $# -ge 2 && -n "$2" && "$2" != --* ]] || { usage >&2; exit 2; }
      output="$2"
      shift 2
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      usage >&2
      exit 2
      ;;
  esac
done

if [[ "$(uname -s)" != Linux ]]; then
  echo "C++ Linux packaging is only supported on Linux" >&2
  exit 1
fi

if [[ -n "${DESTDIR:-}" ]]; then
  echo "DESTDIR must be empty; the packager manages its own staging directory" >&2
  exit 1
fi

for tool in cmake tar zstd file readelf patchelf awk grep mktemp ln; do
  if ! command -v "$tool" >/dev/null 2>&1; then
    echo "required packaging tool is missing: $tool" >&2
    exit 1
  fi
done
# Read the producer completely. grep -q in a pipe can cause SIGPIPE under pipefail.
tar_help="$(tar --help)"
if [[ "$tar_help" != *--zstd* ]]; then
  echo "tar does not support --zstd" >&2
  exit 1
fi

if [[ "$build_dir" != /* ]]; then
  build_dir="$repo_root/$build_dir"
fi
if [[ "$legal_root" != /* ]]; then
  legal_root="$repo_root/$legal_root"
fi
if [[ "$output" != /* ]]; then
  output="$repo_root/$output"
fi
if [[ ! -d "$build_dir" || ! -f "$build_dir/CMakeCache.txt" ]]; then
  echo "configured CMake build directory is missing: $build_dir" >&2
  exit 1
fi
if [[ ! -d "$legal_root" ]]; then
  echo "legal input directory is missing: $legal_root" >&2
  exit 1
fi

# -LA omits STATIC cache entries, including CMAKE_PROJECT_VERSION. Do not source
# the cache as shell code. Multiple matching entries also fail the exact check.
project_version="$(awk -F= '$1 == "CMAKE_PROJECT_VERSION:STATIC" { print $2 }' "$build_dir/CMakeCache.txt")"
if [[ "$project_version" != "$version" ]]; then
  echo "CMake build version must be $version, got ${project_version:-missing}; reconfigure the release build" >&2
  exit 1
fi

expected_name="melearner-${version}-linux-x86_64.tar.zst"
if [[ "$(basename -- "$output")" != "$expected_name" ]]; then
  echo "output filename must be $expected_name" >&2
  exit 1
fi
if [[ -e "$output" || -L "$output" ]]; then
  echo "refusing to overwrite output: $output" >&2
  exit 1
fi
output_dir="$(dirname -- "$output")"

# Stage on the output filesystem so publication can use an atomic, no-clobber
# hard link. A preflight existence check followed by mv can overwrite a racer.
mkdir -p -- "$output_dir"
work_dir="$(mktemp -d "$output_dir/.melearner-package.XXXXXX")"
cleanup() {
  rm -rf -- "$work_dir"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
trap 'exit 129' HUP

stage_dir="$work_dir/melearner-${version}"
cmake \
  -DMELEARNER_SOURCE_DIR="$repo_root" \
  -DMELEARNER_BUILD_DIR="$build_dir" \
  -DMELEARNER_STAGE_DIR="$stage_dir" \
  -DMELEARNER_LEGAL_ROOT="$legal_root" \
  -DMELEARNER_VERSION="$version" \
  -P "$repo_root/scripts/stage-cpp-linux.cmake"

archive_tmp="$work_dir/${expected_name}.partial"
tar \
  --create \
  --zstd \
  --file "$archive_tmp" \
  --directory "$work_dir" \
  --numeric-owner \
  --owner=0 \
  --group=0 \
  --sort=name \
  --mtime='UTC 1970-01-01' \
  "melearner-${version}"
test -s "$archive_tmp"
# A corrupt/truncated listing must fail even if it already mentioned the binary.
# Save the full listing once rather than making tar race two early-exiting greps.
archive_list="$work_dir/archive.list"
if ! tar --zstd --list --file "$archive_tmp" >"$archive_list"; then
  echo "cannot read the complete archive listing" >&2
  exit 1
fi
if ! grep -Fxq "melearner-${version}/usr/bin/melearner" "$archive_list"; then
  echo "archive is missing the melearner executable" >&2
  exit 1
fi
if grep -Eq '(^/|(^|/)\.\.(/|$))' "$archive_list"; then
  echo "archive contains an unsafe path" >&2
  exit 1
fi
if ! ln -T -- "$archive_tmp" "$output"; then
  echo "could not publish archive without overwriting output: $output" >&2
  exit 1
fi

printf 'Created diagnostic C++ Linux archive: %s\n' "$output"
printf 'Release-qualified: false; AppImage and Arch acceptance remain separate gates.\n'
