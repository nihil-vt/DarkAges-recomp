# Mace: The Dark Age — PC port status

Static recompilation of *Mace: The Dark Age* (N64, USA) using N64Recomp +
the N64 modern runtime (librecomp / ultramodern) with the RT64 renderer.

## Status: the game boots and runs, but the screen stays black (threading blocker)

```
$ MACE_HEADLESS=1 ./build/mace-recomp "roms/Mace - The Dark Age (USA).z64"
[mace] loaded 12582912 bytes of ROM for DMA emulation
[mace] task table initialized
[mace] SP memory initialized with 8388608 bytes of ROM
mace: starting at 0x80000400
[trace] enter func_80000400
...
(timeout after 300s, no crash)
```

Phase 1 and Phase 2 are complete: the game's boot sequence runs, the message queues
work, the task table is initialized, and tasks are created. The main loop runs
continuously without segfaulting.

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
* The game's boot code runs: the stack is set up by the launcher and the
  boot code's calls into the IPL3 are routed to native stubs
  (`src/mace_patches.cpp`).

## Key findings about the ROM

* The code section is `0x1000..0x7D600` (file offsets); `0x7D600` onwards is
  data (strings, the custom VFS table at `0x8B978`, `.rom` blobs).
* **There is no two-build mix; the mapping was wrong.** The IPL3 DMAs cart
  `0x1000` to RDRAM physical `0x400`, i.e. kseg0 `0x80000400`, so the code
  section is `vram = file + 0x7FFFF400`, not `file + 0x80000000`. Under the
  correct mapping the entry stub's jump target `0x8006BF64` is file `0x6CB64`,
  a real function prologue (`addiu sp,sp,-0x88`). Checked over the 4905 `jal`
  targets in the code section: 2315 land on a `addiu sp` prologue with the
  `0x80000400` mapping versus 12 with `0x80001000`. The entry patch and the
  "stubbed switch statements" caused by the old mapping are no longer needed.
  The entry is now `0x80000400` (`mace.toml`), and `tools/make_rom_elf.py`
  builds an ELF with this mapping for analysis.
* `0x8000095C` is a heap allocator and `0x800008C8` is memset, not a PI probe.
  The boot code (`0x80000400`) clears BSS, sets `sp = 0x801614F0` and
  `jr`s to the game's bootstrap `0x8006BF64`. That bootstrap calls the OS
  (`osCreateThread`/`osStartThread` in the real game), and the scheduler never
  returns. In the recomp it returns, so the game's main thread never runs and
  the window stays black.
* Done: the game's `osCreateThread` (`0x80073880`) and `osStartThread`
  (`0x800739D0`) are renamed in `config/symbols.toml`, so the recompiler
  emits the native `osCreateThread_recomp`/`osStartThread_recomp`. Thread
  creation works (entry `0x800666A0`, id 1). Do NOT add them to `ignored`:
  the recompiler already treats them as reimplemented, and listing them there
  makes N64Recomp exit with "set as ignored ... does not exist".
* Task table: `0x8007E760`, stride `0x1D0`, key at `+4`, link at `+0x1D4`,
  size at `+0xC`, stack base at `+0x14`. `0x8006A010` allocates the stack bases.
  `0x8005CFD8` looks a key up by walking the links. The game's main thread is
  `0x800666A0`, which creates tasks 6 and 7 and starts them with `0x8005D024`.
* Fixed this round (all in the build pipeline, not game logic):
  1. **Fall-through between split functions was dropped.** Ghidra splits some
     functions at a point where the code simply continues, so `0x8005D024`
     never reached its `osStartThread` call. `tools/N64Recomp` now emits a tail
     call to the next function when a function ends without a jump. Source is
     saved in `tools/patches/N64Recomp-local.patch`; rebuild with
     `cmake --build tools/N64Recomp/build --target N64RecompCLI`.
  2. **Bad symbol splits** in `config/symbols.toml`: `0x80066660` swallowed the
     thread entry `0x800666A0` (split); `0x8006570C` and `0x80065938` were one
     function (merged); `0x80065FB8` had no symbol (added, merged with
     `0x800660F0`). Added functions must start after a `jr ra` and must not end
     in the middle of a branch target: `N64Recomp` reports these as
     "Unhandled branch" or "Error in recompiling".
  3. **`__osRunningThread` (`0x80081BD0`)** is not set by the native scheduler.
     The game's `osGetThreadPri(NULL)` reads it, so `mace_thread_start` in
     `src/main.cpp` writes `ultramodern::this_thread()` there at each thread start.
* Root cause of the `0x8010CAA0` crash (found): `func_80070CE4` was split by
  Ghidra into ten fragments. Its `addiu sp,-0x30` prologue was never undone,
  so every later call in `func_8006570C` ran with `sp` 0x108 too low and its
  stack slots (`0x120`..`0x138`) were written to the wrong frame. Merging the
  cluster (`0x80070CE4` size `0x104`, ten fragments removed) fixes it.
* Current blocker (after that merge): `func_80070CE4` is called with a NULL
  structure pointer from `func_8001D40C`, so an asset/data load returns nothing.
  Likely the PI DMA path (Phase 3). Next step: trace `func_8001D40C` and its
  caller.
* `tools/merge_fragments.py` generalises the merge but does not converge yet: each
  pass exposes another branch into the middle of a function (boot section, and
  stubbed `func_80001DD4`). Run it report-only; do not apply it blindly.
* Old blocker (now resolved): task 6 (`0x80065FB8`) runs and reaches `0x80070B40`, which
  loads a struct from `0x8010CAA0` and segfaults. That struct is probably set by
  init code that is not running correctly yet. Next step: find who writes
  `0x8010CAA0` and why it is still junk.
* Diagnostics: `src/main.cpp` dumps the task table after boot. The earlier
  OS-queue dump was wrong, because RDRAM words are host-order and it assembled
  bytes big-endian. Use `MEM_W` for word reads.
* The game uses a custom VFS: `.rom` blobs (models, textures, sounds) are
  stored in the cart and decompressed by the game's own code at runtime, so
  the recompiler cannot resolve the game's switch tables statically.
* The boot section (`0x400..0x1000`) is the IPL3/CRT0 area. Several of its
  functions call into the IPL3 boot code that the PIF normally copies to
  physical `0x400000` (the `0x84xxxxxx` call pattern) and probe the cart's
  memory map. They cannot be recompiled as-is, and the recomp never loads
  the boot section at all (the IPL3's 1 MB DMA starts at cart `0x1000`).
* Memory model note: the recomp's `MEM_*` macros expect sign-extended
  32-bit values in the register file, i.e. the game's kseg0 addresses
  (`0x80xxxxxx`) map to RDRAM offsets `0..8 MB`. The entry point therefore
  has to be passed sign-extended (`(gpr)(int64_t)(int32_t)0x80001000`).

## Open problems / next steps

0. **Memory mapped I/O for the boot code.** The boot code programs the PI
   (cart DMA), VI, AI and EXI registers directly. The recomp runtime only
   emulates the SDK level entry points, so either the boot code's hardware
   functions have to be reimplemented natively (like the IPL3 stubs) or the
   runtime needs a small MMIO layer. Until then the game waits for a cart DMA
   that never completes and the window stays black.


1. **Resolve the two-build mix.** The port has to pick one of the two builds
   and remap the whole ROM (or find the original ROM revision the game
   expects). Everything else depends on this: the entry point, the boot
   section and the 434 stubs.
2. **Boot section** (`0x400..0x1000`): reconstruct the merged functions,
   reimplement the four IPL3-dependent probes natively, and make the runtime
   load the boot section (the game calls into it).
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
