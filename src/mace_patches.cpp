// Native reimplementations of game functions that cannot be recompiled.
//
// The game's boot code (cart 0x400..0x1000) talks to the PIF/PI hardware
// directly and calls into the IPL3 boot code that the PIF copies to physical
// 0x400000 during the boot. The recomp runtime does not emulate the PIF, so
// the boot code's IPL3 calls are routed to native stubs (see mace.toml).

#include "mace_patches.h"

#include <cstdio>
#include <cstdlib>

#include "recomp.h"

// Provided by librecomp: looks up a recompiled function by its address.
extern "C" recomp_func_t* get_function(int32_t addr);

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

// The IPL3's secondary boot code. The PIF copies it to physical 0x400000
// during the boot and the cart's boot code jumps to it right before the
// game starts.
extern "C" void func_80000000_recomp(uint8_t* rdram, recomp_context* ctx) {
    static uint32_t main_address = 0x8005104C;
    const char* main_env = getenv("MACE_MAIN");
    if (main_env != nullptr) {
        main_address = (uint32_t)strtoul(main_env, nullptr, 0);
    }
    fprintf(stderr, "mace: entering the game at 0x%08X\n", main_address);
    // The IPL3's secondary boot code sets up the hardware and then jumps into
    // the game. The setup lives in the cart's boot code.
    static const uint32_t setup_functions[] = {0x80000400u};
    for (uint32_t setup : setup_functions) {
        recomp_func_t* setup_func = get_function((int32_t)setup);
        if (setup_func != nullptr) {
            setup_func(rdram, ctx);
        }
    }
    recomp_func_t* func = get_function((int32_t)main_address);
    func(rdram, ctx);
}

