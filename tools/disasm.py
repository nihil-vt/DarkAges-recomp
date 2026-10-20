#!/usr/bin/env python3
"""Disassemble a VRAM range of the N64 ROM (big-endian MIPS3) with capstone."""

import sys
from pathlib import Path

from capstone import Cs, CS_ARCH_MIPS, CS_MODE_MIPS32, CS_MODE_BIG_ENDIAN

ROM = Path(__file__).resolve().parent.parent / "roms" / "Mace - The Dark Age (USA).z64"


def disasm(vram_start, vram_end):
    rom = ROM.read_bytes()
    md = Cs(CS_ARCH_MIPS, CS_MODE_MIPS32 | CS_MODE_BIG_ENDIAN)
    # ROM offset = vram - 0x80000000
    offset = vram_start - 0x80000000
    data = rom[offset:offset + (vram_end - vram_start)]
    for insn in md.disasm(data, vram_start):
        print(f"  {insn.address:#010x}: {insn.mnemonic:10} {insn.op_str}")


if __name__ == "__main__":
    start = int(sys.argv[1], 16)
    end = int(sys.argv[2], 16)
    disasm(start, end)
