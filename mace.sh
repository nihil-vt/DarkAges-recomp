#!/bin/bash
# Mace: The Dark Age - PC Launcher
# Requires: retroarch (sudo pacman -S retroarch libretro-mupen64plus-next)

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROM_PATH="${1:-$SCRIPT_DIR/roms/Mace - The Dark Age (USA).z64}"
CORES_DIR="/usr/lib/libretro"
CORE="mupen64plus_next_libretro.so"
SYS_DIR="$SCRIPT_DIR/system"
SAVE_DIR="$SCRIPT_DIR/saves"

# Check RetroArch
if ! command -v retroarch &>/dev/null; then
    echo "RetroArch not found. Install with:"
    echo "  sudo pacman -S retroarch libretro-mupen64plus-next"
    exit 1
fi

# Check core
if [ ! -f "$CORES_DIR/$CORE" ]; then
    echo "Core not found at $CORES_DIR/$CORE"
    echo "Install with: sudo pacman -S libretro-mupen64plus-next"
    exit 1
fi

# Check ROM
if [ ! -f "$ROM_PATH" ]; then
    echo "ROM not found: $ROM_PATH"
    echo "Usage: $0 [path/to/Mace.z64]"
    exit 1
fi

# Create dirs
mkdir -p "$SYS_DIR" "$SAVE_DIR"

echo "[mace] Launching Mace: The Dark Age..."
echo "[mace] ROM: $ROM_PATH"

# Launch
retroarch \
    -L "$CORES_DIR/$CORE" \
    --subsystem "parallel" \
    "$ROM_PATH"
