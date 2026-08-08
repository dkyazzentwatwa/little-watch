#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ARDUINO_LIB_ROOT="${ARDUINO_LIB_ROOT:-${HOME}/Documents/Arduino/libraries}"

if [[ -z "${LITTLECUBE_FQBN:-}" ]]; then
  echo "Error: LITTLECUBE_FQBN is not set." >&2
  echo 'Example:' >&2
  echo '  export LITTLECUBE_FQBN="esp32:esp32:esp32s3:FlashSize=16M,PSRAM=opi,USBMode=hwcdc,CDCOnBoot=cdc,PartitionScheme=custom"' >&2
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

# Passing any --library disables automatic library discovery, so every
# dependency is listed explicitly (same approach as the proven reference
# firmware for this board).
echo "[build] compiling little-cube-os"
"${ARDUINO_CLI}" compile \
  --fqbn "${LITTLECUBE_FQBN}" \
  --warnings all \
  --library "${ARDUINO_LIB_ROOT}/Adafruit_GFX_Library" \
  --library "${ARDUINO_LIB_ROOT}/Adafruit_BusIO" \
  --library "${ARDUINO_LIB_ROOT}/Adafruit_XCA9554" \
  --library "${ARDUINO_LIB_ROOT}/GFX_Library_for_Arduino" \
  --library "${ARDUINO_LIB_ROOT}/XPowersLib" \
  --library "${ARDUINO_LIB_ROOT}/SensorLib" \
  --library "${ARDUINO_LIB_ROOT}/ArduinoJson" \
  --library "${ARDUINO_LIB_ROOT}/WebSockets" \
  --library "${ARDUINO_LIB_ROOT}/ESP8266Audio" \
  --library "${ARDUINO_LIB_ROOT}/JPEGDEC" \
  --library "${ROOT}/libraries/Arduino_DriveBus" \
  --output-dir "${ROOT}/dist" \
  "${ROOT}/little-cube-os"

echo "[build] output: ${ROOT}/dist"
