#!/usr/bin/env bash
# Build the c6-hub firmware for ESP32-C6.
#
# Usage:
#   ./build.sh                 # configure + build
#   ./build.sh fullclean       # wipe build directory
#   ./build.sh menuconfig      # interactive Kconfig (use with caution)

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

case "${1:-build}" in
  fullclean)
    idf.py fullclean
    ;;
  menuconfig)
    idf.py menuconfig
    ;;
  build|"")
    idf.py set-target esp32c6
    idf.py build
    ;;
  *)
    idf.py "$@"
    ;;
esac
