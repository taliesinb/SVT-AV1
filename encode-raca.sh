#!/bin/bash
# encode-raca.sh IN.raw OUT.mp4 [options] [-- extra SvtAv1EncApp args]
#
# Raw rgba64le frames (raca's 16-bit renderer output; alpha ignored) -> 10-bit 4:2:0 BT.709 limited-range y4m
# (ffmpeg) -> SvtAv1EncApp from this fork with --dither-grain -> MP4 with BT.709 tags (ffmpeg, stream copy).
#
# Feed the UNDITHERED render: the smooth 10-bit picture compresses well, and the designed film grain makes every
# conforming AV1 decoder (dav1d, Apple VideoToolbox/AVFoundation, ...) add ~1 8-bit step of white noise at output.
#
# Options:
#   -s WxH          frame size (default 1280x720)
#   -r FPS          frame rate (default 60; fractions such as 30000/1001 allowed)
#   -q CRF          constant rate factor (default 26)
#   -p PRESET       SVT-AV1 preset (default 6)
#   -g AMP          --dither-grain luma amplitude, TPDF-equivalent 8-bit steps (default 1.25; 0 = off)
#   -c AMP          --dither-grain-chroma amplitude (default 0)
#   -k              keep the intermediate .ivf next to OUT
# Environment: SVTAV1ENCAPP (default: Bin/Release/SvtAv1EncApp next to this script), FFMPEG (default: ffmpeg)
set -euo pipefail

usage() { sed -n '2,/^set -euo/p' "$0" | sed '$d; s/^# \{0,1\}//'; exit "${1:-1}"; }
[ $# -ge 2 ] || usage
IN=$1; OUT=$2; shift 2
SIZE=1280x720; FPS=60; CRF=26; PRESET=6; GRAIN=1.25; CGRAIN=0; KEEP=0
while [ $# -gt 0 ]; do
  case $1 in
    -s) SIZE=$2; shift 2;;
    -r) FPS=$2; shift 2;;
    -q) CRF=$2; shift 2;;
    -p) PRESET=$2; shift 2;;
    -g) GRAIN=$2; shift 2;;
    -c) CGRAIN=$2; shift 2;;
    -k) KEEP=1; shift;;
    -h|--help) usage 0;;
    --) shift; break;;
    *) echo "unknown option $1" >&2; usage;;
  esac
done
HERE=$(cd "$(dirname "$0")" && pwd)
APP=${SVTAV1ENCAPP:-$HERE/Bin/Release/SvtAv1EncApp}
FFMPEG=${FFMPEG:-ffmpeg}
[ -x "$APP" ] || { echo "SvtAv1EncApp not found at $APP (build: cmake -S . -B Build/release -G Ninja -DCMAKE_BUILD_TYPE=Release && ninja -C Build/release SvtAv1EncApp)" >&2; exit 1; }
[ -f "$IN" ] || { echo "no such input: $IN" >&2; exit 1; }

IVF=${OUT%.*}.ivf
GRAIN_ARGS=()
if [ "$GRAIN" != 0 ] || [ "$CGRAIN" != 0 ]; then
  GRAIN_ARGS=(--dither-grain "$GRAIN" --dither-grain-chroma "$CGRAIN")
fi

"$FFMPEG" -nostdin -hide_banner -loglevel error -f rawvideo -pix_fmt rgba64le -s "$SIZE" -r "$FPS" -i "$IN" \
  -vf "scale=out_color_matrix=bt709:out_range=tv,format=yuv420p10le" -strict -1 -f yuv4mpegpipe - |
  "$APP" -i stdin -b "$IVF" --preset "$PRESET" --crf "$CRF" --tune 0 \
    --color-primaries bt709 --transfer-characteristics bt709 --matrix-coefficients bt709 --color-range studio \
    ${GRAIN_ARGS[@]+"${GRAIN_ARGS[@]}"} "$@" --progress 0

"$FFMPEG" -nostdin -hide_banner -loglevel error -y -i "$IVF" -c copy \
  -colorspace bt709 -color_primaries bt709 -color_trc bt709 -color_range tv -movflags +faststart "$OUT"
[ "$KEEP" = 1 ] || rm -f "$IVF"
echo "$OUT: $(( $(stat -f %z "$OUT" 2>/dev/null || stat -c %s "$OUT") / 1024 )) KB" >&2
