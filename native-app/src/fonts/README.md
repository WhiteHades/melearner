# melearner UI font

`melearner-ui.ttf` is a Modified Version that combines Native SDK's bundled Geist Regular coverage with Noto Sans JP 2.004-H2 Japanese outlines. It preserves all 649 codepoints mapped by the built-in face and adds Japanese punctuation, kana, and the Unicode 17.0 Jōyō and Jinmeiyō kanji sets. The result maps 3,904 BMP codepoints. The complete notices are packaged from `../../assets/licenses/melearner-ui/`.

- Geist source: `vercel-labs/native@f7aa92af6dcece250feba852af4d22e7f5429312`
- Geist source SHA-256: `5e7c3b25da393b1619253c655f95c5d123184759a4961562add5b0f5386e63c9`
- Noto source: `google/fonts@295d98a7a0c17c68f1341eaeea354e7960ea70d3`
- Noto source SHA-256: `c2f3b4d463500a2ddcd3849cded1fceeb9fd6d1c32e6cbecd568453ba50fc68f`
- Noto static Regular SHA-256: `946280470c7f8dff9c7256a10c6fb06544c75e83553e906a1a0ad946211de7ed`
- Unihan source: Unicode 17.0.0
- Unihan source SHA-256: `f7a48b2b545acfaa77b2d607ae28747404ce02baefee16396c5d2d7a8ef34b5e`
- Output SHA-256: `8f57affb411a388eeaa7a2e08a7013a49cccbc65d8db8418b2495a41ba088cc6`
- Generator: FontTools 4.63.0

```sh
set -euo pipefail
repo_root=$(git rev-parse --show-toplevel)
workdir="$repo_root/.tmp/melearner-ui-font"
output="$repo_root/native-app/src/fonts/melearner-ui.ttf"
generated="$workdir/melearner-ui.ttf"
rm -rf "$workdir"
mkdir -p "$workdir"
trap 'rm -rf "$workdir"; rmdir "$repo_root/.tmp" 2>/dev/null || true' EXIT
curl -fsSL 'https://raw.githubusercontent.com/vercel-labs/native/f7aa92af6dcece250feba852af4d22e7f5429312/src/primitives/canvas/fonts/Geist-Regular.ttf' -o "$workdir/Geist-Regular.ttf"
curl -fsSL 'https://raw.githubusercontent.com/google/fonts/295d98a7a0c17c68f1341eaeea354e7960ea70d3/ofl/notosansjp/NotoSansJP%5Bwght%5D.ttf' -o "$workdir/NotoSansJP-wght.ttf"
curl -fsSL 'https://www.unicode.org/Public/17.0.0/ucd/Unihan.zip' -o "$workdir/Unihan.zip"
printf '%s  %s\n' '5e7c3b25da393b1619253c655f95c5d123184759a4961562add5b0f5386e63c9' "$workdir/Geist-Regular.ttf" | sha256sum --check --strict
printf '%s  %s\n' 'c2f3b4d463500a2ddcd3849cded1fceeb9fd6d1c32e6cbecd568453ba50fc68f' "$workdir/NotoSansJP-wght.ttf" | sha256sum --check --strict
printf '%s  %s\n' 'f7a48b2b545acfaa77b2d607ae28747404ce02baefee16396c5d2d7a8ef34b5e' "$workdir/Unihan.zip" | sha256sum --check --strict
export SOURCE_DATE_EPOCH=1784100876
fonttools varLib.instancer "$workdir/NotoSansJP-wght.ttf" wght=400 --update-name-table --output="$workdir/NotoSansJP-Regular-400.ttf"
printf '%s  %s\n' '946280470c7f8dff9c7256a10c6fb06544c75e83553e906a1a0ad946211de7ed' "$workdir/NotoSansJP-Regular-400.ttf" | sha256sum --check --strict
unzip -p "$workdir/Unihan.zip" Unihan_OtherMappings.txt | awk '$2 == "kJoyoKanji" || $2 == "kJinmeiyoKanji" { print $1 }' > "$workdir/japanese-unicodes.txt"
printf '%s\n' 'U+3000-303F' 'U+3040-30FF' 'U+31F0-31FF' >> "$workdir/japanese-unicodes.txt"
pyftsubset "$workdir/Geist-Regular.ttf" --unicodes='*' --output-file="$workdir/Geist-min.ttf" '--drop-tables+=DSIG,GDEF,GPOS,GSUB' --no-layout-closure
pyftsubset "$workdir/NotoSansJP-Regular-400.ttf" --unicodes-file="$workdir/japanese-unicodes.txt" --output-file="$workdir/NotoSansJP-common-min.ttf" '--drop-tables+=BASE,GPOS,GSUB,STAT,vhea,vmtx' --no-layout-closure '--name-IDs=*' --name-legacy '--name-languages=*'
fonttools merge --output-file="$generated" "$workdir/Geist-min.ttf" "$workdir/NotoSansJP-common-min.ttf"
printf '%s  %s\n' '8f57affb411a388eeaa7a2e08a7013a49cccbc65d8db8418b2495a41ba088cc6' "$generated" | sha256sum --check --strict
mv "$generated" "$output"
```
