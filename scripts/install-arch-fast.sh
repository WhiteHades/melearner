#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo_root"

if command -v sccache >/dev/null 2>&1; then
  export RUSTC_WRAPPER="${RUSTC_WRAPPER:-sccache}"
  export CARGO_INCREMENTAL="${CARGO_INCREMENTAL:-0}"
else
  export CARGO_INCREMENTAL="${CARGO_INCREMENTAL:-1}"
fi

pnpm build
cargo build --manifest-path src-tauri/Cargo.toml --release

(
  cd packaging/arch
  makepkg -f --noconfirm
)

package_path="$(find packaging/arch -maxdepth 1 -type f -name 'melearner-bin-*.pkg.tar.*' -printf '%T@ %p\n' | sort -nr | awk 'NR == 1 {print $2}')"
if [[ -z "$package_path" ]]; then
  echo "no Arch package was produced" >&2
  exit 1
fi

sudo pacman -U --noconfirm "$package_path"
