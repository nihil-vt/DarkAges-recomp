# M0/M1 ROM Survey — Mace: The Dark Ages (USA)

## ROM baseline
- File: `roms/Mace - The Dark Age (USA).z64`
- Size: 12,582,912 bytes (96 Mbit)
- SHA-1: `05d82a2c73ac536180b68137dbb9972a9e8e883e`
- MD5: `39a2bca1c17cd4cf1a9f3ae2b725b5c6`
- Byte order: big-endian z64 (`80 37 12 40`)

## Boot header (libultra, IDO toolchain)
- Clock: default (0x0F)
- Entry point: `0x80000400` (standard), code at ROM 0x400 starts with
  `bgezal $zero` __start sequence
- libultra release word @0x0C: `0x00001447`
- CRC1 `0x6b700750`, CRC2 `0x29d621fe`
- Game name "MACE", NTSC (0x4E), cart version 0
- CIC: undetermined (custom detection algo mismatch; low priority — recomp
  does not PIF-boot)

## Engine / asset filesystem (custom VFS)
- Custom archive FS with path strings in ROM, TOC record format:
  `[u32 romoff][u32 0][u32 0][name, NUL + pad to 8][u32 0x20000000][u32 x][u32 y]`
  - `x`/`y`: compressed size / uncompressed size (approx; x<=y in all samples)
  - Data at offsets looks entropy-coded — custom compression, no zlib/ysize magic
  - 78 records parsed from table (some entries have variations; 206 path string
    occurrences total)
- Path tree: per-character dirs (`/assassin`, `/berserker`, `/undead`, ...),
  each with `rom/model.rom`, `rom/linkseq.rom`, `rom/message.rom`,
  `dynrom/model.rom`, `dynrom/textures.rom`, `fxrom/model.rom`,
  `text/{book,defeat,victory}.rom`
- Also: `/battle/{model,worlds}.rom`, `/boss/...`, `/book/...`, `/audio/model.rom`,
  `/bios/model.rom`, `/configure/model.rom`
- Strings: `/all.dynamic.anims`, dev name "MINH NGUYEN"

## Implications for port
- Runtime loads ROM at runtime; game's own (recompiled) decompressor handles
  VFS entries — no need to crack compression for v1
- BYO-ROM flow: verify hash -> mmap/serve ROM bytes to recompiled code
- Code segment: starts ROM 0x1000; strings begin ~0x82e08; TOC ~0x8b9xx-0x8c7xx
- Microcode: not yet identified (no telltale strings). Verify at first
  runtime boot via RT64 ucode-hash log. Risk item if non-F3DEX2.

## Toolchain status
- cmake/gcc/g++/git/python3/SDL2: present
- Ghidra: absent (try N64Recomp builtin function heuristics first)
