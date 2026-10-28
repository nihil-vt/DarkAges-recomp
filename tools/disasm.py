#!/usr/bin/env python3
"""Disassemble the ROM's code section by N64 virtual address.

file offset = vram - 0x80000400 + 0x1000
Usage: tools/disasm.py <start_vram> [end_vram]
"""
import sys
from pathlib import Path
import capstone

CODE_VRAM = 0x80000400
CODE_ROM = 0x1000
ROM = Path(__file__).resolve().parent.parent / "roms" / "Mace - The Dark Age (USA).z64"

md = capstone.Cs(capstone.CS_ARCH_MIPS, capstone.CS_MODE_MIPS32 + capstone.CS_MODE_BIG_ENDIAN)

def main():
    start = int(sys.argv[1], 0)
    end = int(sys.argv[2], 0) if len(sys.argv) > 2 else start + 0x100
    data = ROM.read_bytes()
    off = start - CODE_VRAM + CODE_ROM
    blob = data[off:off + (end - start)]
    for insn in md.disasm(blob, start):
        print(f"{insn.address:08x}: {insn.bytes.hex():8s} {insn.mnemonic:8s} {insn.op_str}")

main()
