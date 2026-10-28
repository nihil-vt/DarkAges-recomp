#!/usr/bin/env python3
"""Wrap the N64 ROM's code section in a big-endian MIPS ELF.

Ghidra (and therefore REA) cannot open a .z64 ROM directly, and a raw binary
gives no load address, so every cross-reference would be relative to 0. This
builds an ELF whose single PT_LOAD segment maps the ROM's code section at its
real runtime address, which is what makes absolute references (jump tables,
string pointers, BSS) resolve correctly.

The N64 boot code copies the cart's 0x1000 onwards to RDRAM at physical 0x400,
which is kseg0 0x80000400 -- the ROM header's entry point.
"""

import struct
import sys
from pathlib import Path

# Cart file offset of the code section and the runtime address it is loaded at.
CODE_ROM = 0x1000
CODE_VRAM = 0x80000400

# ELF constants
ET_EXEC = 2
EM_MIPS = 8
ELFCLASS32 = 1
ELFDATA2MSB = 2
EV_CURRENT = 1
PT_LOAD = 1
SHT_MIPS_ABIFLAGS = 0x7000002A
STRTAB = 3
PF_R, PF_W, PF_X = 4, 2, 1
EHDR_SIZE = 52
PHDR_SIZE = 32
SHDR_SIZE = 40

# EF_MIPS_ARCH_32R2 | EF_MIPS_ABI_O32 | EF_MIPS_CPIC. Note that 0x04000000 is
# EF_MIPS_ASE_M16, not CPIC, and would select the MIPS16 ASE lane.
EF_MIPS = 0x70001004


def shdr(name, sh_type, flags, addr, offset, size, link=0, info=0, align=1, entsize=0):
    return struct.pack(
        ">IIIIIIIIII", name, sh_type, flags, addr, offset, size, link, info, align, entsize
    )


def build(rom: bytes, rom_offset: int, vram: int) -> bytes:
    data = rom[rom_offset:]

    data_off = EHDR_SIZE + PHDR_SIZE
    abiflags_off = data_off + len(data)
    # The MIPS ABI flags record is 24 bytes: version, isa_level, isa_rev,
    # gpr_size, fp_abi (1 = hard float), isa_ext, ases, ases2, then 16 bytes
    # of reserved/padding.
    abiflags = struct.pack(">8B16x", 0, 32, 2, 4, 1, 0, 0, 0)

    shstrtab = b"\0.mips.abiflags\0.shstrtab\0"
    shstr_off = abiflags_off + len(abiflags)
    shoff = shstr_off + len(shstrtab)

    ehdr = struct.pack(
        ">4sBBBBB7xHHIIIIIHHHHHH",
        b"\x7fELF",
        ELFCLASS32,
        ELFDATA2MSB,
        EV_CURRENT,
        0,  # OSABI: System V
        0,  # ABIVERSION
        ET_EXEC,
        EM_MIPS,
        EV_CURRENT,
        vram,  # e_entry
        EHDR_SIZE,  # e_phoff
        shoff,  # e_shoff
        EF_MIPS,  # e_flags
        EHDR_SIZE,
        PHDR_SIZE,
        1,  # e_phnum
        SHDR_SIZE,
        3,  # e_shnum: null, .mips.abiflags, .shstrtab
        2,  # e_shstrndx
    )
    phdr = struct.pack(
        ">IIIIIIII",
        PT_LOAD,
        data_off,
        vram,
        vram,
        len(data),
        len(data),
        PF_R | PF_X,
        0x1000,
    )
    shdrs = b"\0" * SHDR_SIZE
    shdrs += shdr(1, SHT_MIPS_ABIFLAGS, 0x70000012, 0, abiflags_off, len(abiflags), align=16)
    shdrs += shdr(17, STRTAB, 0, 0, shstr_off, len(shstrtab), align=1)

    return ehdr + phdr + data + abiflags + shstrtab + shdrs


def main() -> int:
    rom_path = Path(sys.argv[1])
    out_path = Path(sys.argv[2])
    rom = rom_path.read_bytes()
    out_path.write_bytes(build(rom, CODE_ROM, CODE_VRAM))
    print(f"wrote {out_path} ({out_path.stat().st_size} bytes, entry {CODE_VRAM:#010x})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
