#!/usr/bin/env bash
# One-command build: recompile the ROM, then build the app.
# Needs tools/N64Recomp/build/N64Recomp (see docs/STATUS.md for the patch setup).
set -euo pipefail
cd "$(dirname "$0")/.."

RECOMP=tools/N64Recomp/build/N64Recomp
if [ ! -x "$RECOMP" ]; then
    echo "missing $RECOMP: build tools/N64Recomp first" >&2
    exit 1
fi

# N64Recomp does not clear stale output; remove it so renamed functions are re-emitted.
rm -rf generated/functions

"$RECOMP" mace.toml > build-recomp.log 2>&1 || {
    grep -E "Error|Unhandled" build-recomp.log >&2 || true
    echo "recompile failed, see build-recomp.log" >&2
    exit 1
}

cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release > /dev/null
cmake --build build -j"$(nproc)"
