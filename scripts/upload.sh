#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

if [[ -z "${LITTLECUBE_FQBN:-}" ]]; then
  echo "Error: LITTLECUBE_FQBN is not set." >&2
  exit 1
fi
if [[ -z "${LITTLECUBE_PORT:-}" ]]; then
  echo "Error: LITTLECUBE_PORT is not set." >&2
  echo 'Example: export LITTLECUBE_PORT="/dev/cu.usbmodem101"' >&2
  exit 1
fi

ARDUINO_CLI="${ARDUINO_CLI:-$(command -v arduino-cli || true)}"
if [[ -z "${ARDUINO_CLI}" && -x /opt/homebrew/bin/arduino-cli ]]; then
  ARDUINO_CLI="/opt/homebrew/bin/arduino-cli"
fi
if [[ -z "${ARDUINO_CLI}" ]]; then
  echo "Error: arduino-cli not found on PATH." >&2
  exit 127
fi

if [[ ! -f "${ROOT}/dist/little-cube-os.ino.bin" ]]; then
  echo "[upload] no build found in dist/; compiling first"
  "${ROOT}/scripts/build.sh"
fi

PORT="${LITTLECUBE_PORT}"

# ESP32-S3 with USBMode=hwcdc + CDCOnBoot=cdc needs the 1200-baud "touch" to
# enter the bootloader; the CDC port disappears and re-enumerates, so we
# re-resolve it before uploading.
echo "[upload] touching ${PORT} at 1200 baud"
stty -f "${PORT}" 1200 || true
sleep 2

NEW_PORT="$(ls /dev/cu.usbmodem* 2>/dev/null | head -n 1 || true)"
if [[ -n "${NEW_PORT}" ]]; then
  PORT="${NEW_PORT}"
fi

echo "[upload] uploading on ${PORT}"
"${ARDUINO_CLI}" upload \
  --fqbn "${LITTLECUBE_FQBN}" \
  --input-dir "${ROOT}/dist" \
  -p "${PORT}" \
  "${ROOT}/little-cube-os"

echo "[upload] done"
