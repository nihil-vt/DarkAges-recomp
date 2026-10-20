// Native reimplementations of game functions that cannot be recompiled.
//
// The game's boot code (cart 0x400..0x1000) talks to the PIF/PI hardware
// directly and calls into the IPL3 boot code that the PIF copies to physical
// 0x400000 during the boot. The recomp runtime does not emulate the PIF, so
// the boot code's IPL3 calls are routed to native stubs (see mace.toml).

#include "mace_patches.h"

#include "recomp.h"

// The IPL3 boot code probes the cart and the RAM. The values below are what
// the game expects: an 8 MB cart and 8 MB of RAM.
extern "C" void func_8007d200_recomp(uint8_t* rdram, recomp_context* ctx) {
    ctx->r2 = 8 * 1024 * 1024;
}

extern "C" void func_8007d204_recomp(uint8_t* rdram, recomp_context* ctx) {
    ctx->r2 = 8 * 1024 * 1024;
}

extern "C" void func_8007d208_recomp(uint8_t* rdram, recomp_context* ctx) {
    ctx->r2 = 0;
}

extern "C" void func_8007d20c_recomp(uint8_t* rdram, recomp_context* ctx) {
    ctx->r2 = 0;
}

extern "C" void func_8007d210_recomp(uint8_t* rdram, recomp_context* ctx) {
    ctx->r2 = 0;
}
