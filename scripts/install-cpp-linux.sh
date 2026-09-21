#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo_root"

if [[ "$(uname -s)" != Linux ]]; then
  echo "This installer supports Linux only. macOS and Windows packages are not qualified yet." >&2
  exit 1
fi
for tool in cmake ninja c++ pkg-config; do
  if ! command -v "$tool" >/dev/null; then
    echo "Missing build tool: $tool. See cpp-app/README.md for development prerequisites." >&2
    exit 1
  fi
done
if ! pkg-config --exists Qt6Widgets Qt6OpenGLWidgets Qt6Network Qt6Pdf sqlite3 mpv libzip md4c; then
  echo "Missing native development libraries. See cpp-app/README.md." >&2
  exit 1
fi

install_prefix="${1:-$HOME/.local}"
if [[ "$install_prefix" != /* ]]; then
  echo "The installation prefix must be an absolute path." >&2
  exit 1
fi
cmake --preset linux-release
cmake --build --preset linux-release --parallel 4
ctest --preset linux-release
cmake --install build/cpp-release --prefix "$install_prefix"
if command -v update-desktop-database >/dev/null; then
  update-desktop-database "$install_prefix/share/applications"
fi
printf 'Installed melearner. Start it from your application menu or run %s/bin/melearner\n' "$install_prefix"
