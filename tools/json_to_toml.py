#!/usr/bin/env python3
"""Regenerate config/symbols.toml from the raw Ghidra function list."""

import json
import re
import sys
from pathlib import Path

SECTIONS = [
    # name, rom, vram, size
    ("boot", 0x400, 0x80000400, 0xC00),
    ("code", 0x1000, 0x80001000, 0x7D200),
]


def main():
    json_path = Path(sys.argv[1])
    out_path = Path(sys.argv[2])
    text = json_path.read_text()
    # The Ghidra export is a JSON array with a trailing comma; strip it.
    text = re.sub(r",\s*]", "]", text)
    funcs = json.loads(text)

    out = ["# Auto-generated"]
    for name, rom, vram, size in SECTIONS:
        out += ["[[section]]", f'name = "{name}"', f"rom = 0x{rom:x}", f"vram = 0x{vram:x}", f"size = 0x{size:x}"]
        for func in funcs:
            fvram = func["vram"]
            if not (vram <= fvram < vram + size):
                continue
            out += ["", "[[section.functions]]", f'name = "func_{fvram:08x}"',
                    f"vram = 0x{fvram:x}", f"size = 0x{func['size']:x}"]
        out.append("")
    out_path.write_text("\n".join(out) + "\n")
    print(f"wrote {out_path} with {len(funcs)} functions")


if __name__ == "__main__":
    main()
