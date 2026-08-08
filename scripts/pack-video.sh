#!/usr/bin/env bash
set -euo pipefail

# Transcode any video ffmpeg can read into a .lcv for the cube.
# Usage: ./scripts/pack-video.sh [--upright|--rotated] input.mkv [output.lcv]
#
# Two orientations, matching the cube's `videoorient` setting. Frames are fit
# (never padded, never cropped) into the picture box the mode leaves free, and
# the orientation is recorded in the header so the device knows which it got.
#
#   --upright (default)  No rotation. Frames land the same way up as the Home
#     screen, fit into the 368x312 box above the horizontal chrome band. A 16:9
#     source stores at 368x206, a 4:3 at 368x276. This is the mode for wearing
#     the cube on a wrist, where it cannot be turned to meet the picture.
#
#   --rotated            Frames are fit into the 448x310 landscape box and then
#     rotated 90 CW (transpose=1) so they land on the portrait panel with no
#     runtime rotation; the user turns the device sideways. A 16:9 source lands
#     at 252x448 stored, a 4:3 at ~310x412. Wider picture, hand-held.
#
# Files packed the "wrong" way for the current setting still play — the device
# turns and scales them during decode — but matching the two is free and the
# device then blits 1:1. 15 fps; 22.05 kHz mono PCM.

command -v ffmpeg >/dev/null 2>&1 || { echo "error: ffmpeg not found — brew install ffmpeg" >&2; exit 127; }

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ORIENT=upright
while [[ "${1:-}" == --* ]]; do
  case "$1" in
    --upright) ORIENT=upright; shift ;;
    --rotated) ORIENT=rotated; shift ;;
    *) echo "error: unknown option $1" >&2; exit 2 ;;
  esac
done
IN="${1:?usage: pack-video.sh [--upright|--rotated] input.mkv [output.lcv]}"
OUT="${2:-${IN%.*}.lcv}"
FPS=15
RATE=22050
TMP="$(mktemp -d)"
trap 'rm -rf "${TMP}"' EXIT

if [ "${ORIENT}" = "upright" ]; then
  VF="scale=w=368:h=312:force_original_aspect_ratio=decrease:force_divisible_by=2,fps=${FPS}"
else
  VF="scale=w=448:h=310:force_original_aspect_ratio=decrease:force_divisible_by=2,transpose=1,fps=${FPS}"
fi

echo "[pack] video pass (mjpeg ${FPS} fps, ${ORIENT})"
ffmpeg -hide_banner -loglevel error -y -i "${IN}" \
  -vf "${VF}" \
  -c:v mjpeg -q:v 7 -pix_fmt yuvj420p -an -f image2pipe "${TMP}/video.mjpeg"

echo "[pack] audio pass (${RATE} Hz mono s16le)"
ffmpeg -hide_banner -loglevel error -y -i "${IN}" \
  -vn -ac 1 -ar "${RATE}" -c:a pcm_s16le -f s16le "${TMP}/audio.pcm"

python3 "${ROOT}/scripts/lcv_mux.py" mux \
  --video "${TMP}/video.mjpeg" --audio "${TMP}/audio.pcm" \
  --fps "${FPS}" --rate "${RATE}" --orientation "${ORIENT}" -o "${OUT}"
python3 "${ROOT}/scripts/lcv_mux.py" inspect "${OUT}"
echo "[pack] done: ${OUT} — copy to the card under /littlecube/video/"
