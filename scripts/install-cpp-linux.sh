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

for tool in cmake ctest ninja c++ cc meson patch pkg-config; do
  if ! command -v "$tool" >/dev/null; then
    fail "Missing build tool: $tool. See docs/development.md for development prerequisites."
  fi
done
if ! pkg-config --print-errors --exists \
  Qt6Widgets Qt6OpenGLWidgets Qt6Network Qt6Pdf Qt6Test sqlite3 libzip md4c \
  libavcodec libavdevice libavfilter libavformat libavutil libswresample libswscale \
  libass libplacebo alsa libpipewire-0.3 libpulse \
  egl gl libdrm gbm x11 xext xpresent xrandr xscrnsaver \
  wayland-client wayland-cursor wayland-egl wayland-protocols xkbcommon \
  libva libva-drm libva-x11 libva-wayland vdpau ffnvcodec; then
  fail "Missing native development libraries (including Qt Test and libmpv build dependencies). See docs/development.md."
fi

# Match mpv 0.41's Lua dependency candidates and version bounds. The bundled
# runtime enables Lua, so absence of every supported provider must fail before
# starting the longer configure/build pipeline.
lua_provider_found=false
for lua_candidate in \
  "lua:5.1.0:5.3.0" \
  "lua52:5.2.0:" "lua5.2:5.2.0:" "lua-5.2:5.2.0:" \
  "luajit:2.0.0:" \
  "lua51:5.1.0:" "lua5.1:5.1.0:" "lua-5.1:5.1.0:"; do
  IFS=: read -r lua_module lua_minimum lua_exclusive_maximum <<<"$lua_candidate"
  if pkg-config --atleast-version="$lua_minimum" "$lua_module" && \
     { [[ -z "$lua_exclusive_maximum" ]] ||
       ! pkg-config --atleast-version="$lua_exclusive_maximum" "$lua_module"; }; then
    lua_provider_found=true
    break
  fi
done
if [[ "$lua_provider_found" != true ]]; then
  fail "Missing Lua development dependency required by the bundled libmpv (supported: Lua 5.1/5.2 or LuaJIT). See docs/install.md."
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
