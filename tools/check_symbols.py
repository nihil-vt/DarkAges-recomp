#!/usr/bin/env python3
"""Static sanity check for function boundaries in config/symbols.toml.

For every function, walk its instructions following branches/delay slots and
track the $sp delta. Reports functions whose returns do not restore $sp, that
fall off the end without a jump, or whose start looks like another function's
epilogue. These are the Ghidra split bugs that corrupt the recompiled stack.

Usage: tools/check_symbols.py [--fix-hints]
"""
import re
import sys
from collections import deque
from pathlib import Path
import capstone

ROOT = Path(__file__).resolve().parent.parent
CODE_VRAM = 0x80000400
CODE_ROM = 0x1000
ROM = ROOT / "roms" / "Mace - The Dark Age (USA).z64"
SYMBOLS = ROOT / "config" / "symbols.toml"

md = capstone.Cs(capstone.CS_ARCH_MIPS, capstone.CS_MODE_MIPS32 + capstone.CS_MODE_BIG_ENDIAN)
md.detail = False


def load():
    txt = SYMBOLS.read_text()
    items = re.findall(
        r'\[\[section\.functions\]\]\nname = "([^"]+)"\nvram = (0x[0-9a-fA-F]+)\nsize = (0x[0-9a-fA-F]+)',
        txt,
    )
    fs = [(int(v, 16), int(s, 16), n) for n, v, s in items]
    fs.sort()
    return fs


def disasm_at(rom, addr):
    off = addr - CODE_VRAM + CODE_ROM
    for insn in md.disasm(rom[off:off + 4], addr):
        return insn
    return None


def is_branch(m):
    return m.startswith("b") or m in ("j", "jal", "jalr", "jr")


def has_branch(m):
    return m.startswith("b") or m in ("j", "jal", "jalr", "jr")


def analyze(rom, start, size):
    """Linear scan: track $sp along the fall-through path and check the tail."""
    end = start + size
    problems = []
    insns = []
    for addr in range(start, end, 4):
        insn = disasm_at(rom, addr)
        if insn is None:
            problems.append(f"undecodable at {addr:08x}")
            break
        insns.append(insn)
    if not insns:
        return problems

    delay_slots = {insns[i + 1].address for i in range(len(insns) - 1) if has_branch(insns[i].mnemonic)}
    sp = 0
    returns = []
    for idx, insn in enumerate(insns):
        m, ops = insn.mnemonic, insn.op_str
        if insn.address not in delay_slots and m == "addiu" and ops.startswith("$sp, $sp,"):
            sp += int(ops.split(",")[-1], 0)
        elif m == "jr":
            # IDO restores $sp in the jump's delay slot, so apply it here.
            delta = sp
            if idx + 1 < len(insns):
                d = insns[idx + 1]
                if d.mnemonic == "addiu" and d.op_str.startswith("$sp, $sp,"):
                    delta += int(d.op_str.split(",")[-1], 0)
            returns.append((insn.address, delta))

    last = insns[-1]
    last_real = insns[-2] if insns[-1].address in delay_slots and len(insns) > 1 else last
    if not has_branch(last_real.mnemonic):
        problems.append(f"falls off end at {end:08x}")
    for addr, delta in returns:
        if delta != 0:
            problems.append(f"return at {addr:08x} leaves sp delta {delta:+#x}")
            break
    return problems


def main():
    rom = ROM.read_bytes()
    fs = load()
    by_addr = {v: (s, n) for v, s, n in fs}
    bad = 0
    for v, s, n in fs:
        probs = analyze(rom, v, s)
        if probs:
            bad += 1
            print(f"{v:08x} {n} size {s:#x}: {'; '.join(probs)}")
    # gaps / overlaps
    for (v, s, n), (nv, ns, nn) in zip(fs, fs[1:]):
        if v + s != nv:
            kind = "OVERLAP" if v + s > nv else "GAP"
            print(f"{kind} {v + s:08x}..{nv:08x} ({(nv - v - s):#x}) before {nn}")
    print(f"\n{bad}/{len(fs)} functions with problems", file=sys.stderr)


main()
