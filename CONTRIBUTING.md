# Contributing

## Building

```bash
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

The recompiler only needs to be re-run when symbols change:

```bash
tools/N64Recomp/build/N64Recomp mace.toml
```

## Project Status

The game boots and runs but the screen stays black due to the threading
problem. See [README.md](README.md) for details.

## Where to Help

### Threading (most impact)

The game uses custom preemptive threading with VI-interrupt-driven context
switching. The recompiler has no interrupt mechanism. Two approaches:

1. **Signal-based**: `SIGALRM` → VI handler, full context save/restore
2. **Per-primitive patching**: hook yield/dispatch/enqueue to runtime scheduler

Files: `src/main.cpp` (search for `mace_thread`, `mace_entry`, VI handler)

### RSP Microcode

The game uploads custom RSP microcode for rendering. Needs either:

- Recompilation with `RSPRecomp` (from Zelda64Recomp)
- A generic RSP interpreter

Files: `src/mace_rt64.cpp` (SP register emulation)

### Stubbed Functions

434 functions are stubbed because Ghidra cannot resolve their switch tables.
These need manual reimplementation or symbol boundary fixes.

Tools: `tools/fix_symbols.py`, `tools/derive_symbols.py`

### Symbol Recovery

Many Ghidra-split functions have incorrect boundaries. The game uses a
custom VFS with `.rom` blobs that confuse static analysis.

Tools: `tools/check_symbols.py`, `tools/disasm.py`, `tools/fix_symbols.py`

## Code Style

- C++20, 4-space indent
- Match the surrounding code style
- Keep changes minimal and focused

## Submitting Changes

1. Fork the repo
2. Create a feature branch
3. Make your changes
4. Test that the build still succeeds
5. Open a pull request describing what you changed and why

For large changes, open an issue first to discuss the approach.

## Development Tips

- Enable trace mode in `mace.toml` (`trace_mode = true`) to see function calls
- Use `tools/disasm.py <addr> <end>` to disassemble ROM regions
- The task table is dumped to stderr after boot
- Run with `MACE_HEADLESS=1` to suppress the window
