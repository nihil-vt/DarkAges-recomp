#!/usr/bin/env python3
"""Prepare config/symbols.toml for the recompiler.

Adds the manual entries the port needs:
- The game's entry stub at 0x80000400 (Ghidra did not recognise it).
- The crt0's jump target at 0x8006BF64 (mid-function, needs native trampoline).
"""

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from fix_symbols import parse, write  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent

# The IPL3 secondary boot code at physical 0x0 (kseg0 0x80000000).
# The crt0 jumps here after initialization. It should call the game's main loop.
IPL3_TRAMPOLINE = (0x80000000, 4)

# The game's entry stub at the ROM header's entry point.
ENTRY_STUB = (0x80000400, 0x130)

# The crt0 jumps to 0x8006BF64 (mid-function in FUN_8006be10).
# The real init is at 0x8006CB64. Add a native trampoline.
TRAMPOLINE = (0x8006BF64, 4)


def main():
    sections = parse(ROOT / "config" / "symbols.toml")
    for sec in sections:
        if sec.name == "code":
            sec.funcs.append([f"func_{IPL3_TRAMPOLINE[0]:08x}", IPL3_TRAMPOLINE[0], IPL3_TRAMPOLINE[1]])
            sec.funcs.append([f"func_{ENTRY_STUB[0]:08x}", ENTRY_STUB[0], ENTRY_STUB[1]])
            sec.funcs.append([f"func_{TRAMPOLINE[0]:08x}", TRAMPOLINE[0], TRAMPOLINE[1]])
            sec.funcs.sort(key=lambda f: f[1])
    write(ROOT / "config" / "symbols.toml", sections)
    print("symbols prepared")


if __name__ == "__main__":
    main()