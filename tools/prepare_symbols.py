#!/usr/bin/env python3
"""Prepare config/symbols.toml for the recompiler.

Regenerates the symbol file from the raw Ghidra function list and adds the
few manual entries the port needs (the native stubs that replace the IPL3 boot
code calls, and the two entry point candidates the cart's boot code calls).
"""

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from fix_symbols import parse, write, split_at  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent

# Native stubs that replace calls into the IPL3 boot code.
STUBS = [0x8007D200, 0x8007D204, 0x8007D208, 0x8007D20C, 0x8007D210]

# The cart's boot code calls into the game code at these two addresses; they
# are the candidates for the game's main function.
ENTRY_CANDIDATES = [0x80061840, 0x80021020]

# The game's entry stub. Ghidra did not recognise it as a function, so it is
# added by hand (it is the recomp's entry point).
ENTRY_STUB = (0x80001000, 0x130)

# The code section ends where the data section starts; Ghidra also produced
# functions for the data, which must not be recompiled.
CODE_END = 0x8007B3AC


def main():
    sections = parse(ROOT / "config" / "symbols.toml")
    for sec in sections:
        if sec.name == "code":
            sec.funcs = [f for f in sec.funcs if f[1] < CODE_END]
            sec.size = CODE_END - sec.vram
            for addr in STUBS:
                sec.funcs.append([f"func_{addr:08x}", addr, 4])
            sec.funcs.append([f"func_{ENTRY_STUB[0]:08x}", ENTRY_STUB[0], ENTRY_STUB[1]])
        for addr in ENTRY_CANDIDATES:
            split_at(sections, addr)
        sec.funcs.sort(key=lambda f: f[1])
    write(ROOT / "config" / "symbols.toml", sections)
    print("symbols prepared")


if __name__ == "__main__":
    main()
