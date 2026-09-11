#!/usr/bin/env bash
#
# build-ota.sh — build the firmware and (re)generate the ota/ payload from THAT build,
# so firmware.bin and merged.bin can never disagree with each other.
#
# ota/ holds everything needed to update or install the firmware, all in one place:
#   firmware.bin  the app image (flashed at 0x10000); used by `update ota`/`update local`.
#                 `update ota` reads the version straight out of this image's embedded
#                 esp_app_desc_t — there is no separate version marker to keep in sync.
#   merged.bin    bootloader + partition table + app in one image, flashed at 0x0 with
#                 esptool or the Espressif Flash Download Tool (first install / migration)
#
# Because both are written from the same `pio run`, they are always in sync within a
# refresh. The `pio test -e native` guard (test/test_ota_payload) then fails if the
# version embedded in the committed binaries drifts from version.h — e.g. after a version
# bump where you forgot to re-run this script.
#
# Usage:  tools/build-ota.sh          # build + regenerate ota/, then remind you to commit
#         tools/build-ota.sh --check  # only verify ota/ is in sync with the current build
#
set -euo pipefail

ENV=devkit-espidf
FLASH_SIZE=4MB           # the DEVKIT V1 board is 4 MB (the partition table runs to 0x3D0000)

REPO="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO"

BUILD_DIR=".pio/build/$ENV"
PIO="${PIO:-pio}"
# esptool ships with the PlatformIO espressif32 platform; fall back to a PATH copy.
ESPTOOL_PY="$(ls "$HOME"/.platformio/packages/tool-esptoolpy/esptool.py 2>/dev/null | head -1 || true)"
PYTHON="${PYTHON:-$HOME/.platformio/penv/bin/python}"
[ -x "$PYTHON" ] || PYTHON=python3

echo ">> building firmware ($ENV)"
"$PIO" run -e "$ENV"

for f in bootloader.bin partitions.bin firmware.bin; do
    [ -f "$BUILD_DIR/$f" ] || { echo "!! missing $BUILD_DIR/$f — build incomplete" >&2; exit 1; }
done

# Flash offsets for this board: bootloader 0x1000, partition table 0x8000, app 0x10000.
# (These match partitions.csv / the Installing Firmware wiki page.)
esptool() {
    if [ -n "$ESPTOOL_PY" ]; then "$PYTHON" "$ESPTOOL_PY" "$@"; else command esptool.py "$@"; fi
}

echo ">> writing ota/merged.bin (flash at 0x0)"
esptool --chip esp32 merge_bin \
    --output ota/merged.bin \
    --flash_mode dio --flash_freq 40m --flash_size "$FLASH_SIZE" \
    0x1000  "$BUILD_DIR/bootloader.bin" \
    0x8000  "$BUILD_DIR/partitions.bin" \
    0x10000 "$BUILD_DIR/firmware.bin"

echo ">> writing ota/firmware.bin (app image, flash at 0x10000)"
cp "$BUILD_DIR/firmware.bin" ota/firmware.bin

echo ">> verifying the regenerated payload is in sync"
"$PIO" test -e native -f test_ota_payload

echo
echo "ota/ payload rebuilt for version $(cat version.txt):"
ls -l ota/
echo
echo "Review and commit the ota/ changes when ready."
