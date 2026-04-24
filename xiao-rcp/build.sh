#!/usr/bin/env bash
# Build the OpenThread RCP firmware for XIAO nRF52840 Plus.
#
# Usage:
#   ./build.sh               # build only
#   ./build.sh --uf2         # build and produce xiao-rcp.uf2 in xiao-rcp/
#
# Must be invoked from inside an `nrfutil toolchain-manager launch --shell` session.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
NCS_DIR="${NCS_DIR:-$HOME/ncs/v3.3.0}"
SAMPLE="$NCS_DIR/nrf/samples/openthread/coprocessor"
BUILD_DIR="$REPO_ROOT/xiao-rcp/build"

if ! command -v west >/dev/null 2>&1; then
  echo "error: west not on PATH. Run: nrfutil toolchain-manager launch --ncs-version v3.3.0 --shell" >&2
  exit 1
fi

if [ ! -d "$SAMPLE" ]; then
  echo "error: ncs sample not found at $SAMPLE. Did you run 'west update'?" >&2
  exit 1
fi

west build \
  -b xiao_ble/nrf52840 \
  -p always \
  -d "$BUILD_DIR" \
  "$SAMPLE" \
  -- \
  -DEXTRA_DTC_OVERLAY_FILE="$REPO_ROOT/xiao-rcp/boards/xiao_ble_nrf52840.overlay" \
  -DEXTRA_CONF_FILE="$REPO_ROOT/xiao-rcp/prj.conf"

HEX="$BUILD_DIR/zephyr/zephyr.hex"
if [ ! -f "$HEX" ]; then
  echo "error: build succeeded but $HEX not produced" >&2
  exit 1
fi

echo "build ok: $HEX"

if [ "${1:-}" = "--uf2" ]; then
  UF2_OUT="$REPO_ROOT/xiao-rcp/xiao-rcp.uf2"
  python3 "$REPO_ROOT/xiao-rcp/uf2conv.py" \
    --family 0xADA52840 \
    --convert \
    --output "$UF2_OUT" \
    "$HEX"
  echo "uf2 ok: $UF2_OUT"
fi
