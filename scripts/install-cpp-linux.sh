#!/usr/bin/env bash
set -euo pipefail

fail() {
  printf '%s\n' "$*" >&2
  exit 1
}

if (( $# > 1 )); then
  fail "Usage: bash scripts/install-cpp-linux.sh [absolute-install-prefix]"
fi
if [[ "${1-}" == --help || "${1-}" == -h ]]; then
  printf 'Usage: bash scripts/install-cpp-linux.sh [absolute-install-prefix]\nDefault prefix: $HOME/.local\n'
  exit 0
fi
if (( $# == 1 )); then
  install_prefix="$1"
elif [[ -n "${HOME:-}" ]]; then
  install_prefix="$HOME/.local"
else
  fail "HOME is unset or empty. Supply an absolute installation prefix."
fi
if [[ "$install_prefix" != /* ]]; then
  fail "The installation prefix must be an absolute path."
fi
if [[ -n "${DESTDIR:-}" ]]; then
  fail "Unset DESTDIR before using this source installer; it installs directly into the requested prefix."
fi
if [[ "$(uname -s)" != Linux ]]; then
  fail "This installer supports Linux only. macOS and Windows packages are not qualified yet."
fi

repo_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo_root"

for tool in cmake ctest ninja c++ pkg-config; do
  if ! command -v "$tool" >/dev/null; then
    fail "Missing build tool: $tool. See cpp-app/README.md for development prerequisites."
  fi
done
if ! pkg-config --print-errors --exists Qt6Widgets Qt6OpenGLWidgets Qt6Network Qt6Pdf Qt6Test sqlite3 mpv libzip md4c; then
  fail "Missing native development libraries (including Qt Test). See cpp-app/README.md."
fi

cmake --preset linux-release
cmake --build --preset linux-release --parallel 4
# An empty test discovery must never authorize installation.
ctest --preset linux-release --no-tests=error
cmake --install build/cpp-release --prefix "$install_prefix"
if [[ ! -x "$install_prefix/bin/melearner" ]]; then
  fail "Installation did not produce an executable at $install_prefix/bin/melearner."
fi
if command -v update-desktop-database >/dev/null; then
  update-desktop-database "$install_prefix/share/applications"
fi
printf 'Installed melearner. Run: %q\n' "$install_prefix/bin/melearner"
