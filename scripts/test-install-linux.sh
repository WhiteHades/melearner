#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
command -v bwrap >/dev/null || { printf 'bwrap is required for isolated installer tests.\n' >&2; exit 1; }

test_root="$repo_root/.tmp/installer-e2e"
mkdir -p -- "$test_root"
chmod 700 "$test_root"
run_root="$(mktemp -d "$test_root/run.XXXXXXXX")"
trap 'rm -rf -- "$run_root"' EXIT

fixture="$run_root/root with spaces"
sandbox_fixture="/tmp/repo/.tmp/installer-e2e/${run_root##*/}/root with spaces"
mkdir -p "$fixture/home" "$fixture/bin" "$fixture/tmp"
chmod 700 "$fixture/home" "$fixture/tmp"

payload='#!/usr/bin/env bash
printf "%s\\n" "$@" >"$INSTALLER_TEST_ARGS"
'
printf '%s\n' "$payload" >"$fixture/payload"
expected_hash="$(sha256sum "$fixture/payload" | cut -d ' ' -f 1)"

cat >"$fixture/bin/curl" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
out=
url=
strict_https=0
tls12=0
while (($#)); do
  case "$1" in
    --output) out=$2; shift 2 ;;
    --proto) [[ ${2-} == '=https' ]] || exit 22; strict_https=1; shift 2 ;;
    --tlsv1.2) tls12=1; shift ;;
    https://*) url=$1; shift ;;
    *) shift ;;
  esac
done
[[ "$url" == https://github.com/WhiteHades/melearner/releases/download/v0.1.1/* || \
   "$url" == https://raw.githubusercontent.com/WhiteHades/melearner/main/packaging/checksums/v0.1.1/*.sha256 ]] || exit 22
[[ "$strict_https" == 1 && "$tls12" == 1 ]] || exit 22
printf '%s\n' "$url" >>"$INSTALLER_TEST_URLS"
case "$url" in
  https://raw.githubusercontent.com/WhiteHades/melearner/main/packaging/checksums/v0.1.1/*.sha256)
    if [[ ${INSTALLER_TEST_MALFORMED_CHECKSUM:-} == 1 ]]; then
      printf 'not-a-valid-checksum\n' >"$out"
    elif [[ ${INSTALLER_TEST_MALFORMED_CHECKSUM_NAME:-} == 1 ]]; then
      printf '%s  melearner_0X1Y0_amd64ZAppImage\n' "$INSTALLER_TEST_HASH" >"$out"
    else
      asset_name="${url##*/}"
      asset_name="${asset_name%.sha256}"
      printf '%s  %s\n' "$INSTALLER_TEST_HASH" "$asset_name" >"$out"
    fi
    ;;
  *) cp "$INSTALLER_TEST_PAYLOAD" "$out" ;;
esac
EOF
cat >"$fixture/bin/getconf" <<'EOF'
#!/usr/bin/env bash
[[ ${1-} == GNU_LIBC_VERSION ]] || exit 1
printf 'glibc %s\n' "${INSTALLER_TEST_GLIBC:-2.39}"
EOF
cat >"$fixture/bin/uname" <<'EOF'
#!/usr/bin/env bash
case ${1-} in -s) printf 'Linux\n' ;; -m) printf '%s\n' "${INSTALLER_TEST_MACHINE:-x86_64}" ;; *) exit 1 ;; esac
EOF
cat >"$fixture/bin/sudo" <<'EOF'
#!/usr/bin/env bash
printf '%s\n' "$*" >>"$INSTALLER_TEST_SUDO_LOG"
exec "$@"
EOF
cat >"$fixture/bin/pacman" <<'EOF'
#!/usr/bin/env bash
printf '%s\n' "$*" >>"$INSTALLER_TEST_PACMAN_LOG"
EOF
cat >"$fixture/bin/mktemp" <<'EOF'
#!/usr/bin/env bash
if [[ ${INSTALLER_TEST_FAIL_STAGE:-} == launcher && ${1-} == *melearner-launcher* ]]; then
  exit 1
fi
exec /usr/bin/mktemp "$@"
EOF
cat >"$fixture/bin/sha256sum" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
for arg in "$@"; do file="$arg"; done
if [[ ${INSTALLER_TEST_LEGACY_HASH:-} && ${file##*/} =~ ^melearner_0\.1\.(0|9)_amd64\.AppImage$ ]]; then
  printf '%s  %s\n' "$INSTALLER_TEST_LEGACY_HASH" "$file"
else
  exec /usr/bin/sha256sum "$@"
fi
EOF
chmod 755 "$fixture/bin/"*

run_installer() {
  bwrap --ro-bind / / --tmpfs /tmp --dir /tmp/repo --ro-bind "$repo_root" /tmp/repo --proc /proc --dev /dev \
    --bind "$fixture" "$sandbox_fixture" \
    --bind "${TEST_FIXTURE_HOME:-$fixture/home}" "$HOME" \
    --unsetenv XDG_DATA_HOME \
    --setenv TMPDIR "$sandbox_fixture/tmp" \
    --setenv PATH "$sandbox_fixture/bin:/usr/bin:/bin" \
    --setenv INSTALLER_TEST_PAYLOAD "$sandbox_fixture/payload" \
    --setenv INSTALLER_TEST_HASH "${TEST_HASH:-$expected_hash}" \
    --setenv INSTALLER_TEST_URLS "$sandbox_fixture/urls" \
    --setenv INSTALLER_TEST_ARGS "$sandbox_fixture/args" \
    --setenv INSTALLER_TEST_SUDO_LOG "$sandbox_fixture/sudo.log" \
    --setenv INSTALLER_TEST_PACMAN_LOG "$sandbox_fixture/pacman.log" \
    --setenv INSTALLER_TEST_FAIL_STAGE "${TEST_FAIL_STAGE:-}" \
    --setenv INSTALLER_TEST_LEGACY_HASH "${TEST_LEGACY_HASH:-}" \
    --setenv INSTALLER_TEST_MALFORMED_CHECKSUM "${TEST_MALFORMED_CHECKSUM:-}" \
    --setenv INSTALLER_TEST_MALFORMED_CHECKSUM_NAME "${TEST_MALFORMED_CHECKSUM_NAME:-}" \
    --setenv INSTALLER_TEST_GLIBC "${TEST_GLIBC:-2.39}" \
    --setenv INSTALLER_TEST_MACHINE "${TEST_MACHINE:-x86_64}" \
    /bin/bash /tmp/repo/scripts/install-linux.sh "$@"
}

fail() { printf 'FAIL: %s\n' "$*" >&2; exit 1; }
pass() { printf 'PASS: %s\n' "$*"; }
assert_no_install() {
  [[ ! -e "$fixture/home/Applications/meLearner/melearner_0.1.1_amd64.AppImage" ]] || fail 'unexpected AppImage install'
  [[ ! -e "$fixture/home/.local/bin/melearner" ]] || fail 'unexpected launcher install'
  [[ ! -e "$fixture/home/.local/share/applications/io.github.whitehades.melearner.desktop" ]] || fail 'unexpected desktop install'
}

run_installer --help >"$fixture/help.out" || fail '--help failed'
rg -q 'Usage: bash scripts/install-linux.sh' "$fixture/help.out" || fail '--help output missing usage'
[[ ! -s "$fixture/urls" ]] || fail '--help attempted a download'
pass '--help succeeds without network access'

TEST_MACHINE=aarch64
export TEST_MACHINE
if run_installer >"$fixture/arch.out" 2>&1; then fail 'unsupported architecture accepted'; fi
rg -q 'x86_64 only' "$fixture/arch.out" || fail 'unsupported architecture message missing'
assert_no_install
unset TEST_MACHINE
pass 'unsupported architecture is rejected before download'

TEST_GLIBC=2.38
export TEST_GLIBC
if run_installer >"$fixture/glibc.out" 2>&1; then fail 'old glibc accepted'; fi
rg -q 'glibc 2.39 or newer' "$fixture/glibc.out" || fail 'old glibc message missing'
assert_no_install
unset TEST_GLIBC
pass 'glibc below 2.39 is rejected before download'

# A wrong checksum must leave no target and must remove the installer's temp dir.
TEST_HASH=0000000000000000000000000000000000000000000000000000000000000000
export TEST_HASH
if run_installer >"$fixture/hash.out" 2>&1; then fail 'bad checksum accepted'; fi
rg -q 'SHA-256 verification failed' "$fixture/hash.out" || fail 'checksum failure message missing'
assert_no_install
[[ -z "$(find "$fixture/tmp" -mindepth 1 -print -quit)" ]] || fail 'temporary download was not cleaned up'
unset TEST_HASH
pass 'checksum mismatch prevents installation and cleans temporary files'

TEST_MALFORMED_CHECKSUM=1
export TEST_MALFORMED_CHECKSUM
if run_installer >"$fixture/malformed-checksum.out" 2>&1; then fail 'malformed checksum accepted'; fi
rg -q 'invalid format' "$fixture/malformed-checksum.out" || fail 'malformed checksum message missing'
assert_no_install
[[ -z "$(find "$fixture/tmp" -mindepth 1 -print -quit)" ]] || fail 'malformed checksum left temporary files'
unset TEST_MALFORMED_CHECKSUM
pass 'malformed checksum is rejected and cleaned up'

TEST_MALFORMED_CHECKSUM_NAME=1
export TEST_MALFORMED_CHECKSUM_NAME
if run_installer >"$fixture/malformed-checksum-name.out" 2>&1; then fail 'checksum with a malformed asset filename was accepted'; fi
rg -q 'invalid format' "$fixture/malformed-checksum-name.out" || fail 'malformed checksum filename message missing'
assert_no_install
[[ -z "$(find "$fixture/tmp" -mindepth 1 -print -quit)" ]] || fail 'malformed checksum filename left temporary files'
unset TEST_MALFORMED_CHECKSUM_NAME
pass 'checksum with a malformed asset filename is rejected and cleaned up'

# Successful install: verify exact release URLs, installed modes/content, wrapper argv,
# desktop integration, and untouched SQLite-like user data.
rm -f "$fixture/urls"
mkdir -p "$fixture/home/.local/share/melearner"
printf 'existing progress database bytes\n' >"$fixture/home/.local/share/melearner/library.sqlite"
progress_before="$(sha256sum "$fixture/home/.local/share/melearner/library.sqlite" | cut -d ' ' -f 1)"
run_installer >"$fixture/install.out" || fail 'valid AppImage install failed'
app="$fixture/home/Applications/meLearner/melearner_0.1.1_amd64.AppImage"
launcher="$fixture/home/.local/bin/melearner"
desktop="$fixture/home/.local/share/applications/io.github.whitehades.melearner.desktop"
logical_app="$HOME/Applications/meLearner/melearner_0.1.1_amd64.AppImage"
logical_launcher="$HOME/.local/bin/melearner"
[[ -x "$app" && "$(sha256sum "$app" | cut -d ' ' -f 1)" == "$expected_hash" ]] || fail 'installed AppImage missing or incorrect'
[[ -x "$launcher" ]] || fail 'launcher missing or not executable'
grep -Fqx "Exec=\"$logical_launcher\"" "$desktop" || fail 'desktop entry has wrong or unescaped Exec target'
[[ "$(cat "$fixture/urls")" == $'https://github.com/WhiteHades/melearner/releases/download/v0.1.1/melearner_0.1.1_amd64.AppImage\nhttps://raw.githubusercontent.com/WhiteHades/melearner/main/packaging/checksums/v0.1.1/melearner_0.1.1_amd64.AppImage.sha256' ]] || fail 'AppImage download URLs were not exact'
INSTALLER_TEST_ARGS="$fixture/args" bwrap --ro-bind / / --tmpfs /tmp --dir /tmp/repo --ro-bind "$repo_root" /tmp/repo --proc /proc --dev /dev \
  --bind "$fixture" "$sandbox_fixture" --bind "$fixture/home" "$HOME" \
  --setenv INSTALLER_TEST_ARGS "$sandbox_fixture/args" \
  --setenv PATH "$sandbox_fixture/bin:/usr/bin:/bin" \
  /bin/bash "$logical_launcher" 'two words' --example
case "$(cat "$fixture/args")" in
  $'two words\n--example') pass 'FUSE-capable launcher preserves arguments' ;;
  $'--appimage-extract-and-run\ntwo words\n--example') pass 'fallback launcher preserves arguments' ;;
  *) fail 'launcher did not preserve arguments or selected a supported launch route' ;;
esac
progress_after="$(sha256sum "$fixture/home/.local/share/melearner/library.sqlite" | cut -d ' ' -f 1)"
[[ "$progress_before" == "$progress_after" ]] || fail 'existing progress data changed'
[[ -z "$(find "$fixture/tmp" -mindepth 1 -print -quit)" ]] || fail 'successful install left temporary files'
pass 'valid AppImage install creates expected files, preserves args/progress, and downloads exact assets'

# A known old install may have an exact launcher symlink and a legacy desktop entry.
rm "$launcher"
ln -s "$logical_app" "$launcher"
printf '[Desktop Entry]\nName=meLearner\nExec=%s %%F\nIcon=io.github.whitehades.melearner\nMimeType=inode/directory;\n' "$logical_launcher" >"$desktop"
run_installer >"$fixture/reinstall.out" || fail 'recognized existing launcher/desktop was rejected'
[[ -x "$launcher" && ! -L "$launcher" ]] || fail 'official launcher symlink was not replaced'
grep -Fqx "Exec=$logical_launcher %F" "$desktop" || fail 'recognized desktop Exec arguments were not preserved'
grep -Fqx 'Icon=io.github.whitehades.melearner' "$desktop" || fail 'recognized desktop icon was not preserved'
grep -Fqx 'MimeType=inode/directory;' "$desktop" || fail 'recognized desktop MIME type was not preserved'
backup_parent="$fixture/home/.local/state/melearner/backups"
backup_dir="$(find "$backup_parent" -mindepth 1 -maxdepth 1 -type d -print -quit)"
[[ -n "$backup_dir" && -L "$backup_dir/melearner-launcher" ]] || fail 'recognized launcher symlink was not backed up'
[[ -s "$backup_dir/io.github.whitehades.melearner.desktop" ]] || fail 'recognized desktop entry was not backed up'
grep -Fqx "Exec=$logical_launcher %F" "$backup_dir/io.github.whitehades.melearner.desktop" || fail 'recognized desktop backup lost Exec arguments'
grep -Fqx 'Icon=io.github.whitehades.melearner' "$backup_dir/io.github.whitehades.melearner.desktop" || fail 'recognized desktop backup lost icon'
grep -Fqx 'MimeType=inode/directory;' "$backup_dir/io.github.whitehades.melearner.desktop" || fail 'recognized desktop backup lost MIME type'
pass 'recognized official symlink and legacy desktop are backed up on reinstall'

# An older official launcher may still point at its versioned AppImage. It must
# be accepted only when the previous AppImage matches the verified fixture hash.
previous_app="$fixture/home/Applications/meLearner/melearner_0.1.0_amd64.AppImage"
logical_previous_app="$HOME/Applications/meLearner/melearner_0.1.0_amd64.AppImage"
cp "$fixture/payload" "$previous_app"
chmod 755 "$previous_app"
rm "$launcher"
ln -s "$logical_previous_app" "$launcher"
TEST_LEGACY_HASH=458ede022af3a54fd44186a803ece682710e6b4c025666bb7f4b36d5ff9a3891
export TEST_LEGACY_HASH
run_installer >"$fixture/previous-version-reinstall.out" || fail 'recognized previous-version AppImage symlink was rejected'
unset TEST_LEGACY_HASH
[[ -x "$launcher" && ! -L "$launcher" ]] || fail 'previous-version launcher symlink was not replaced'
[[ -f "$previous_app" && "$(sha256sum "$previous_app" | cut -d ' ' -f 1)" == "$expected_hash" ]] || fail 'previous official AppImage was not preserved'
pass 'verified official 0.1.0 AppImage symlink is upgraded while preserving old binary'

previous_app="$fixture/home/Applications/meLearner/melearner_0.1.9_amd64.AppImage"
logical_previous_app="$HOME/Applications/meLearner/melearner_0.1.9_amd64.AppImage"
cp "$fixture/payload" "$previous_app"
chmod 755 "$previous_app"
rm "$launcher"
ln -s "$logical_previous_app" "$launcher"
TEST_LEGACY_HASH=a7140578c6fa8f35514353bd09585595356f2ae64aecc12d602b11c868942bff
export TEST_LEGACY_HASH
run_installer >"$fixture/0.1.9-reinstall.out" || fail 'recognized previous 0.1.9 AppImage symlink was rejected'
unset TEST_LEGACY_HASH
[[ -x "$launcher" && ! -L "$launcher" ]] || fail 'previous 0.1.9 launcher symlink was not replaced'
[[ -f "$previous_app" && "$(sha256sum "$previous_app" | cut -d ' ' -f 1)" == "$expected_hash" ]] || fail 'previous 0.1.9 AppImage was not preserved'
pass 'verified official 0.1.9 AppImage symlink is upgraded while preserving old binary'

# A matching versioned filename is not sufficient: unknown content must not
# make the launcher symlink eligible for replacement.
known_launcher="$fixture/known-launcher"
cp -a "$launcher" "$known_launcher"
printf 'unrecognized AppImage bytes\n' >"$previous_app"
rm "$launcher"
ln -s "$logical_previous_app" "$launcher"
if run_installer >"$fixture/unknown-previous.out" 2>&1; then fail 'unknown-hash previous-version target was accepted'; fi
rg -q 'unrecognized launcher target' "$fixture/unknown-previous.out" || fail 'unknown previous target refusal message missing'
[[ -L "$launcher" && "$(readlink "$launcher")" == "$logical_previous_app" ]] || fail 'unknown previous-version launcher link changed'
rm "$launcher"
cp -a "$known_launcher" "$launcher"
pass 'unknown-hash versioned AppImage target is refused without changing the launcher'

# Existing unrelated target content must not be replaced.
printf 'user-owned launcher\n' >"$launcher"
if run_installer >"$fixture/existing.out" 2>&1; then fail 'unrelated launcher was overwritten'; fi
rg -q 'unrelated launcher' "$fixture/existing.out" || fail 'unrelated launcher refusal missing'
[[ "$(cat "$launcher")" == 'user-owned launcher' ]] || fail 'unrelated launcher content changed'
pass 'unrelated pre-existing launcher is preserved'

# Fail the second staging-file creation. The preinstalled AppImage stage and
# download directory must both be removed by the installer's EXIT trap.
mkdir -p "$fixture/failhome"
TEST_FIXTURE_HOME="$fixture/failhome" TEST_FAIL_STAGE=launcher
export TEST_FIXTURE_HOME TEST_FAIL_STAGE
if run_installer >"$fixture/stage-failure.out" 2>&1; then fail 'stage-creation failure was ignored'; fi
[[ -z "$(find "$fixture/tmp" -mindepth 1 -print -quit)" ]] || fail 'stage failure left download temporary files'
[[ -z "$(find "$fixture/failhome/Applications" -name '.melearner-*' -print -quit 2>/dev/null)" ]] || fail 'stage failure left staged install files'
[[ ! -e "$fixture/failhome/Applications/meLearner/melearner_0.1.1_amd64.AppImage" ]] || fail 'stage failure installed an incomplete AppImage'
unset TEST_FIXTURE_HOME TEST_FAIL_STAGE
pass 'staging failure cleans prior stage and download temporary files'

# Arch path verifies before invoking sudo pacman -U; sudo's stub deliberately execs
# the pacman stub, which records the normal interactive argument vector.
rm -f "$fixture/urls" "$fixture/pacman.log" "$fixture/sudo.log"
TEST_HASH=0000000000000000000000000000000000000000000000000000000000000000
export TEST_HASH
if run_installer --arch >"$fixture/arch-bad-checksum.out" 2>&1; then fail 'Arch package with bad checksum accepted'; fi
[[ ! -e "$fixture/pacman.log" && ! -e "$fixture/sudo.log" ]] || fail 'Arch bad checksum invoked package manager'
[[ -z "$(find "$fixture/tmp" -mindepth 1 -print -quit)" ]] || fail 'Arch checksum failure left temporary files'
unset TEST_HASH
pass 'Arch checksum failure prevents sudo and pacman'
rm -f "$fixture/urls"
run_installer --arch >"$fixture/arch-install.out" || fail 'Arch install failed'
[[ "$(cat "$fixture/urls")" == $'https://github.com/WhiteHades/melearner/releases/download/v0.1.1/melearner-bin-0.1.1-1-x86_64.pkg.tar.zst\nhttps://raw.githubusercontent.com/WhiteHades/melearner/main/packaging/checksums/v0.1.1/melearner-bin-0.1.1-1-x86_64.pkg.tar.zst.sha256' ]] || fail 'Arch download URLs were not exact'
rg -q '^pacman -U .*/melearner-bin-0.1.1-1-x86_64.pkg.tar.zst$' "$fixture/sudo.log" || fail 'Arch package was not passed to sudo pacman -U'
rg -q '^-U .*/melearner-bin-0.1.1-1-x86_64.pkg.tar.zst$' "$fixture/pacman.log" || fail 'pacman arguments changed'
[[ -z "$(find "$fixture/tmp" -mindepth 1 -print -quit)" ]] || fail 'Arch install left temporary files'
pass 'Arch mode downloads exact verified package and invokes normal sudo pacman -U'

printf 'All isolated Linux installer E2E checks passed.\n'
