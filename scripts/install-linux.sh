#!/usr/bin/env bash
set -euo pipefail

version=0.1.1
release_url="https://github.com/WhiteHades/melearner/releases/download/v${version}"
checksum_url="https://raw.githubusercontent.com/WhiteHades/melearner/main/packaging/checksums/v${version}"
appimage_asset="melearner_${version}_amd64.AppImage"
arch_asset="melearner-bin-${version}-1-x86_64.pkg.tar.zst"

fail() {
  printf 'install-linux: %s\n' "$*" >&2
  exit 1
}

usage() {
  cat <<'EOF'
Usage: bash scripts/install-linux.sh [--arch] [--help]

Install the v0.1.1 AppImage for the current user, or use --arch to install
the official Arch package with pacman.
EOF
}

mode=appimage
while (($#)); do
  case "$1" in
    --help|-h) usage; exit 0 ;;
    --arch)
      [[ "$mode" == appimage ]] || fail "--arch may only be specified once."
      mode=arch
      ;;
    *) fail "Unknown option: $1. Use --help for usage." ;;
  esac
  shift
done

[[ "$(uname -s)" == Linux ]] || fail "Linux is required."
[[ "$(uname -m)" == x86_64 ]] || fail "This release supports Linux x86_64 only."

glibc_version="$(getconf GNU_LIBC_VERSION 2>/dev/null || true)"
if [[ ! "$glibc_version" =~ ^glibc[[:space:]]+([0-9]+)\.([0-9]+)$ ]]; then
  fail "Could not determine the installed glibc version."
fi
glibc_major="${BASH_REMATCH[1]}"
glibc_minor="${BASH_REMATCH[2]}"
if ((10#$glibc_major < 2 || (10#$glibc_major == 2 && 10#$glibc_minor < 39))); then
  fail "glibc 2.39 or newer is required; found $glibc_version."
fi

if [[ "$mode" == appimage ]]; then
  [[ "$EUID" -ne 0 ]] || fail "Run the AppImage installer as a regular user, not root."
  [[ -n "${HOME:-}" && "$HOME" == /* ]] || fail "HOME must be set to an absolute path."
else
  command -v pacman >/dev/null 2>&1 || fail "--arch requires pacman."
  command -v sudo >/dev/null 2>&1 || fail "--arch requires sudo to run pacman."
fi
command -v curl >/dev/null 2>&1 || fail "curl is required."
command -v sha256sum >/dev/null 2>&1 || fail "sha256sum is required."

asset="$appimage_asset"
[[ "$mode" != arch ]] || asset="$arch_asset"
work_dir="$(mktemp -d "${TMPDIR:-/tmp}/melearner-install.XXXXXXXX")" || fail "Could not create a temporary directory."
if [[ ! -d "$work_dir" || -L "$work_dir" || "$work_dir" != "${TMPDIR:-/tmp}"/melearner-install.* ]]; then
  fail "mktemp returned an unexpected temporary directory."
fi
cleanup() {
  [[ -n "${work_dir:-}" && -d "$work_dir" && ! -L "$work_dir" && "$work_dir" == "${TMPDIR:-/tmp}"/melearner-install.* ]] && rm -rf -- "$work_dir"
}
trap cleanup EXIT

download() {
  local name="$1" destination="$2"
  curl --fail --location --proto '=https' --tlsv1.2 --retry 3 \
    --output "$destination" "$release_url/$name"
}

download "$asset" "$work_dir/$asset"
curl --fail --location --proto '=https' --tlsv1.2 --retry 3 \
  --output "$work_dir/$asset.sha256" "$checksum_url/$asset.sha256"

mapfile -t checksum_lines < "$work_dir/$asset.sha256"
asset_pattern="${asset//./\\.}"
if ((${#checksum_lines[@]} != 1)) || [[ ! "${checksum_lines[0]}" =~ ^([[:xdigit:]]{64})([[:space:]]+${asset_pattern})?$ ]]; then
  fail "The official checksum file for $asset has an invalid format."
fi
expected_hash="${BASH_REMATCH[1],,}"
actual_hash="$(sha256sum -- "$work_dir/$asset" | cut -d ' ' -f 1)"
[[ "$actual_hash" == "$expected_hash" ]] || fail "SHA-256 verification failed for $asset."

if [[ "$mode" == arch ]]; then
  sudo pacman -U "$work_dir/$asset"
  exit 0
fi

app_dir="$HOME/Applications/meLearner"
app_path="$app_dir/$appimage_asset"
launcher_dir="$HOME/.local/bin"
launcher_path="$launcher_dir/melearner"
data_root="${XDG_DATA_HOME:-$HOME/.local/share}"
desktop_dir="$data_root/applications"
desktop_path="$desktop_dir/io.github.whitehades.melearner.desktop"
backup_root="$HOME/.local/state/melearner/backups"
launcher_backup=false
desktop_backup=false

desktop_exec='"'
for ((i = 0; i < ${#launcher_path}; i++)); do
  char="${launcher_path:i:1}"
  case "$char" in
    '\') desktop_exec+='\\\\' ;;
    '"'|'$'|'`') desktop_exec+="\\\\$char" ;;
    '%') desktop_exec+='%%' ;;
    *) desktop_exec+="$char" ;;
  esac
done
desktop_exec+='"'

if [[ -e "$app_path" || -L "$app_path" ]]; then
  [[ -f "$app_path" && ! -L "$app_path" ]] || fail "Refusing to replace non-file install target: $app_path"
  existing_hash="$(sha256sum -- "$app_path" | cut -d ' ' -f 1)"
  [[ "$existing_hash" == "$expected_hash" || "$existing_hash" == 8428e78c80a287f021e5cc8852c7d2c2fdd0c3cc0975feafe61aedf410d54bd6 || "$existing_hash" == 458ede022af3a54fd44186a803ece682710e6b4c025666bb7f4b36d5ff9a3891 ]] || fail "Refusing to replace an unrecognized AppImage: $app_path"
fi

if [[ -e "$launcher_path" || -L "$launcher_path" ]]; then
  if [[ -L "$launcher_path" ]]; then
    launcher_target="$(readlink -- "$launcher_path")"
    if [[ "$launcher_target" != "$app_path" ]]; then
      previous_name="${launcher_target#"$app_dir/"}"
      [[ "$launcher_target" == "$app_dir/"* && "$previous_name" =~ ^melearner_[0-9]+\.[0-9]+\.[0-9]+_amd64\.AppImage$ && -f "$launcher_target" && ! -L "$launcher_target" ]] || fail "Refusing to replace an unrelated launcher link: $launcher_path"
      previous_hash="$(sha256sum -- "$launcher_target" | cut -d ' ' -f 1)"
      # Recognize the previously published official AppImage without trusting its filename.
      [[ "$previous_hash" == "$expected_hash" || "$previous_hash" == a7140578c6fa8f35514353bd09585595356f2ae64aecc12d602b11c868942bff || "$previous_hash" == 8428e78c80a287f021e5cc8852c7d2c2fdd0c3cc0975feafe61aedf410d54bd6 || "$previous_hash" == 458ede022af3a54fd44186a803ece682710e6b4c025666bb7f4b36d5ff9a3891 ]] || fail "Refusing to replace an unrecognized launcher target: $launcher_target"
    fi
  elif [[ -f "$launcher_path" ]] && grep -Fqx '# Managed by scripts/install-linux.sh for meLearner.' "$launcher_path"; then
    :
  else
    fail "Refusing to replace an unrelated launcher: $launcher_path"
  fi
  launcher_backup=true
fi
if [[ -e "$desktop_path" || -L "$desktop_path" ]]; then
  [[ -f "$desktop_path" && ! -L "$desktop_path" ]] || fail "Refusing to replace non-file install target: $desktop_path"
  if grep -Fqx '# Managed by scripts/install-linux.sh for meLearner.' "$desktop_path" || \
     { grep -Fqx 'Name=meLearner' "$desktop_path" && \
       { grep -Fqx "Exec=$desktop_exec" "$desktop_path" || grep -Fqx "Exec=$launcher_path" "$desktop_path" || \
         grep -Fqx "Exec=$desktop_exec %F" "$desktop_path" || grep -Fqx "Exec=$launcher_path %F" "$desktop_path"; }; }; then
    desktop_backup=true
  else
    fail "Refusing to replace an unrelated desktop entry: $desktop_path"
  fi
fi

mkdir -p -- "$app_dir" "$launcher_dir" "$desktop_dir"
app_stage=
launcher_stage=
desktop_stage=
cleanup_stages() {
  for stage in "$app_stage" "$launcher_stage" "$desktop_stage"; do
    [[ -z "$stage" ]] || rm -f -- "$stage"
  done
}
trap 'cleanup_stages; cleanup' EXIT
app_stage="$(mktemp "$app_dir/.melearner-appimage.XXXXXXXX")"
launcher_stage="$(mktemp "$launcher_dir/.melearner-launcher.XXXXXXXX")"
desktop_stage="$(mktemp "$desktop_dir/.melearner-desktop.XXXXXXXX")"
install -m 755 -- "$work_dir/$asset" "$app_stage"

appimage_shell="$(printf '%q' "$app_path")"
cat >"$launcher_stage" <<EOF
#!/usr/bin/env bash
# Managed by scripts/install-linux.sh for meLearner.
if [[ -r /dev/fuse && -w /dev/fuse ]] && { command -v fusermount3 >/dev/null 2>&1 || command -v fusermount >/dev/null 2>&1; }; then
  exec $appimage_shell "\$@"
fi
exec $appimage_shell --appimage-extract-and-run "\$@"
EOF
chmod 755 "$launcher_stage"

if [[ "$desktop_backup" == true ]]; then
  cp -- "$desktop_path" "$desktop_stage"
else
  cat >"$desktop_stage" <<EOF
[Desktop Entry]
# Managed by scripts/install-linux.sh for meLearner.
Type=Application
Name=meLearner
Exec=$desktop_exec
Terminal=false
Categories=Education;
EOF
fi
chmod 644 "$desktop_stage"

if [[ "$launcher_backup" == true || "$desktop_backup" == true ]]; then
  mkdir -p -- "$backup_root"
  backup_dir="$backup_root/$(date +%Y%m%dT%H%M%S).$$"
  mkdir -- "$backup_dir"
  [[ "$launcher_backup" != true ]] || cp -a -- "$launcher_path" "$backup_dir/melearner-launcher"
  [[ "$desktop_backup" != true ]] || cp -a -- "$desktop_path" "$backup_dir/io.github.whitehades.melearner.desktop"
fi

mv -f -- "$app_stage" "$app_path"
mv -f -- "$launcher_stage" "$launcher_path"
mv -f -- "$desktop_stage" "$desktop_path"
printf 'Installed meLearner to %s\n' "$app_path"
