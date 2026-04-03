#!/usr/bin/env bash
# Build RoombaFlasher for Linux (native) and Windows (cross-compile).
#
# Prerequisites (one-time setup):
#   rustup target add x86_64-pc-windows-gnu
#   sudo apt install gcc-mingw-w64-x86-64
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "${SCRIPT_DIR}"

if [ ! -f firmware.bin ]; then
    echo "ERROR: firmware.bin not found. Run tools/flasher/prepare_release.sh first."
    exit 1
fi

echo "=== Building for Linux ==="
cargo build --release
echo "  -> target/release/RoombaFlasher"

echo ""
echo "=== Cross-compiling for Windows ==="
cargo build --release --target x86_64-pc-windows-gnu
echo "  -> target/x86_64-pc-windows-gnu/release/RoombaFlasher.exe"

echo ""
echo "=== Done ==="
ls -lh target/release/RoombaFlasher \
       target/x86_64-pc-windows-gnu/release/RoombaFlasher.exe
