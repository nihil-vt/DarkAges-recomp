#!/usr/bin/env python3
"""Merge Ghidra function fragments back into the function they continue.

Ghidra sometimes splits one function where the code just continues. Left
split, the recompiler either sees a branch into the middle of another
function ("Unhandled branch") or treats the fragment as a separate function,
which leaves the stack frame of the real function unbalanced.

A fragment merges into the function before it when it is contiguous (bytes
no symbol covers may sit in between, if they hold no prologue), has no
prologue, is not a `jal` target, and is not a stubbed or ignored function in
mace.toml. Fragments that a branch from outside reaches mid-body are forced
to merge too.

Usage: merge_fragments.py [--apply]
Without --apply it only reports. With --apply it rewrites config/symbols.toml
(the previous file is kept as config/symbols.toml.bak).
"""

import re
import struct
import sys
from pathlib import Path

from capstone import Cs, CS_ARCH_MIPS, CS_MODE_MIPS32, CS_MODE_BIG_ENDIAN

ROOT = Path(__file__).resolve().parent.parent
ROM = ROOT / "roms" / "Mace - The Dark Age (USA).z64"
SYMS = ROOT / "config" / "symbols.toml"
CODE_START, CODE_END = 0x1000, 0x7D600
VRAM_OFFSET = 0x7FFFF400  # vram = cart file offset + VRAM_OFFSET
MAX_PASSES = 20
# The IPL3 boot section mixes code and data; leave its Ghidra splits alone.
BOOT_END = 0x80001000


def main():
    apply = "--apply" in sys.argv
    rom = ROM.read_bytes()
    code = rom[CODE_START:CODE_END]
    text = SYMS.read_text()
    entries = sorted(
        (int(m.group(2), 16), int(m.group(3), 16), m.group(1))
        for m in re.finditer(r'name = "([^"]+)"\s*\nvram = (0x[0-9a-fA-F]+)\s*\nsize = (0x[0-9a-fA-F]+)', text)
    )
    config = (ROOT / "mace.toml").read_text()
    barriers = set(re.findall(r"func_[0-9a-fA-F]{8}", config))

    def word(va):
        return struct.unpack(">I", rom[va - VRAM_OFFSET:va - VRAM_OFFSET + 4])[0]

    def prologue(va):
        return (word(va) >> 16) == 0x27BD and bool(word(va) & 0x8000)

    md = Cs(CS_ARCH_MIPS, CS_MODE_MIPS32 | CS_MODE_BIG_ENDIAN)
    jal_targets = set()
    branches = []  # (source, target)
    for ins in md.disasm(code, CODE_START + VRAM_OFFSET):
        if ins.mnemonic == "jal" or ins.mnemonic == "j" or ins.mnemonic.startswith("b"):
            try:
                target = int(ins.op_str.split(",")[-1].strip(), 16)
            except ValueError:
                continue
            if ins.mnemonic == "jal":
                jal_targets.add(target)
            else:
                branches.append((ins.address, target))

    starts = [e[0] for e in entries]

    def layout(forced):
        """Return (owner name for each original entry, new size per owner)."""
        # Bytes between two symbols that hold no prologue belong to the
        # function before them, so a branch into such a gap stays inside it.
        for k in range(len(entries) - 1):
            vram, size, name = entries[k]
            nxt = entries[k + 1][0]
            if vram >= BOOT_END and vram + size < nxt and all(not prologue(a) for a in range(vram + size, nxt, 4)):
                entries[k] = (vram, nxt - vram, name)
        owner = entries[0]
        merged = {}
        sizes = {}
        for vram, size, name in entries[1:]:
            o_vram, o_size, o_name = owner
            end = sizes.get(o_name, o_size) + o_vram
            gap_ok = end <= vram and all(not prologue(a) for a in range(end, vram, 4))
            can = (
                vram >= BOOT_END
                and gap_ok
                and not prologue(vram)
                and vram not in jal_targets
                and name not in barriers
                and o_name not in barriers
            )
            if can and (name in forced or all(o_vram <= a < vram for a, t in branches if t == vram)):
                merged[name] = o_name
                sizes[o_name] = vram + size - o_vram
                owner = (o_vram, sizes[o_name], o_name)
            else:
                owner = (vram, size, name)
        return merged, sizes

    def ranges(merged, sizes):
        """Map each remaining function start to its end address."""
        out = {}
        for vram, size, name in entries:
            if name in merged:
                continue
            out[vram] = vram + sizes.get(name, size)
        return out

    forced = set()
    for _ in range(MAX_PASSES):
        merged, sizes = layout(forced)
        rng = ranges(merged, sizes)
        starts_now = sorted(rng)
        new_forced = set()
        for src, tgt in branches:
            # Source function: the start at or below src.
            s_start = max((s for s in starts_now if s <= src), default=None)
            if s_start is None or src >= rng[s_start]:
                continue
            if s_start <= tgt < rng[s_start]:
                continue
            if tgt in rng:
                continue  # tail call into a function start
            # Target is mid-function in a different function.
            t_start = max((s for s in starts_now if s <= tgt), default=None)
            if t_start is None or tgt >= rng[t_start]:
                continue
            # Only fragments that directly follow the source function can merge.
            fragment = [e[2] for e in entries if e[0] == t_start]
            if fragment and t_start == rng[s_start] and fragment[0] not in barriers:
                new_forced.add(fragment[0])
        if not new_forced - forced:
            break
        forced |= new_forced
    else:
        print("warning: merge did not settle", file=sys.stderr)

    merged, sizes = layout(forced)
    print(f"fragments to merge: {len(merged)} of {len(entries)} functions")
    if not apply:
        return 0

    SYMS.with_suffix(".toml.bak").write_text(text)
    lines = text.split("\n")
    out = []
    i = 0
    while i < len(lines):
        if lines[i] == "[[section.functions]]":
            j = i + 1
            while j < len(lines) and lines[j] != "" and not lines[j].startswith("["):
                j += 1
            block = lines[i:j]
            name = re.search(r'name = "([^"]+)"', "\n".join(block)).group(1)
            i = j
            if name in merged:
                continue
            if name in sizes:
                block = [re.sub(r"size = 0x[0-9a-fA-F]+", f"size = 0x{sizes[name]:x}", b) for b in block]
            out.extend(block)
            continue
        out.append(lines[i])
        i += 1
    SYMS.write_text("\n".join(out))
    print(f"wrote {SYMS}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
