#!/usr/bin/env python3
"""Repair function boundaries in config/symbols.toml.

N64Recomp needs every direct branch target to be either inside the current
function or at the start of a known function. Ghidra occasionally mis-splits
functions, so we split the containing function at the offending target and
re-run the recompiler until it is happy.

Usage: fix_symbols.py <symbols.toml> <recomp binary> <recomp config> [--apply]
"""

import re
import subprocess
import sys
from pathlib import Path


class Section:
    def __init__(self, name, rom, vram, size):
        self.name = name
        self.rom = rom
        self.vram = vram
        self.size = size
        self.funcs = []  # list of [name, vram, size]


def parse(path):
    lines = Path(path).read_text().splitlines()
    sections = []
    i = 0
    while i < len(lines):
        line = lines[i].strip()
        if line == "[[section]]":
            i += 1
            fields = {}
            while i < len(lines) and lines[i].strip() and not lines[i].strip().startswith("["):
                key, val = lines[i].split("=", 1)
                val = val.strip()
                fields[key.strip()] = int(val, 16) if val.startswith("0x") else val.strip('"')
                i += 1
            sec = Section(fields["name"], fields["rom"], fields["vram"], fields["size"])
            # Read the functions of this section.
            while i < len(lines):
                if lines[i].strip() == "[[section.functions]]":
                    i += 1
                    f = {}
                    while i < len(lines) and lines[i].strip() and not lines[i].strip().startswith("["):
                        key, val = lines[i].split("=", 1)
                        val = val.strip()
                        f[key.strip()] = int(val, 16) if val.startswith("0x") else val.strip('"')
                        i += 1
                    sec.funcs.append([f["name"], f["vram"], f["size"]])
                elif lines[i].strip() == "[[section]]":
                    break
                else:
                    i += 1
            sections.append(sec)
        else:
            i += 1
    return sections


def write(path, sections):
    out = ["# Auto-generated"]
    for s in sections:
        out += ["[[section]]", f'name = "{s.name}"', f"rom = 0x{s.rom:x}",
                f"vram = 0x{s.vram:x}", f"size = 0x{s.size:x}"]
        for name, vram, size in sorted(s.funcs, key=lambda f: f[1]):
            out += ["", "[[section.functions]]", f'name = "{name}"',
                    f"vram = 0x{vram:x}", f"size = 0x{size:x}"]
        out.append("")
    Path(path).write_text("\n".join(out) + "\n")


def find_container(sections, addr):
    for sec in sections:
        for name, vram, size in sec.funcs:
            if vram < addr < vram + size:
                return (name, vram, size, sec)
    return None


def find_owner(sections, addr):
    """The function that contains addr, or the one that ends exactly at addr."""
    for sec in sections:
        for name, vram, size in sec.funcs:
            if vram <= addr < vram + size:
                return (name, vram, size, sec)
    return None


def find_gap_owner(sections, addr):
    """The function whose end is exactly addr (addr is a missed function start)."""
    for sec in sections:
        for name, vram, size in sec.funcs:
            if vram + size == addr:
                return (name, vram, size, sec)
    return None


def jal_targets(rom):
    """All addresses that are the target of a jal instruction in the ROM."""
    targets = set()
    for off in range(0x1000, 0x7D600, 4):
        word = int.from_bytes(rom[off:off + 4], "big")
        if word >> 26 == 3:
            imm = word & 0x03FFFFFF
            targets.add(0x80000000 | ((imm << 2) & 0x0FFFFFFF))
    return targets


def merge_functions(sections, addr, branch_func, force=False, renames=None, called=None):
    """Merge the function containing addr with the branching function.

    Ghidra sometimes splits a function in two; a branch from the second half
    back into the first half then lands outside of any function. Merging the
    two restores the original function and keeps jump tables (and their case
    labels) in the same function.
    """
    hit = find_container(sections, addr)
    if hit is None:
        return None
    name, vram, size, sec = hit
    if branch_func is None or branch_func not in sec.funcs:
        return None
    b_name, b_vram, b_size = branch_func
    if b_vram == vram:
        return None
    # A function that is called from elsewhere must keep its own entry point,
    # so it cannot be merged into another one. The lower of the two start
    # addresses survives the merge, so only the higher one may not be called.
    if called is not None and max(vram, b_vram) in called:
        return None
    # Only merge functions that are close to each other, otherwise the merged
    # function would swallow unrelated code.
    if not force:
        if b_vram + b_size < vram and vram - (b_vram + b_size) > 0x40:
            return None
        if vram + size < b_vram and b_vram - (vram + size) > 0x40:
            return None
    # The merged function must not swallow other functions.
    lo, hi = min(vram, b_vram), max(vram + size, b_vram + b_size)
    for other in sec.funcs:
        if other[0] in (name, b_name):
            continue
        if lo < other[1] < hi:
            return None
    if hi - lo > 0x4000:
        return None
    start = min(vram, b_vram)
    end = max(vram + size, b_vram + b_size)
    sec.funcs = [f for f in sec.funcs if f[0] not in (name, b_name)]
    new_name = f"func_{start:08x}"
    sec.funcs.append([new_name, start, end - start])
    sec.funcs.sort(key=lambda f: f[1])
    if renames is not None:
        renames[name] = new_name
        renames[b_name] = new_name
    return new_name


def find_section(sections, addr):
    for sec in sections:
        if sec.vram <= addr < sec.vram + sec.size:
            return sec
    return None


# IDO function prologues: stack frame setup, $gp setup or a saved return address.
PROLOGUES = (
    0x27BD0000,  # addiu $sp, $sp, -imm
    0x3C1C0000,  # lui   $gp, imm
    0xAFBF0000,  # sw    $ra, off($sp)
    0xAFAF0000,  # sw    $t0, off($sp)
    0xAFB00000,  # sw    $s0, off($sp)
)


def is_prologue(word):
    for pat in PROLOGUES:
        if word & 0xFFFF0000 == pat:
            return True
    return False


def decodes_cleanly(rom, start, end):
    """True if the whole range decodes as valid MIPS instructions."""
    try:
        from capstone import Cs, CS_ARCH_MIPS, CS_MODE_MIPS32, CS_MODE_BIG_ENDIAN
    except ImportError:
        return True
    md = Cs(CS_ARCH_MIPS, CS_MODE_MIPS32 | CS_MODE_BIG_ENDIAN)
    data = rom[start - 0x80000000:end - 0x80000000]
    count = 0
    for insn in md.disasm(data, start):
        count += 1
    return count * 4 == len(data)


def fill_gaps(sections, rom, only=None):
    """Create functions for code regions that no known function covers."""
    created = 0
    for sec in sections:
        if only is not None and sec.name not in only:
            continue
        covered = sorted((f[1], f[1] + f[2]) for f in sec.funcs)
        gaps = []
        pos = sec.vram
        for start, end in covered:
            if start > pos:
                gaps.append((pos, start))
            pos = max(pos, end)
        if pos < sec.vram + sec.size:
            gaps.append((pos, sec.vram + sec.size))
        for gstart, gend in gaps:
            if not decodes_cleanly(rom, gstart, gend):
                continue  # data, not code
            # Find function starts inside the gap using IDO prologues.
            starts = [gstart]
            addr = gstart
            while addr + 4 <= gend:
                word = int.from_bytes(rom[addr - 0x80000000:addr - 0x80000000 + 4], "big")
                if is_prologue(word):
                    starts.append(addr)
                addr += 4
            if not starts:
                starts = [gstart]
            for i, fstart in enumerate(starts):
                fend = starts[i + 1] if i + 1 < len(starts) else gend
                sec.funcs.append([f"func_{fstart:08x}", fstart, fend - fstart])
                created += 1
        sec.funcs.sort(key=lambda f: f[1])
    return created


def dedup(sections):
    """Remove duplicate functions and fix up overlapping ones.

    Overlaps are resolved by truncating the earlier function instead of
    dropping it, so that every function start stays reachable.
    """
    for sec in sections:
        seen = set()
        unique = []
        for f in sorted(sec.funcs, key=lambda f: f[1]):
            if f[0] in seen:
                continue
            seen.add(f[0])
            if unique and f[1] < unique[-1][1] + unique[-1][2]:
                # Overlaps the previous function: truncate that one.
                overlap = unique[-1][1] + unique[-1][2] - f[1]
                unique[-1][2] -= overlap
                if unique[-1][2] == 0:
                    unique.pop()
            unique.append(f)
        sec.funcs = unique


def split_at(sections, addr):
    """Split the function containing addr so that addr becomes a function start."""
    hit = find_container(sections, addr)
    if hit is not None:
        name, vram, size, sec = hit
        idx = next(i for i, f in enumerate(sec.funcs) if f[1] == vram and f[0] == name)
        sec.funcs[idx] = [name, vram, addr - vram]
        sec.funcs.append([f"func_{addr:08x}", addr, vram + size - addr])
        sec.funcs.sort(key=lambda f: f[1])
        return True

    # The target is in a gap between known functions: start a new function there.
    sec = find_section(sections, addr)
    if sec is None:
        return False
    nxt = [f[1] for f in sec.funcs if f[1] > addr]
    end = min(nxt) if nxt else sec.vram + sec.size
    sec.funcs.append([f"func_{addr:08x}", addr, end - addr])
    sec.funcs.sort(key=lambda f: f[1])
    return True


def run_recomp(recomp, config, cwd):
    proc = subprocess.run([recomp, config], cwd=cwd, capture_output=True, text=True)
    return proc.returncode, proc.stdout + proc.stderr


TARGET_RE = re.compile(r"Function (\S+) is branching outside of the function \(to (0x[0-9A-Fa-f]+)\)")
UNHANDLED_RE = re.compile(r"Unhandled branch in \S+ at 0x[0-9A-Fa-f]+ to (0x[0-9A-Fa-f]+)")
JTBL_RE = re.compile(r"Failed to determine size of jump table at (0x[0-9A-Fa-f]+) for instruction at (0x[0-9A-Fa-f]+)")
NOJAL_RE = re.compile(r"No function found for jal target: (0x[0-9A-Fa-f]+)")


def read_stubs(config_path):
    text = Path(config_path).read_text()
    m = re.search(r"^stubs = \[(.*?)\]", text, re.S | re.M)
    if not m:
        return text, []
    names = re.findall(r'"([^"]+)"', m.group(1))
    return text, names


def write_stubs(config_path, text, names):
    body = ", ".join(f'"{n}"' for n in names)
    new = re.sub(r"^stubs = \[.*?\]", f"stubs = [{body}]", text, count=1, flags=re.S | re.M)
    Path(config_path).write_text(new)


def containing_func(sections, addr):
    for sec in sections:
        for name, vram, size in sec.funcs:
            if vram <= addr < vram + size:
                return name
    return None


def read_jump_table(rom, vram, max_entries=256):
    """Read the entries of a jump table stored in the ROM at `vram`."""
    offset = vram - 0x80000000
    if offset < 0 or offset + 4 > len(rom):
        return []
    entries = []
    for i in range(max_entries):
        if offset + i * 4 + 4 > len(rom):
            break
        word = int.from_bytes(rom[offset + i * 4:offset + i * 4 + 4], "big")
        if not (0x80001000 <= word < 0x8007E200):
            break
        entries.append(word)
    return entries


def containing_func_entry(sections, addr):
    for sec in sections:
        for f in sec.funcs:
            if f[1] <= addr < f[1] + f[2]:
                return f
    return None


def main():
    if len(sys.argv) < 4:
        print(__doc__)
        return 1
    sym_path, recomp, config = sys.argv[1], sys.argv[2], sys.argv[3]
    apply = "--apply" in sys.argv
    rom_path = None
    if "--rom" in sys.argv:
        rom_path = sys.argv[sys.argv.index("--rom") + 1]
    cwd = Path(config).resolve().parent

    sections = parse(sym_path)
    rom = Path(rom_path).read_bytes() if rom_path else None
    cfg_text, stubs = read_stubs(config)
    renames = {}
    called = jal_targets(rom) if rom is not None else set()
    for i in range(1000):
        dedup(sections)
        if rom is not None:
            # Every call target must be a function, otherwise the recompiler
            # has to invent one and cannot be told to stub it.
            starts = {f[1] for sec in sections for f in sec.funcs}
            added = 0
            for target in sorted(called):
                if 0x80001000 <= target < 0x8007E200 and target not in starts:
                    if split_at(sections, target):
                        added += 1
                        starts.add(target)
            if added:
                print(f"round {i}: added {added} functions for call targets")
                if apply:
                    write(sym_path, sections)
            n = fill_gaps(sections, rom, only=("boot",))
            if n:
                print(f"round {i}: filled {n} gap functions")
                if apply:
                    write(sym_path, sections)
        # Drop stubs for functions that no longer exist (they were merged).
        known = {f[0] for sec in sections for f in sec.funcs}
        filtered = [s for s in stubs if s in known]
        if len(filtered) != len(stubs):
            stubs = filtered
            if apply:
                write_stubs(config, cfg_text, stubs)
        rc, out = run_recomp(recomp, config, cwd)
        targets = set()
        for m in TARGET_RE.finditer(out):
            targets.add((m.group(1), int(m.group(2), 16)))
        for m in UNHANDLED_RE.finditer(out):
            targets.add((None, int(m.group(1), 16)))
        recompiled_ok = rc == 0
        # Functions that cannot be recompiled statically get stubbed.
        new_stubs = []
        merged_cases = 0
        for m in JTBL_RE.finditer(out):
            instr_addr = int(m.group(2), 16)
            table_vram = int(m.group(1), 16)
            dispatch = containing_func_entry(sections, instr_addr)
            if dispatch is None or rom is None:
                continue
            # The case labels of a switch live in the same function as the
            # dispatch. If Ghidra put them elsewhere, merge them back.
            entries = read_jump_table(rom, table_vram)
            if not entries:
                continue
            for entry in entries:
                case_func = containing_func_entry(sections, entry)
                if case_func is None:
                    continue
                # Re-find the dispatch function: merging renames it.
                dispatch = containing_func_entry(sections, instr_addr)
                if dispatch is None or case_func[0] == dispatch[0]:
                    continue
                if merge_functions(sections, entry, dispatch, force=True, renames=renames, called=called):
                    merged_cases += 1
        if merged_cases:
            print(f"round {i}: merged {merged_cases} jump table case labels into their dispatch function")
            if renames:
                stubs = [renames.get(s, s) for s in stubs]
                renames.clear()
            if apply:
                write(sym_path, sections)
                write_stubs(config, cfg_text, stubs)
            continue

        for m in JTBL_RE.finditer(out):
            name = containing_func(sections, int(m.group(2), 16))
            if name and name not in stubs and name not in new_stubs:
                new_stubs.append(name)
        if NOJAL_RE.search(out) or UNHANDLED_RE.search(out) or "Unhandled instruction" in out:
            # The caller is the function the recompiler gave up on.
            m = re.search(r"Error in recompiling (\S+?),", out)
            if m and m.group(1) not in stubs and m.group(1) not in new_stubs:
                new_stubs.append(m.group(1))
        if not targets and not new_stubs and not recompiled_ok:
            print("recomp failed with no fixable branch targets:")
            print("\n".join(out.splitlines()[-25:]))
            return 1
        fixed = 0
        for src, t in sorted(targets, key=lambda p: p[1]):
            # Prefer merging the branching function with the one containing the
            # target: that restores functions that Ghidra split in two and keeps
            # jump tables (and their case labels) in the same function.
            branch_func = None
            if src is not None:
                for sec in sections:
                    for f in sec.funcs:
                        if f[0] == src:
                            branch_func = f
                            break
            if split_at(sections, t):
                fixed += 1
                continue
            merged = merge_functions(sections, t, branch_func, renames=renames, called=called)
            if merged:
                fixed += 1
                continue
            print(f"  target 0x{t:08X} is not inside any known function (needs manual fix)")
            if src is not None and src not in stubs and src not in new_stubs:
                new_stubs.append(src)
        if new_stubs:
            stubs += new_stubs
            print(f"round {i}: stubbing {len(new_stubs)} functions: {new_stubs}")
        print(f"round {i}: {len(targets)} branch targets, {fixed} fixes applied")
        if recompiled_ok and fixed == 0:
            print(f"recomp succeeded after {i} fix rounds")
            return 0
        if renames:
            stubs = [renames.get(s, s) for s in stubs]
            renames.clear()
        if fixed == 0 and not new_stubs:
            print("\n".join(out.splitlines()[-25:]))
            return 1
        if apply:
            write(sym_path, sections)
            write_stubs(config, cfg_text, stubs)
    print("gave up after 1000 rounds")
    return 1


if __name__ == "__main__":
    sys.exit(main())
