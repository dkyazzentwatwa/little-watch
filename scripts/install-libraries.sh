#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
INDEX_URL="https://espressif.github.io/arduino-esp32/package_esp32_index.json"

ARDUINO_CLI="${ARDUINO_CLI:-$(command -v arduino-cli || true)}"
if [[ -z "${ARDUINO_CLI}" && -x /opt/homebrew/bin/arduino-cli ]]; then
  ARDUINO_CLI="/opt/homebrew/bin/arduino-cli"
fi
if [[ -z "${ARDUINO_CLI}" ]]; then
  echo "Error: arduino-cli not found on PATH. Install it first (brew install arduino-cli)." >&2
  exit 127
fi

echo "[install] esp32 core 3.3.8"
"${ARDUINO_CLI}" core update-index --additional-urls "${INDEX_URL}"
"${ARDUINO_CLI}" core install esp32:esp32@3.3.8 --additional-urls "${INDEX_URL}"

echo "[install] libraries"
"${ARDUINO_CLI}" lib install "GFX Library for Arduino@1.6.6"
"${ARDUINO_CLI}" lib install "Adafruit GFX Library"
"${ARDUINO_CLI}" lib install "Adafruit BusIO"
"${ARDUINO_CLI}" lib install "Adafruit XCA9554"
"${ARDUINO_CLI}" lib install "XPowersLib"
"${ARDUINO_CLI}" lib install "SensorLib"
"${ARDUINO_CLI}" lib install "ArduinoJson@7.4.3"
"${ARDUINO_CLI}" lib install "ESP8266Audio@2.4.1"

if [[ ! -d "${ROOT}/libraries/Arduino_DriveBus" ]]; then
  echo "Error: vendored libraries/Arduino_DriveBus is missing from this repo." >&2
  exit 1
fi

echo "[install] done. Vendored: libraries/Arduino_DriveBus (FT3168 touch)."
