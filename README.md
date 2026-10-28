# Mace: The Dark Age — PC Recompilation Project

Static recompilation of *Mace: The Dark Age* (N64, USA) targeting native PC
playability with support for texture mods, widescreen, cheats, and other
enhancements.

> **You must source your own ROM.** This project does not distribute game
> assets. Provide a legally-obtained USA copy of *Mace: The Dark Age*
> (`.z64`, big-endian, 12,582,912 bytes) and place it at
> `roms/Mace - The Dark Age (USA).z64`. Update `mace.toml` if you use a
> different path.

## Status

**The game boots and runs. The blocker is threading.**

The static recompiler handles the full code section (~3600 functions), MMIO,
DMA, COP0, and RT64 rendering. The game's boot code executes, tasks are
created, and the emulator runs without crashing. But the screen stays black
because the game's custom preemptive threading system never switches to the
rendering tasks.

| Phase | Status |
|-------|--------|
| 1. ROM analysis & symbol recovery | ✅ Complete |
| 2. Runtime wiring (MMIO, DMA, COP0, task table) | ✅ Complete |
| 3. Graphics rendering (RT64, VI handler) | ✅ Complete |
| 4. Threading (custom preemptive scheduler) | ❌ Blocked |

## Two Approaches

### Approach A: Emulation Wrapper (works today)

A minimal [libretro](https://www.libretro.com/) frontend that loads an N64
core and plays the game. Also includes a RetroArch launcher.

```bash
# Option 1: RetroArch (recommended)
sudo pacman -S retroarch libretro-mupen64plus-next  # or equivalent
./mace.sh "roms/Mace - The Dark Age (USA).z64"

# Option 2: Custom libretro frontend
cmake --build build --target mace-libretro -j$(nproc)
./build/mace-libretro cores/parallel_n64_libretro.so "roms/Mace - The Dark Age (USA).z64"
```

### Approach B: Static Recompilation (in progress)

The end goal. Native code, patchable at compile time, mod-friendly.

```bash
# Build
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)

# Run (requires threaded renderer to be fixed)
./build/mace-recomp "roms/Mace - The Dark Age (USA).z64"
```

## The Threading Problem (HELP WANTED)

The game uses a **custom preemptive threading system** layered on top of
libultra. Unlike the 16+ N64 games that have been successfully recompiled
(Majora's Mask, Star Fox 64, Mario Kart 64, etc.), Mace does NOT use standard
`osCreateThread`/`osStartThread` for its tasks. Instead it has:

- **Custom thread init** (`func_80073880`) and **thread start** (`func_800739D0`)
- **Custom task table** at `0x8007E760` (stride 0x1D0)
- **VI interrupt handler** (`func_80075F20`) as the scheduling tick
- **Priority-based preemption** with full context save/restore

The critical issue: the threading code **assumes MIPS interrupt hardware**
automatically saves and restores all 32 general-purpose registers on the
interrupt stack. The recompiler has no interrupt mechanism, so context
switching never happens.

### What a Fix Requires

Two possible approaches:

1. **Signal-based interrupt simulation**: Use `SIGALRM` to trigger the VI
   handler, with full `recomp_context` save/restore before switching threads.
   This is the "correct" approach but requires careful handling of MIPS COP0
   state, interrupt masks, and the `eret` return path.

2. **Per-primitive patching**: Hook `func_800743EC` (yield), `func_80074544`
   (dispatch), and `func_800744EC` (enqueue) to redirect through the runtime's
   cooperative scheduler. Less faithful but simpler.

Both are significant work. If you have experience with signal-based context
switching, N64 kernel internals, or emulator threading, we need your help.

## Project Structure

```
CMakeLists.txt              Build for the app (RT64 + runtime + recompiled code)
mace.toml                   N64Recomp config (entry point, stubs, patches)
config/symbols.toml         Function map used by the recompiler (~3600 functions)
src/main.cpp                App: runtime startup, MMIO, DMA, task table
src/mace_rt64.cpp           RT64 renderer adapter
src/mace_overlays.cpp       Register recompiled sections with runtime
src/mace_libretro.cpp       Minimal libretro frontend (emulation wrapper)
src/mace_patches.cpp        Native implementations of IPL3/boot functions
tools/                      Build scripts, symbol tools, disassembler
cores/                      Downloaded libretro cores (not in repo)
roms/                       Your ROM (not in repo)
```

## Build Dependencies

- C++20 compiler (GCC 11+, Clang 14+, or MSVC 2022+)
- CMake 3.20+
- SDL2
- Vulkan SDK (for RT64 renderer)
- Python 3 (for symbol tools)

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md) for details. Key areas needing help:

- **Threading** (signal-based interrupts or per-primitive patching)
- **RSP microcode** (the game uses a custom RSP ucode)
- **Stubbed functions** (434 functions need native reimplementations)
- **Symbol recovery** (Ghidra analysis, function boundary detection)

## Resources

- [N64Recomp](https://github.com/N64Recomp/N64Recomp) — the recompiler
- [N64ModernRuntime](https://github.com/N64Recomp/N64ModernRuntime) — the runtime
- [RT64](https://github.com/rt64/rt64) — the renderer
- [Recompedia](https://recompedia.com/platform/n64) — list of working N64 recomps

## License

[MIT](LICENSE)
