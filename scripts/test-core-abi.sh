#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
manifest="$repo_root/crates/melearner-core/Cargo.toml"
target_dir="$repo_root/crates/melearner-core/target"

cargo build --locked --manifest-path "$manifest"
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
  -lmelearner_core \
  -lc \
  -lgcc_s \
  -lutil \
  -lrt \
  -lpthread \
  -lm \
  -ldl
