#!/usr/bin/env bash
# Build production firmware and copy it to tools/flasher/ for embedding in RoombaFlasher.
#
# The firmware version is set automatically at compile time by tools/set_version.py
# using the git commit count as the PATCH number (format: MAJOR.MINOR.PATCH).
# No manual version bump is needed.
#
# Usage:
#   bash tools/flasher/prepare_release.sh
#   cd tools/flasher && bash build.sh   # then build the flasher executable
set -euo pipefail

# Add PlatformIO to PATH if not already available
if ! command -v pio &>/dev/null; then
    export PATH="$HOME/.platformio/penv/bin:$PATH"
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
BUILD_ENV="esp01_via_usb_prod"
FIRMWARE_SRC="${PROJECT_ROOT}/.pio/build/${BUILD_ENV}/firmware.bin"
FIRMWARE_DST="${SCRIPT_DIR}/firmware.bin"

echo "=== Building production firmware (${BUILD_ENV}) ==="
cd "${PROJECT_ROOT}"
pio run -e "${BUILD_ENV}"

if [ ! -f "${FIRMWARE_SRC}" ]; then
    echo "ERROR: firmware build succeeded but binary not found at ${FIRMWARE_SRC}"
    exit 1
fi

echo ""
echo "=== Copying firmware ==="
cp "${FIRMWARE_SRC}" "${FIRMWARE_DST}"
echo "  firmware.bin  $(du -sh "${FIRMWARE_DST}" | cut -f1)"

# Copy to the Rust flasher directory as well (for embedding via include_bytes!)
FLASHER_RS_DIR="${SCRIPT_DIR}/../flasher-rs"
if [ -d "${FLASHER_RS_DIR}" ]; then
    cp "${FIRMWARE_SRC}" "${FLASHER_RS_DIR}/firmware.bin"
    echo "  firmware.bin  also copied to flasher-rs/"
fi

echo ""
echo "=== Done ==="
echo "Next step: cd tools/flasher && bash build.sh"
