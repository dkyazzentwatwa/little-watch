#!/usr/bin/env bash
set -euo pipefail

# Transcode any video ffmpeg can read into a .lcv for the cube.
# Usage: ./scripts/pack-video.sh input.mkv [output.lcv]
# 448x252 letterboxed, rotated 90 CW (transpose=1) to 252x448 so frames land
# on the portrait panel with no runtime rotation; 15 fps; 22.05 kHz mono PCM.

command -v ffmpeg >/dev/null 2>&1 || { echo "error: ffmpeg not found — brew install ffmpeg" >&2; exit 127; }

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
IN="${1:?usage: pack-video.sh input.mkv [output.lcv]}"
OUT="${2:-${IN%.*}.lcv}"
FPS=15
RATE=22050
TMP="$(mktemp -d)"
trap 'rm -rf "${TMP}"' EXIT

echo "[pack] video pass (mjpeg ${FPS} fps, rotated)"
ffmpeg -hide_banner -loglevel error -y -i "${IN}" \
  -vf "scale=w=448:h=252:force_original_aspect_ratio=decrease,pad=448:252:(ow-iw)/2:(oh-ih)/2,transpose=1,fps=${FPS}" \
  -c:v mjpeg -q:v 7 -pix_fmt yuvj420p -an -f image2pipe "${TMP}/video.mjpeg"

echo "[pack] audio pass (${RATE} Hz mono s16le)"
ffmpeg -hide_banner -loglevel error -y -i "${IN}" \
  -vn -ac 1 -ar "${RATE}" -c:a pcm_s16le -f s16le "${TMP}/audio.pcm"

python3 "${ROOT}/scripts/lcv_mux.py" mux \
  --video "${TMP}/video.mjpeg" --audio "${TMP}/audio.pcm" \
  --fps "${FPS}" --rate "${RATE}" --width 252 --height 448 -o "${OUT}"
python3 "${ROOT}/scripts/lcv_mux.py" inspect "${OUT}"
echo "[pack] done: ${OUT} — copy to the card under /littlecube/video/"
