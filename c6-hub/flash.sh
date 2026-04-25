#!/usr/bin/env bash
# Flash and monitor the c6-hub firmware on a connected ESP32-C6.
#
# Usage:
#   ./flash.sh                # auto-detect port, flash + monitor
#   ./flash.sh /dev/cu.usbmodemXXXX  # explicit port

set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
IDF_PATH="${IDF_PATH:-$HOME/esp/esp-idf-v6.0}"

if [ ! -f "$IDF_PATH/export.sh" ]; then
  echo "error: ESP-IDF not found at $IDF_PATH. Run Task 2 of the pivot plan." >&2
  exit 1
fi

# shellcheck disable=SC1091
. "$IDF_PATH/export.sh" >/dev/null

cd "$PROJECT_DIR"

PORT="${1:-}"
if [ -z "$PORT" ]; then
  PORT=$(ls /dev/cu.usbmodem* 2>/dev/null | head -1)
  if [ -z "$PORT" ]; then
    echo "error: no /dev/cu.usbmodem* device found. Connect the C6 via USB-C." >&2
    exit 1
  fi
  echo "info: auto-detected port $PORT"
fi

idf.py -p "$PORT" flash monitor
