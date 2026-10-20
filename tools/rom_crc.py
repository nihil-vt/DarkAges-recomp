#!/usr/bin/env python3
"""Verify the N64 header CRC1/CRC2 of a ROM (n64dev n64crc algorithm, 1MB range)."""

import struct
import sys
from pathlib import Path

CHECKSUM_START = 0x1000
CHECKSUM_LENGTH = 0x100000
SEEDS = {
    6101: 0xF8CA4DDC,
    6102: 0xF8CA4DDC,
    6103: 0xA3886759,
    6105: 0xDF26F436,
    6106: 0x1FEA617A,
}


def crc32(data):
    poly = 0xEDB88320
    table = []
    for i in range(256):
        c = i
        for _ in range(8):
            c = (c >> 1) ^ poly if c & 1 else c >> 1
        table.append(c)
    crc = 0xFFFFFFFF
    for b in data:
        crc = (crc >> 8) ^ table[(crc ^ b) & 0xFF]
    return ~crc & 0xFFFFFFFF


def rol(v, b):
    b &= 31
    return ((v << b) | (v >> (32 - b))) & 0xFFFFFFFF


def get_cic(data):
    return {
        0x6170A4A1: 6101,
        0x90BB6CB5: 6102,
        0x0B050EE0: 6103,
        0x98BC2C86: 6105,
        0xACC8580A: 6106,
    }.get(crc32(data[0x40:0x1000]), 6105)


def calc_crc(data, bootcode):
    seed = SEEDS[bootcode]
    t1 = t2 = t3 = t4 = t5 = t6 = seed
    for i in range(CHECKSUM_START, CHECKSUM_START + CHECKSUM_LENGTH, 4):
        d = struct.unpack_from(">I", data, i)[0]
        if (t6 + d) < t6:
            t4 += 1
        t6 = (t6 + d) & 0xFFFFFFFF
        t3 ^= d
        r = rol(d, d & 0x1F)
        t5 = (t5 + r) & 0xFFFFFFFF
        if t2 > d:
            t2 ^= r
        else:
            t2 ^= t6 ^ d
        t2 &= 0xFFFFFFFF
        if bootcode == 6105:
            t1 = (t1 + struct.unpack_from(">I", data, 0x40 + 0x710 + (i & 0xFF))[0] ^ d) & 0xFFFFFFFF
        else:
            t1 = (t1 + t5 ^ d) & 0xFFFFFFFF
    if bootcode == 6103:
        return ((t6 ^ t4) + t3) & 0xFFFFFFFF, ((t5 ^ t2) + t1) & 0xFFFFFFFF
    if bootcode == 6106:
        return ((t6 * t4) + t3) & 0xFFFFFFFF, ((t5 * t2) + t1) & 0xFFFFFFFF
    return (t6 ^ t4 ^ t3) & 0xFFFFFFFF, (t5 ^ t2 ^ t1) & 0xFFFFFFFF


def main():
    path = Path(sys.argv[1])
    data = path.read_bytes()
    cic = get_cic(data)
    crc1, crc2 = calc_crc(data, cic)
    hdr1, hdr2 = struct.unpack_from(">II", data, 0x10)
    print(f"file:     {path}")
    print(f"cic:      {cic}")
    print(f"crc1:     header 0x{hdr1:08X}  calculated 0x{crc1:08X}  {'OK' if hdr1 == crc1 else 'MISMATCH'}")
    print(f"crc2:     header 0x{hdr2:08X}  calculated 0x{crc2:08X}  {'OK' if hdr2 == crc2 else 'MISMATCH'}")
    return 0 if (hdr1 == crc1 and hdr2 == crc2) else 1


if __name__ == "__main__":
    sys.exit(main())
