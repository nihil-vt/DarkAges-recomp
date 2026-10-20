#!/usr/bin/env python3
"""Search for the game's real entry point.

The ROM contains code from two different builds, so the function that the
entry stub jumps to does not behave correctly. This script starts the game at
every known function and reports the ones that get the furthest (detected by
the game doing a PI DMA, which only happens once the real startup code runs).
"""

import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
APP = ROOT / "build" / "mace-recomp"
ROM = ROOT / "roms" / "Mace - The Dark Age (USA).z64"

sys.path.insert(0, str(ROOT / "tools"))
from fix_symbols import parse  # noqa: E402

TIMEOUT = 4


def candidate_addresses():
    sections = parse(ROOT / "config" / "symbols.toml")
    out = []
    for sec in sections:
        if sec.name != "code":
            continue
        for name, vram, size in sec.funcs:
            if size >= 0x10:
                out.append((vram, name))
    return sorted(out)


def run(entry):
    try:
        proc = subprocess.run(
            [str(APP), str(ROM)],
            env={"MACE_ENTRY": hex(entry), "MACE_CONFIG": f"/home/nihil/mace-cfg/{entry:08x}", "PATH": "/usr/bin:/bin"},
            capture_output=True, text=True, timeout=TIMEOUT)
    except subprocess.TimeoutExpired as e:
        out = (e.stdout or b"") + (e.stderr or b"")
        text = out.decode("utf-8", "replace") if isinstance(out, bytes) else out
        return entry, "timeout", text
    text = proc.stdout + proc.stderr
    return entry, "crash" if proc.returncode != 0 else "clean", text


def main():
    cands = candidate_addresses()
    if len(sys.argv) > 1:
        lo, hi = int(sys.argv[1], 0), int(sys.argv[2], 0)
        cands = [c for c in cands if lo <= c[0] < hi]
    print(f"testing {len(cands)} candidates")
    best = []
    with ThreadPoolExecutor(max_workers=8) as pool:
        for entry, status, text in pool.map(run, [c[0] for c in cands]):
            if "PI DMA" in text:
                print(f"*** HIT 0x{entry:08X}: PI DMA seen")
                for line in text.splitlines():
                    if "PI DMA" in line:
                        print("   ", line)
                best.append((entry, text))
            elif status == "timeout":
                print(f"--- survived 0x{entry:08X}")
                best.append((entry, text))
    print(f"\n{len(best)} candidates worth a look")


if __name__ == "__main__":
    main()
