#!/usr/bin/env python3
"""Repair function boundaries in config/symbols.toml.

Two defects in Ghidra's export, both of which corrupt the recompiled stack:

  1. A "function" starts at an internal branch target of a real function, so
     that function's prologue and epilogue land in different C functions and
     $sp is never restored.
  2. A real function start (a jal target) is swallowed by a neighbouring
     symbol's range.

The tool alternates two operations until the table is stable:
  * split any symbol that contains a jal target;
  * merge a symbol into its predecessor when the predecessor does not end in a
    jump, or when the symbol's start is a branch target of the predecessor.
Merges never cross a jal target.

Usage:
    tools/fix_symbols.py            # report only
    tools/fix_symbols.py --apply    # rewrite config/symbols.toml
"""
import re
import sys
from pathlib import Path
import capstone

ROOT = Path(__file__).resolve().parent.parent
CODE_VRAM = 0x80000400
CODE_ROM = 0x1000
ROM = ROOT / "roms" / "Mace - The Dark Age (USA).z64"
SYMBOLS = ROOT / "config" / "symbols.toml"

md = capstone.Cs(capstone.CS_ARCH_MIPS, capstone.CS_MODE_MIPS32 + capstone.CS_MODE_BIG_ENDIAN)
md.detail = False

JUMPS = {"j", "jr", "jal", "jalr"}
MAX_GAP = 0x20


def is_jump(insn):
    m = insn.mnemonic
    return m in JUMPS or m.startswith("b")


def insn_at(rom, addr):
    off = addr - CODE_VRAM + CODE_ROM
    for insn in md.disasm(rom[off:off + 4], addr):
        return insn
    return None


def target_of(insn):
    if insn.mnemonic in ("jalr", "jr"):
        return None
    op = insn.op_str.strip()
    if not op:
        return None
    last = op.split(",")[-1].strip()
    if re.fullmatch(r"0x[0-9a-fA-F]+", last):
        return int(last, 16)
    return None


class Func:
    __slots__ = ("name", "vram", "size")

    def __init__(self, name, vram, size):
        self.name, self.vram, self.size = name, vram, size

    @property
    def end(self):
        return self.vram + self.size


def parse(text):
    out = []
    for m in re.finditer(
        r'\[\[section\.functions\]\]\nname = "([^"]+)"\nvram = (0x[0-9a-fA-F]+)\nsize = (0x[0-9a-fA-F]+)\n?',
        text,
    ):
        out.append(Func(m.group(1), int(m.group(2), 16), int(m.group(3), 16)))
    out.sort(key=lambda f: f.vram)
    return out


def main():
    apply = "--apply" in sys.argv
    rom = ROM.read_bytes()
    text = SYMBOLS.read_text()
    funcs = parse(text)
    first, last = funcs[0].vram, funcs[-1].end

    jal_targets, branch_owner = set(), {}
    for a in range(first, last, 4):
        insn = insn_at(rom, a)
        if insn is None:
            continue
        t = target_of(insn)
        if t is None:
            continue
        if insn.mnemonic in ("jal", "jalr"):
            jal_targets.add(t)

    for i, f in enumerate(funcs):
        for a in range(f.vram, f.end, 4):
            insn = insn_at(rom, a)
            if insn is None:
                continue
            t = target_of(insn)
            if t is not None and (insn.mnemonic == "j" or insn.mnemonic.startswith("b")):
                branch_owner.setdefault(t, i)

    def ends_with_jump(f):
        for a in (f.end - 8, f.end - 4):
            insn = insn_at(rom, a) if a >= f.vram else None
            if insn is not None and is_jump(insn):
                return True
        return False

    splits = merges = 0
    changed = True
    rounds = 0
    while changed and rounds < 50:
        changed = False
        rounds += 1

        # 1. split at swallowed jal targets
        for i, f in enumerate(funcs):
            inner = sorted(t for t in jal_targets if f.vram < t < f.end)
            if not inner:
                continue
            cut = inner[0]
            print(f"split {f.name} at {cut:08x}")
            funcs.insert(i + 1, Func(f"func_{cut:08x}", cut, f.end - cut))
            f.size = cut - f.vram
            splits += 1
            changed = True
            break
        if changed:
            continue

        # 2. merge false fragments
        for i in range(1, len(funcs)):
            prev, cur = funcs[i - 1], funcs[i]
            if cur.vram in jal_targets:
                continue
            gap = cur.vram - prev.end
            if gap < 0 or gap > MAX_GAP:
                continue
            is_internal = branch_owner.get(cur.vram) == i - 1
            prev_falls = not ends_with_jump(prev)
            if not (is_internal or prev_falls):
                continue
            if any(prev.vram < t < cur.end for t in jal_targets):
                continue
            print(
                f"merge {cur.name} ({cur.vram:08x}+{cur.size:#x}) into {prev.name}"
                f" [{'branch target' if is_internal else 'no terminator'}]"
            )
            prev.size = cur.end - prev.vram
            del funcs[i]
            merges += 1
            changed = True
            break

    print(f"\n{splits} splits, {merges} merges, {len(funcs)} functions remain", file=sys.stderr)

    if apply and (splits or merges):
        SYMBOLS.with_suffix(".toml.bak").write_text(text)
        head = text.split("[[section.functions]]")[0]
        body = "".join(
            f'[[section.functions]]\nname = "{f.name}"\nvram = 0x{f.vram:08x}\nsize = 0x{f.size:x}\n\n'
            for f in funcs
        )
        SYMBOLS.write_text(head + body)
        print(f"wrote {SYMBOLS}", file=sys.stderr)


main()
