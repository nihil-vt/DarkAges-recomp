#!/usr/bin/env python3
"""Locate the byte range that a previous session modified in the ROM.

Hypothesis: the current ROM is the original ROM with 0xC00 bytes inserted at
some offset X (the file length was kept by dropping the trailing padding).

The N64 header CRC1 is `t6 ^ t4 ^ t3` where t6 is the sum of all 32-bit words
in 0x1000..0x101000, t3 is their XOR and t4 is the number of 32-bit carries
during the accumulation.  t4 is a small number (< 2^18), so for a candidate X
we can solve for t4 and reject the candidate unless the result is plausible.
"""

import struct
import sys
from itertools import accumulate
from pathlib import Path

ROM = Path(__file__).resolve().parent.parent / "roms" / "Mace - The Dark Age (USA).z64"
DELTA = 0xC00
START = 0x1000
END = 0x101000


def main():
    raw = ROM.read_bytes()
    hdr1, hdr2 = struct.unpack_from(">II", raw, 0x10)

    words = struct.unpack(f">{len(raw) // 4}I", raw)
    psum = [0] + [s & 0xFFFFFFFF for s in accumulate(words)]
    pxor = [0] + list(accumulate(words, lambda a, b: a ^ b))

    def wsum(a, b):  # word range [a, b)
        return (psum[b] - psum[a]) & 0xFFFFFFFF

    def wxor(a, b):
        return pxor[b] ^ pxor[a]

    s0, s1 = START // 4, END // 4
    print(f"current t6={wsum(s0, s1):#010x} t3={wxor(s0, s1):#010x}  header crc1={hdr1:#010x}")

    hits = 0
    candidates = []
    for x in range(START, 0x896F0, 4):
        wx = x // 4
        new_sum = (wsum(s0, wx) + wsum(wx + DELTA // 4, s1)) & 0xFFFFFFFF
        new_xor = wxor(s0, wx) ^ wxor(wx + DELTA // 4, s1)
        t4 = hdr1 ^ new_sum ^ new_xor
        if t4 < (1 << 18):
            hits += 1
            candidates.append(x)
            print(f"candidate X={x:#08x}: t6={new_sum:#010x} t3={new_xor:#010x} t4={t4}")

    # Confirm with the order sensitive CRC2.
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    import rom_crc
    for x in candidates:
        recon = raw[:x] + raw[x + DELTA:] + raw[len(raw) - DELTA:]
        for cic in (6101, 6102, 6103, 6105, 6106):
            c1, c2 = rom_crc.calc_crc(recon, cic)
            if c1 == hdr1 and c2 == hdr2:
                print(f"MATCH: X={x:#08x} CIC={cic}")
                out = Path(__file__).resolve().parent.parent / "roms" / "Mace - The Dark Age (USA).repaired.z64"
                out.write_bytes(recon)
                print(f"wrote {out}")
                return 0
    if not hits:
        print("no candidate found")
    return 0


if __name__ == "__main__":
    sys.exit(main())
