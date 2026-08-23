#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
manifest="$repo_root/crates/melearner-core/Cargo.toml"
target_dir="$repo_root/crates/melearner-core/target"
mkdir -p "$repo_root/.tmp"
export TMPDIR="$repo_root/.tmp"
c23_tmp="$(mktemp -d "$repo_root/.tmp/core-abi-c23.XXXXXX")"
cleanup() {
  rm -rf "$c23_tmp"
  rmdir "$repo_root/.tmp" 2>/dev/null || true
}
trap cleanup EXIT

cargo build --locked --manifest-path "$manifest"
mpv_lib_dir="$(pkg-config --variable=libdir mpv)"
if [[ -z "$mpv_lib_dir" || ! -d "$mpv_lib_dir" ]]; then
  echo "pkg-config did not return a usable libmpv library directory" >&2
  exit 1
fi
if nm -g --defined-only "$target_dir/debug/libmelearner_core.a" \
  | awk '{print $3}' \
  | grep -Eq '^ml_core_test_'; then
  echo "release ABI exports test hooks" >&2
  exit 1
fi
zig run \
  -target x86_64-linux-gnu \
  -I "$repo_root/include" \
  "$repo_root/tests/core_abi_smoke.zig" \
  -L "$target_dir/debug" \
  -L "$mpv_lib_dir" \
  -lmelearner_core \
  -lmpv \
  -lc \
  -lgcc_s \
  -lutil \
  -lrt \
  -lpthread \
  -lm \
  -ldl

zig cc \
  -target x86_64-linux-gnu \
  -std=c23 \
  -pedantic-errors \
  -Wall \
  -Wextra \
  -Werror \
  -I "$repo_root/include" \
  "$repo_root/tests/core_abi_smoke.c" \
  -L "$target_dir/debug" \
  -L "$mpv_lib_dir" \
  -lmelearner_core \
  -lmpv \
  -lgcc_s \
  -lutil \
  -lrt \
  -lpthread \
  -lm \
  -ldl \
  -o "$c23_tmp/core_abi_smoke"

mkdir "$c23_tmp/state"
"$c23_tmp/core_abi_smoke" "$c23_tmp/state"
