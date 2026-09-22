#!/usr/bin/env bash
set -euo pipefail

if [[ ! -f CMakeLists.txt || ! -d fixtures/parity ]]; then
  echo "run this script from the melearner repository root" >&2
  exit 1
fi

expected_version="n8.1.2"
actual_version="$(ffmpeg -hide_banner -version | sed -n '1s/^ffmpeg version \([^ ]*\).*/\1/p')"
if [[ "$actual_version" != "$expected_version" ]]; then
  echo "media corpus requires ffmpeg $expected_version, found ${actual_version:-unknown}" >&2
  exit 1
fi

media_root="$PWD/fixtures/parity/media"
work_root="$PWD/.tmp/generate-media-corpus"
rm -rf "$work_root"
mkdir -p "$media_root/Systems 日本語" "$work_root"
trap 'rm -rf "$work_root"' EXIT

common=(
  -hide_banner
  -loglevel error
  -y
)

output_common=(
  -bitexact
  -fflags +bitexact
  -flags:v +bitexact
  -flags:a +bitexact
)

LC_ALL=C SOURCE_DATE_EPOCH=0 ffmpeg "${common[@]}" \
  -f lavfi -i "testsrc2=size=320x180:rate=10:duration=2" \
  -f lavfi -i "sine=frequency=440:sample_rate=48000:duration=2" \
  "${output_common[@]}" \
  -map_metadata -1 \
  -map 0:v:0 -map 1:a:0 \
  -c:v libx264 -preset veryslow -crf 28 -pix_fmt yuv420p \
  -x264-params "threads=1:lookahead-threads=1:sliced-threads=0" \
  -c:a aac -b:a 64k \
  -metadata creation_time=1970-01-01T00:00:00Z \
  -movflags +faststart \
  "$media_root/Systems 日本語/01 H264 AAC.mp4"

LC_ALL=C SOURCE_DATE_EPOCH=0 ffmpeg "${common[@]}" \
  -f lavfi -i "testsrc2=size=320x180:rate=10:duration=2" \
  -f lavfi -i "sine=frequency=440:sample_rate=48000:duration=2" \
  -f lavfi -i "sine=frequency=660:sample_rate=48000:duration=2" \
  -f ffmetadata -i "$media_root/chapters.ffmeta" \
  "${output_common[@]}" \
  -map 0:v:0 -map 1:a:0 -map 2:a:0 -map_metadata 3 \
  -c:v libx264 -preset veryslow -crf 28 -pix_fmt yuv420p \
  -x264-params "threads=1:lookahead-threads=1:sliced-threads=0" \
  -c:a aac -b:a 64k \
  -metadata:s:a:0 language=eng -metadata:s:a:0 title=English \
  -metadata:s:a:1 language=jpn -metadata:s:a:1 title=Japanese \
  "$media_root/02 Multi audio chapters.mkv"

LC_ALL=C SOURCE_DATE_EPOCH=0 ffmpeg "${common[@]}" \
  -f lavfi -i "testsrc2=size=320x180:rate=10:duration=2,format=yuv420p10le" \
  "${output_common[@]}" \
  -map_metadata -1 \
  -map 0:v:0 \
  -c:v libx265 -preset veryslow -crf 32 -pix_fmt yuv420p10le \
  -x265-params "log-level=error:frame-threads=1:pools=none:wpp=0" \
  "$media_root/03 HEVC Main 10.mkv"

sha256sum \
  "$media_root/Systems 日本語/01 H264 AAC.mp4" \
  "$media_root/02 Multi audio chapters.mkv" \
  "$media_root/03 HEVC Main 10.mkv"
