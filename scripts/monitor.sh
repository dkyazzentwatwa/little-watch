#!/usr/bin/env bash
set -euo pipefail

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

exec "${ARDUINO_CLI}" monitor --port "${LITTLECUBE_PORT}" --config baudrate=115200
