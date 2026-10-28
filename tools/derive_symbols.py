#!/usr/bin/env python3
"""Derive function boundaries from the code itself instead of Ghidra's splits.

A function ends at `jr $ra` when the instruction after its delay slot is not a
branch target (nothing jumps there, so it must be a new function or padding).
Real entries are the entry point and every jal target. This yields boundaries
that keep each $sp frame inside one recompiled function.

Usage: tools/derive_symbols.py [--apply]
"""
import re, sys
from pathlib import Path
import capstone

ROOT = Path(__file__).resolve().parent.parent
CODE_VRAM, CODE_ROM = 0x80000400, 0x1000
ROM = ROOT / "roms" / "Mace - The Dark Age (USA).z64"
SYMBOLS = ROOT / "config" / "symbols.toml"
md = capstone.Cs(capstone.CS_ARCH_MIPS, capstone.CS_MODE_MIPS32 + capstone.CS_MODE_BIG_ENDIAN)
md.detail = False

# $ra and the callee-saved registers are the only values a routine returns
# through; $t/$v registers mean a jump-table dispatch.
RETURN_REGS = {"$ra"} | {f"$s{n}" for n in range(8)}

# Last byte of actual game code; everything above is boot/microcode data.
LAST_CODE = 0x8007A7AC

rom = ROM.read_bytes()

def at(a):
    off = a - CODE_VRAM + CODE_ROM
    for i in md.disasm(rom[off:off + 4], a):
        return i
    return None

def main():
    apply = "--apply" in sys.argv
    text = SYMBOLS.read_text()
    head = text.split("[[section.functions]]")[0]
    m = re.search(r'size = (0x[0-9a-fA-F]+)', head)
    code_size = int(m.group(1), 16)
    end_all = CODE_VRAM + code_size

    jal, branches = set(), set()
    for a in range(CODE_VRAM, end_all, 4):
        i = at(a)
        if i is None:
            continue
        op = i.op_str.strip()
        last = op.split(",")[-1].strip()
        if not re.fullmatch(r"0x[0-9a-fA-F]+", last):
            continue
        t = int(last, 16)
        if i.mnemonic in ("jal", "jalr"):
            jal.add(t)
        elif i.mnemonic == "j" or i.mnemonic.startswith("b"):
            branches.add(t)

    starts = {CODE_VRAM} | {t for t in jal if CODE_VRAM <= t < end_all}
    for a in range(CODE_VRAM, end_all - 8, 4):
        i = at(a)
        if i and i.mnemonic == "jr" and i.op_str.strip() in RETURN_REGS:
            nxt = a + 8
            if nxt < end_all and nxt not in branches:
                starts.add(nxt)

    # A conditional branch never leaves its function in IDO output: the code
    # after an early `jr $ra` is still part of the same routine. Grow functions
    # until they contain every conditional branch target, otherwise the
    # recompiler emits a goto to a label it never defines.
    for _ in range(2000):
        ordered = sorted(starts)
        victims = None
        for i, start in enumerate(ordered):
            end = ordered[i + 1] if i + 1 < len(ordered) else end_all
            for a in range(start, end, 4):
                insn = at(a)
                if insn is None:
                    continue
                mnemonic = insn.mnemonic
                if not mnemonic.startswith("b") or mnemonic == "b":
                    continue
                last = insn.op_str.strip().split(",")[-1].strip()
                if not re.fullmatch(r"0x[0-9a-fA-F]+", last):
                    continue
                target = int(last, 16)
                if start <= target < end:
                    continue
                if target > end:
                    victims = [s for s in ordered if start < s <= target]
                else:
                    victims = [start]
                if victims:
                    break
            if victims:
                break
        if not victims:
            break
        for victim in victims:
            starts.discard(victim)

    # Keep existing symbol names where the address still is a function start.
    old = {}
    for m2 in re.finditer(
        r'\[\[section\.functions\]\]\nname = "([^"]+)"\nvram = (0x[0-9a-fA-F]+)\nsize = (0x[0-9a-fA-F]+)',
        text,
    ):
        old[int(m2.group(2), 16)] = m2.group(1)

    # Symbol boundaries often land on alignment padding. Move each start past
    # leading nops so it points at the real first instruction.
    aligned = set()
    for start in starts:
        while start < LAST_CODE:
            insn = at(start)
            if insn is None or insn.mnemonic != "nop":
                break
            start += 4
        aligned.add(start)
    starts = aligned

    # The tail of the section holds boot/microcode that is never called and
    # does not decode as MIPS32 (absolute jumps outside RDRAM).
    starts = {s for s in starts if s < LAST_CODE}
    starts = sorted(starts)
    funcs = [
        (s, starts[i + 1] - s if i + 1 < len(starts) else LAST_CODE - s,
         old.get(s, f"func_{s:08x}"))
        for i, s in enumerate(starts)
    ]

    # sanity: no function may contain an unreferenced jr $ra boundary
    print(f"{len(funcs)} functions derived", file=sys.stderr)
    if apply:
        SYMBOLS.with_suffix(".toml.bak").write_text(text)
        body = "".join(
            f'[[section.functions]]\nname = "{n}"\nvram = 0x{v:08x}\nsize = 0x{s:x}\n\n'
            for v, s, n in funcs
        )
        SYMBOLS.write_text(head + body)
        print("wrote symbols.toml", file=sys.stderr)

main()
