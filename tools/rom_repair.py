#!/usr/bin/env python3
"""Try to reverse the previous session's ROM patch by matching the header CRC.

The ROM on disk does not match its own header CRC1/CRC2, and every indirect
jump table in the code is stale by 0xC00 bytes. This script tries a few
reconstructions and reports which one reproduces the header CRC.
"""

import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import rom_crc

ROM = Path(__file__).resolve().parent.parent / "roms" / "Mace - The Dark Age (USA).z64"
DELTA = 0xC00
CRC_END = 0x101000
SIZE = 0xC00000


def reconstruct(rom, code_swap=None, data_x=None):
    """code_swap=(a,b) swaps two code blocks; data_x moves [data_x, end) down by DELTA."""
    out = bytearray(rom)
    if code_swap:
        a, b = code_swap
        n = b - a
        out[a:b] = rom[b:b + n]
        out[b:b + n] = rom[a:b]
    if data_x is not None:
        tail = bytes(out[SIZE - DELTA:SIZE])
        out = bytearray(out[:data_x]) + bytearray(out[data_x + DELTA:SIZE]) + bytearray(tail)
    return bytes(out)


def check(rom, label):
    # The boot code was patched too, so CIC detection is unreliable: try every seed.
    hdr1, hdr2 = struct.unpack_from(">II", rom, 0x10)
    for cic in (6101, 6102, 6103, 6105, 6106):
        c1, c2 = rom_crc.calc_crc(rom, cic)
        if c1 == hdr1 and c2 == hdr2:
            print(f"{label:52} MATCH with CIC {cic}")
            return True
    print(f"{label:52} no match (crc1 6102={rom_crc.calc_crc(rom, 6102)[0]:08X} vs {hdr1:08X})")
    return False


def main():
    rom = ROM.read_bytes()
    variants = [
        ("as-is", {}),
        ("data shift X=0x896F0", {"data_x": 0x896F0}),
        ("data shift X=0x894D8", {"data_x": 0x894D8}),
        ("code swap 15900/16500", {"code_swap": (0x15900, 0x16500)}),
        ("code swap + data shift X=0x896F0", {"code_swap": (0x15900, 0x16500), "data_x": 0x896F0}),
        ("code swap + data shift X=0x894D8", {"code_swap": (0x15900, 0x16500), "data_x": 0x894D8}),
        ("code swap + data shift X=0x88AF0", {"code_swap": (0x15900, 0x16500), "data_x": 0x88AF0}),
    ]
    for label, kw in variants:
        check(reconstruct(rom, **kw), label)


if __name__ == "__main__":
    main()
