#!/usr/bin/env python3
"""Validate indirect-jump tables in the N64 ROM.

For every `jr $rt`, find the `lw $rt, off($rn)` that most recently set $rt and
the `lui $rn` that set $rn, then check whether the ROM holds a plausible jump
table at the resulting address.
"""

import struct
import sys
from pathlib import Path

ROM = Path(__file__).resolve().parent.parent / "roms" / "Mace - The Dark Age (USA).z64"
CODE_ROM = 0x1000
CODE_END = 0x7E200
VRAM = 0x80000000


def main():
    rom = ROM.read_bytes()
    n = (CODE_END - CODE_ROM) // 4
    words = struct.unpack(f">{n}I", rom[CODE_ROM:CODE_END])

    good, bad = [], []
    for i, w in enumerate(words):
        if w >> 26 != 0 or (w & 0x3F) != 8:
            continue
        rt = (w >> 21) & 0x1F
        if rt == 31:
            continue
        # Last lw that wrote $rt before this instruction.
        lw_i = None
        for j in range(i - 1, max(0, i - 40), -1):
            lw = words[j]
            if lw >> 26 == 0x23 and ((lw >> 16) & 0x1F) == rt:
                lw_i = j
                break
            if lw >> 26 == 0x0F and ((lw >> 16) & 0x1F) == rt:
                break  # $rt was redefined, not loaded
        if lw_i is None:
            continue
        lw = words[lw_i]
        rn = (lw >> 21) & 0x1F
        off = lw & 0xFFFF
        if off & 0x8000:
            off -= 0x10000
        # Last lui that wrote $rn before the lw.
        lui_i = None
        for j in range(lw_i - 1, max(0, lw_i - 40), -1):
            lui = words[j]
            if lui >> 26 == 0x0F and ((lui >> 16) & 0x1F) == rn:
                lui_i = j
                break
        if lui_i is None:
            continue
        lui = words[lui_i]
        table = ((lui & 0xFFFF) << 16) + off
        rom_off = table - VRAM
        if rom_off < 0 or rom_off + 8 > len(rom):
            bad.append((CODE_ROM + i * 4, table, "outside rom"))
            continue
        entries = 0
        for k in range(64):
            if rom_off + k * 4 + 4 > len(rom):
                break
            v = struct.unpack_from(">I", rom, rom_off + k * 4)[0]
            if VRAM + CODE_ROM <= v < VRAM + CODE_END:
                entries += 1
            else:
                break
        if entries >= 2:
            good.append((CODE_ROM + i * 4, table, entries))
        else:
            bad.append((CODE_ROM + i * 4, table, f"{entries} code entries"))

    print(f"indirect jumps: {len(good) + len(bad)}   with table: {len(good)}   without: {len(bad)}")
    for a, t, why in bad:
        print(f"  jr at rom {a:#08x} -> table {t:#010x} (rom {t - VRAM:#08x}): {why}")


if __name__ == "__main__":
    main()
