# Mace: The Dark Age — PC port status

Static recompilation of *Mace: The Dark Age* (N64, USA) using N64Recomp +
the N64 modern runtime (librecomp / ultramodern) with the RT64 renderer.

## Status: boots up to the game's own init, then crashes in engine code

```
$ ./build/mace-recomp "roms/Mace - The Dark Age (USA).z64"
Device Name: NVIDIA GeForce RTX 3060        <- RT64/Vulkan initialises
Initializing recomp heap at offset 0x01000000 with size 0x1F000000
-> crash in func_8006be10 (called from the game init at 0x8006CB64)
```

## What works

* ROM handling: the ROM is **pristine** (its header CRC1/CRC2 match; verified
  with `tools/rom_crc.py` against the reference `n64crc` algorithm). Hash for
  the game entry: `XXH3-64 = 0x46100E155439376A`.
* Static recompile of the whole code section completes
  (`tools/fix_symbols.py` drives N64Recomp until it succeeds).
  ~3600 functions, 434 of them stubbed (functions the recompiler cannot
  analyse statically, mostly because their switch jump tables live in ROM
  data that is not statically resolvable).
* Runtime: SDL window, RT64 (Vulkan) renderer, SDL audio, keyboard/gamepad
  input, `recomp::start()` wiring, ROM selection/copying into the config dir.
* The game entry stub runs: BSS clear, stack setup, heap init, then it calls
  the engine init.

## Key findings about the ROM

* The code section is `0x1000..0x7D600` (file offsets); `0x7D600` onwards is
  data (strings, the custom VFS table at `0x8B978`, `.rom` blobs).
* **The ROM mixes code from two builds.** ~25 functions (the entry stub at
  `0x1000` and 24 switch statements) reference addresses that are exactly
  `0xC00` bytes lower than the file layout, e.g. the entry stub jumps to
  `0x8006BF64` while the function it really wants is at file offset
  `0x6CB64`. Everything else (99% of the `jal` targets) matches the file
  layout exactly. The entry jump is fixed at recompile time with an
  instruction patch in `mace.toml`; the affected switches are stubbed.
* The game uses a custom VFS: `.rom` blobs (models, textures, sounds) are
  stored in the cart and decompressed by the game's own code at runtime, so
  the recompiler cannot resolve the game's switch tables statically.
* The boot section (`0x400..0x1000`) is the IPL3/CRT0 area. Several of its
  functions call into the IPL3 boot code that the PIF normally copies to
  physical `0x400000` (the `0x84xxxxxx` call pattern) and probe the cart's
  memory map. They cannot be recompiled as-is and are stubbed, but the game
  *calls* into that area, so the boundaries there need to be reconstructed
  (Ghidra split the merged functions) and the probes need native
  reimplementations.

## Open problems / next steps

1. **Boot section (`0x400..0x1000`) reconstruction.** The function boundaries
   there are wrong (Ghidra split functions that the compiler merged) and the
   four functions that call the IPL3 boot code are stubbed. The game's init
   calls into this area, so the stubbed allocator is what makes the game
   crash with a null pointer.
2. **Game init crash** (`func_8006be10`): a store through a null pointer,
   i.e. the game asked the (stubbed) allocator for memory and got nothing.
   Fixing (1) should fix this.
3. **RSP microcode**: the game uploads its own RSP microcode to IMEM and
   renders through it. The runtime needs a microcode implementation:
   either recompile the microcode with `RSPRecomp` (upstream Zelda64Recomp
   does this) or write a generic RSP interpreter. The runtime already
   contains the vector unit (`librecomp/rsp_vu.hpp`); the scalar/control part
   and the SP register emulation are missing. The game also writes the SP
   registers directly (`0xA4040000`), which the runtime does not emulate yet.
4. **Stubbed functions** (434) need to be reimplemented in C++ for correct
   behaviour.
5. Windows build: the CMake setup targets Windows + Linux; only Linux has
   been built here.

## Layout

```
CMakeLists.txt          build for the app (RT64 + runtime + recompiled code)
mace.toml               N64Recomp config (entry point, stubs, instruction patch)
config/symbols.toml     function map used by the recompiler
config/symbols_ghidra.json  raw Ghidra function list
src/main.cpp            app: game registration, SDL window/audio/input, runtime start
src/mace_rt64.cpp       RT64 renderer adapter for ultramodern
src/mace_overlays.cpp    registers the recompiled code sections with the runtime
tools/fix_symbols.py    drives N64Recomp until the whole ROM recompiles
tools/json_to_toml.py   Ghidra JSON -> symbols.toml
tools/disasm.py         quick MIPS disassembly of a ROM range
tools/check_jtables.py  finds indirect jumps whose table is not in the ROM
tools/rom_crc.py        N64 header CRC verification
tools/rom_repair.py     experiments to reverse a ROM patch
```

## Build

```bash
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
./build/mace-recomp "roms/Mace - The Dark Age (USA).z64"
```

The recompiler step (only needed when the symbols change):

```bash
tools/N64Recomp/build/N64Recomp mace.toml
```
