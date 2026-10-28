// Mace: The Dark Age - static recompilation runtime.
//
// Boots the recompiled game through the N64 modern runtime, using RT64 for
// rendering, SDL for the window/audio and a simple controller mapping.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>
#include <thread>
#include <chrono>
#include <sys/mman.h>

#ifndef MACE_HEADLESS
#include "SDL.h"
#endif

#include "ultramodern/ultramodern.hpp"
#include "librecomp/game.hpp"
#include "recomp.h"

// VI register helper functions
uint32_t* mace_get_vi_current() { return (uint32_t*)(vi_reg_buf + 0x10); }
uint32_t* mace_get_vi_intr() { return (uint32_t*)(vi_reg_buf + 0x0C); }

#include "mace_rt64.h"
#include "mace_overlays.h"

// N64 controller button bits (libultra osContPad layout).
namespace n64_button {
    constexpr uint16_t A = 0x8000;
    constexpr uint16_t B = 0x4000;
    constexpr uint16_t Z = 0x2000;
    constexpr uint16_t START = 0x1000;
    constexpr uint16_t DPAD_UP = 0x0800;
    constexpr uint16_t DPAD_DOWN = 0x0400;
    constexpr uint16_t DPAD_LEFT = 0x0200;
    constexpr uint16_t DPAD_RIGHT = 0x0100;
    constexpr uint16_t L = 0x0020;
    constexpr uint16_t R = 0x0010;
} // namespace n64_button

extern "C" void recomp_entrypoint(uint8_t* rdram, recomp_context* ctx);

// The address of the game function to start at. Overridable so that the
// correct entry point can be searched for at runtime.
uint32_t g_entry_address = 0x80000400;
static void install_mmio_handler(); // forward decl

// ROM data for DMA emulation.
static uint8_t* g_rom_data = nullptr;
static size_t g_rom_size = 0;

// SP memory buffer (defined here, declared extern in recomp.h).
uint8_t* sp_buffer = nullptr;

// SP/PI DMA state tracking.
static struct {
    uint32_t dram_addr;
    uint32_t cart_addr;
    uint32_t status;
} sp_regs, pi_regs;

// AI DMA state tracking.
static struct {
    uint32_t dram_addr;
    uint32_t length;
} ai_regs;

// Delay counters for status register busy simulation.
static int pi_status_delay = 0;
static int ai_status_delay = 0;
static int sp_status_delay = 0;
static int g_mace_verbose = 0;

// Wrapper around the game's entry point: the runtime calls this instead of the
// recompiled entry function so that the entry point can be changed without
// recompiling.

// Custom thread switch callback - called from pause_self to process game's thread queue
// Shim: translate game's custom thread init to runtime's osCreateThread
extern "C" void mace_thread_init_shim(uint8_t* rdram, recomp_context* ctx) {
    // Game's func_80073880 args: r4=task_struct, r5=entry, r6=arg2, r7=arg3, sp+0x14=field8
    // Runtime's osCreateThread args: r4=id, r5=pri, r6=thread, r7=entry, sp+10=arg, sp+14=sp
    uint32_t task_struct = ctx->r4;
    uint32_t entry = ctx->r5;

    // Set up runtime arguments
    ctx->r4 = MEM_W(0x04, task_struct);  // id = task->key
    ctx->r5 = 10;  // priority
    ctx->r6 = task_struct;  // thread pointer (task struct IS the thread struct)
    ctx->r7 = entry;  // entry point
    MEM_W(0x10, ctx->r29) = task_struct;  // arg = task struct
    MEM_W(0x14, ctx->r29) = 0xFFFFFFFF80067010ULL + 0x10000;  // stack pointer

    osCreateThread_recomp(rdram, ctx);
}

// Shim: translate game's custom thread start to runtime's osStartThread
extern "C" void mace_thread_start_shim(uint8_t* rdram, recomp_context* ctx) {
    // Game's func_800739D0 args: r4=task_struct
    // Runtime's osStartThread args: r4=thread
    // task_struct IS the thread struct
    osStartThread_recomp(rdram, ctx);
}

extern "C" void mace_entry(uint8_t* rdram, recomp_context* ctx) {
    // Check for verbose mode
    if (getenv("MACE_VERBOSE")) g_mace_verbose = 1;

    install_mmio_handler();

    // The recompiler stores constants in RIP-relative addresses that land past
    // the 512MB mprotected region. Mprotect the entire mmap as RW to avoid faults.
    extern void* mmap_base;
    extern size_t mmap_size;
    // Use /proc/self/maps to find the mmap region and mprotect it.
    // For now, just mprotect a large region after the initial RDRAM.
    mprotect(rdram + 0x20000000ULL, 0x60000000ULL, PROT_READ | PROT_WRITE); // +8GB region

    // Load the ROM data for DMA emulation. The game reads cartridge data
    // through SP/PI DMA; without emulation it gets zeros.
    const char* rom_file = rom_path.empty() ? "roms/Mace - The Dark Age (USA).z64" : rom_path.string().c_str();
    FILE* rom = fopen(rom_file, "rb");
    if (rom) {
        fseek(rom, 0, SEEK_END);
        g_rom_size = ftell(rom);
        fseek(rom, 0, SEEK_SET);
        g_rom_data = (uint8_t*)malloc(g_rom_size);
        fread(g_rom_data, 1, g_rom_size, rom);
        fclose(rom);
        fprintf(stderr, "[mace] loaded %zu bytes of ROM for DMA emulation\n", g_rom_size);
    }

    // Initialize the task table at 0x8007E760 so the boot code can find tasks.
    // The game creates task 1 (main thread), then tasks 6 and 7 from task 1.
    memset(rdram + 0x7E760, 0, 0x1D0 * 4); // clear task table
    // Entry 0 (task 1 - main thread): key at +4, entry point at +10
    *(uint32_t*)(rdram + 0x7E760 + 0x04) = 1; // key
    *(uint32_t*)(rdram + 0x7E760 + 0x10) = 0x800666A0; // entry point (main thread)
    *(uint32_t*)(rdram + 0x7E760 + 0x14) = 0x80066010; // stack base
    *(uint32_t*)(rdram + 0x7E760 + 0x1C) = 0x100000; // stack size (1MB for main thread)
    *(uint32_t*)(rdram + 0x7E760 + 0x0C) = 0x100000; // entry size
    // Entry 1 (task 6): base = 0x7E760 + 0x1D0 = 0x7E930
    *(uint32_t*)(rdram + 0x7E760 + 0x1D0 + 0x04) = 6; // key at entry +4
    *(uint32_t*)(rdram + 0x7E760 + 0x1D0 + 0x10) = 0x80065FB8; // entry point
    *(uint32_t*)(rdram + 0x7E760 + 0x1D0 + 0x14) = 0x80066010 + 0x190000; // stack base
    *(uint32_t*)(rdram + 0x7E760 + 0x1D0 + 0x1C) = 0x50; // stack size
    *(uint32_t*)(rdram + 0x7E760 + 0x1D0 + 0x0C) = 0x50; // entry size
    // Entry 2 (task 7): base = 0x7E760 + 0x3A0 = 0x7EB00
    *(uint32_t*)(rdram + 0x7E760 + 0x3A0 + 0x04) = 7; // key at entry +4
    *(uint32_t*)(rdram + 0x7E760 + 0x3A0 + 0x10) = 0x8006D6EC; // entry point
    *(uint32_t*)(rdram + 0x7E760 + 0x3A0 + 0x14) = 0x80066010 + 0x320000; // stack base
    *(uint32_t*)(rdram + 0x7E760 + 0x3A0 + 0x1C) = 0x50; // stack size
    *(uint32_t*)(rdram + 0x7E760 + 0x3A0 + 0x0C) = 0x50; // entry size
    // Terminator: mark end of list with negative key at entry 3
    *(uint32_t*)(rdram + 0x7E760 + 0x570 + 0x04) = 0xFFFFFFFF; // end marker
    fprintf(stderr, "[mace] task table initialized\n");

    // Set up the ready queue at 0xFFFFFFFF80081C64
    // The VI handler reads this to find threads to switch to
    {
        uint32_t* ready_queue = (uint32_t*)(rdram + (0xFFFFFFFF80081C64ULL - 0xFFFFFFFF80000000ULL));
        // Point to task 6 entry (entry 1 in task table)
        *ready_queue = 0xFFFFFFFF8007E930ULL;

        // Set up task 6 thread structure for the ready queue
        size_t t6_off = 0xFFFFFFFF8007E930ULL - 0xFFFFFFFF80000000ULL;
        *(uint32_t*)(rdram + t6_off + 0x00) = 0xFFFFFFFF8007E930ULL + 0x1D0;  // next = task 7
        *(uint32_t*)(rdram + t6_off + 0x04) = 6;  // task ID
        *(uint32_t*)(rdram + t6_off + 0x08) = 1;  // flag (ready)
        *(uint32_t*)(rdram + t6_off + 0x14) = 0xFFFFFFFF80065FB8ULL;  // entry point

        // Set up task 7 thread structure
        size_t t7_off = 0xFFFFFFFF8007E930ULL + 0x1D0 - 0xFFFFFFFF80000000ULL;
        *(uint32_t*)(rdram + t7_off + 0x00) = 0;  // next = 0 (end of queue)
        *(uint32_t*)(rdram + t7_off + 0x04) = 7;  // task ID
        *(uint32_t*)(rdram + t7_off + 0x08) = 1;  // flag (ready)
        *(uint32_t*)(rdram + t7_off + 0x14) = 0xFFFFFFFF8006D6E8ULL;  // entry point

        fprintf(stderr, "[mace] ready_queue = %08X\n", *ready_queue);
    }

    // SP memory (0xA0000000-0xA0800000 in MIPS space) maps to RDRAM offset
    // 0x0A000000-0x0A800000. Make this region PROT_NONE so the handler catches it.
    // Allocate SP memory buffer and copy ROM data into it.
    sp_buffer = (uint8_t*)malloc(0x800000);
    if (sp_buffer && g_rom_data) {
        memset(sp_buffer, 0, 0x800000);
        memcpy(sp_buffer, g_rom_data, g_rom_size < 0x800000 ? g_rom_size : 0x800000);
        fprintf(stderr, "[mace] SP memory initialized with %zu bytes of ROM\n", g_rom_size < 0x800000 ? g_rom_size : 0x800000);
    }

    // Create a proper VI message queue and register it with osViSetEvent
    {
        // Create queue at 0xFFFFFFFF80091790
        size_t qoff = 0xFFFFFFFF80091790ULL - 0xFFFFFFFF80000000ULL;
        uint32_t* first = (uint32_t*)(rdram + qoff);
        uint32_t* last  = (uint32_t*)(rdram + qoff + 4);
        int32_t*  validCount = (int32_t*)(rdram + qoff + 8);
        int32_t*  msgCount   = (int32_t*)(rdram + qoff + 16);
        uint32_t* msg_ptr    = (uint32_t*)(rdram + qoff + 20);
        *first = 0;
        *last = 0; // not used in this layout
        *validCount = 0;
        *msgCount = 8;
        *msg_ptr = 0xFFFFFFFF800917A8; // message buffer

        // Register with osViSetEvent
        extern void osViSetEvent_recomp(uint8_t* rdram, recomp_context* ctx);
        uint8_t ctx_buf[0x200] = {0};
        recomp_context* ctx = (recomp_context*)ctx_buf;
        ctx->r4 = 0xFFFFFFFF80091790; // queue pointer
        ctx->r5 = 1; // message value (OS_EVENT_RETRACE)
        ctx->r6 = 1; // retrace count
        osViSetEvent_recomp(rdram, ctx);
        fprintf(stderr, "[mace] registered VI queue at 0xFFFFFFFF80091790\n");
    }

    // Initialize the game's VI message queue at 0xFFFFFFFF80091790
    // OSMesgQueue format: blocked_on_recv(u32), blocked_on_send(u32), validCount(s32),
    //                     first(s32), msgCount(s32), msg_ptr(u32)
    {
        size_t qoff = 0xFFFFFFFF80091790ULL - 0xFFFFFFFF80000000ULL;
        uint32_t* blocked_recv = (uint32_t*)(rdram + qoff);
        uint32_t* blocked_send = (uint32_t*)(rdram + qoff + 4);
        int32_t*  validCount   = (int32_t*)(rdram + qoff + 8);
        int32_t*  first        = (int32_t*)(rdram + qoff + 12);
        int32_t*  msgCount     = (int32_t*)(rdram + qoff + 16);
        uint32_t* msg_ptr      = (uint32_t*)(rdram + qoff + 20);
        *blocked_recv = 0;
        *blocked_send = 0;
        *validCount = 0;
        *first = 0;
        *msgCount = 8;
        // Allocate message buffer after the queue structure
        *msg_ptr = 0xFFFFFFFF800917A8ULL;
        fprintf(stderr, "[mace] initialized VI queue at 0xFFFFFFFF80091790\n");
    }

    // Manually set up the game's threading system
    // Thread structure: +0x00=next, +0x04=priority, +0x14=entry, +0x38/0x3C=stack_ptr
    {
        fprintf(stderr, "[mace] setting up threading...\n");
        fprintf(stderr, "[mace] rdram=%p\n", (void*)rdram);
        // Task 6 thread structure at task table entry 0
        uint64_t task6_base = 0xFFFFFFFF8007E760ULL;
        uint64_t task7_base = 0xFFFFFFFF8007E930ULL;
        fprintf(stderr, "[mace] task6_base=%08X task7_base=%08X\n", task6_base, task7_base);
        uint32_t* t6 = (uint32_t*)(rdram + (task6_base - 0xFFFFFFFF80000000ULL));
        uint32_t* t7 = (uint32_t*)(rdram + (task7_base - 0xFFFFFFFF80000000ULL));
        fprintf(stderr, "[mace] t6=%p t7=%p\n", (void*)t6, (void*)t7);
        fprintf(stderr, "[mace] about to set up task 6...\n");

        // Set up task 6 thread
        t6[0x00/4] = task7_base;        // next = task 7
        t6[0x04/4] = 6;                 // priority
        t6[0x14/4] = 0xFFFFFFFF80065FB8ULL; // entry point
        t6[0x38/4] = 0;                 // stack pointer high
        t6[0x3C/4] = 0xFFFFFFFF80067010ULL + 0x10000; // stack pointer

        // Set up task 7 thread
        t7[0x00/4] = 0;                 // next = 0 (end of queue)
        t7[0x04/4] = 7;                 // priority
        t7[0x14/4] = 0xFFFFFFFF8006D6E8ULL; // entry point
        t7[0x38/4] = 0;                 // stack pointer high
        t7[0x3C/4] = 0xFFFFFFFF80068000ULL + 0x10000; // stack pointer

        fprintf(stderr, "[mace] task6 thread: next=%08X pri=%d entry=%08X\n", t6[0], t6[4/4], t6[0x14/4]);
        fprintf(stderr, "[mace] task7 thread: next=%08X pri=%d entry=%08X\n", t7[0], t7[4/4], t7[0x14/4]);

        // Set ready queue head to task 6
        uint32_t* ready_queue = (uint32_t*)(rdram + (0xFFFFFFFF80081C64ULL - 0xFFFFFFFF80000000ULL));
        *ready_queue = task6_base;
        fprintf(stderr, "[mace] ready_queue = %08X\n", *ready_queue);

        fprintf(stderr, "[mace] initialized threading: ready_queue->task6->task7\n");
    }

    // Register game threads with the runtime scheduler
    // Threads are in a linked list at 0xFFFFFFFF80081C64 (ready queue head)
    // Each thread: +0x00=next, +0x04=key, +0x10=entry_point, +0x14=stack_base
    {
        uint32_t* ready_queue = (uint32_t*)(rdram + (0xFFFFFFFF80081C64ULL - 0xFFFFFFFF80000000ULL));
        uint32_t thread_ptr = *ready_queue;
        int count = 0;
        while (thread_ptr != 0 && thread_ptr >= 0xFFFFFFFF80000000ULL) {
            uint32_t* t = (uint32_t*)(rdram + (thread_ptr - 0xFFFFFFFF80000000ULL));
            uint32_t next = t[0];
            uint32_t entry = t[0x10/4];  // entry point at offset 0x10
            fprintf(stderr, "[mace] thread @%08X entry=%08X next=%08X\n", thread_ptr, entry, next);
            thread_ptr = next;
            count++;
        }
        fprintf(stderr, "[mace] registered %d game threads\n", count);
    }

    // Debug thread: periodically dump RDRAM state to check progress
    std::thread([rdram]{
        for (int i = 0; i < 10; i++) {
            std::this_thread::sleep_for(std::chrono::seconds(5));
            // Dump key memory locations to check progress
            uint8_t* r = rdram;
            uint32_t val_1c64 = *(uint32_t*)(r + 0xFFFFFFFF80081C64 - 0xFFFFFFFF80000000);
            uint32_t val_1c00 = *(uint32_t*)(r + 0xFFFFFFFF80081C00 - 0xFFFFFFFF80000000);
            fprintf(stderr, "[tick %d] 80081C64=%08X 80081C00=%08X VI_V_CURRENT=%u\n",
                i, val_1c64, val_1c00, *mace_get_vi_current());
        }
    }).detach();

    // VI timer thread - updates VI registers and sends retrace messages
    std::thread([rdram]{
        uint32_t scanline = 0;
        uint32_t frame = 0;
        fprintf(stderr, "[vi] timer thread started\n");
        while (true) {
            // Update VI_V_CURRENT (scanline counter)
            *mace_get_vi_current() = scanline;
            scanline += 7;
            if (scanline >= 263) {
                scanline = 0;
                frame++;
                // Toggle VI_V_INTR to simulate retrace
                *mace_get_vi_intr() ^= 1;
                // Try to send retrace message to game's queue at 0xFFFFFFFF80091790
                // Queue format: blocked_on_recv(u32), blocked_on_send(u32), validCount(s32),
                //               first(s32), msgCount(s32), msg_ptr(u32)
                size_t qoff = 0xFFFFFFFF80091790ULL - 0xFFFFFFFF80000000ULL;
                if (qoff + 24 <= 0x1F000000) {
                    volatile int32_t* validCount = (volatile int32_t*)(rdram + qoff + 8);
                    volatile int32_t* first      = (volatile int32_t*)(rdram + qoff + 12);
                    volatile int32_t* msgCount   = (volatile int32_t*)(rdram + qoff + 16);
                    volatile uint32_t* msg_ptr    = (volatile uint32_t*)(rdram + qoff + 20);
                    int32_t mc = *msgCount;
                    int32_t vc = *validCount;
                    uint32_t buf = *msg_ptr;
                    if (mc > 0 && vc < mc && buf >= 0xFFFFFFFF80000000ULL) {
                        size_t buf_off = buf - 0x80000000;
                        if (buf_off + mc * 4 <= 0x1F000000) {
                            volatile uint32_t* msgs = (volatile uint32_t*)(rdram + buf_off);
                            int32_t idx = *first;
                            msgs[idx] = 1; // OS_EVENT_RETRACE
                            *first = (idx + 1) % mc;
                            (*validCount)++;
                        }
                    }
                }
                // Call game's VI handler to process thread queue
                extern recomp_func_t* get_function(int32_t address);
                recomp_func_t* vi_handler = get_function(0x80075F20);
                if (vi_handler) {
                    uint8_t ctx_buf[0x200] = {0};
                    recomp_context* ctx = (recomp_context*)ctx_buf;
                    // Set up minimal context for the VI handler
                    ctx->r29 = 0xFFFFFFFF80067010 + 0x10000; // stack
                    ctx->r31 = 0x80000400; // return address
                    // Read current thread from __osRunningThread
                    ctx->r27 = *(uint32_t*)(rdram + (0xFFFFFFFF80081BD0 - 0xFFFFFFFF80000000));
                    ctx->r28 = *(uint32_t*)(rdram + (0xFFFFFFFF80081BD4 - 0xFFFFFFFF80000000));
                    vi_handler(rdram, ctx);
                    if (frame % 60 == 0) fprintf(stderr, "[vi] called handler, r2=%d\n", (int32_t)ctx->r2);
                }
                if (frame % 60 == 0) fprintf(stderr, "[vi] frame %u\n", frame);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }).detach();

    const char* entry_env = getenv("MACE_ENTRY");
    if (entry_env != nullptr) {
        g_entry_address = (uint32_t)strtoul(entry_env, nullptr, 0);
    }
    fprintf(stderr, "mace: starting at 0x%08X\n", g_entry_address);
    // The boot code sets up the stack before calling into the game. The
    // recomp runtime does not run the boot code, so set up the stack here: the
    // game's stack lives at 0x801614F0, just above its BSS.
    ctx->r29 = (gpr)(int64_t)(int32_t)0x801614F0;
    ctx->r30 = (gpr)(int64_t)(int32_t)0x801614F0;
    ctx->r31 = 0;
    // The boot code checks $at to see whether this is a warm boot, the IPL3
    // clears it before jumping to the entry point.
    ctx->r1 = 0;
    recomp_func_t* func = get_function((int32_t)g_entry_address);
    func(rdram, ctx);
    // After the CRT0 init returns, hand control to the game's main routine.
    // Default is the (currently stubbed) candidate; override with MACE_MAIN.
    {
        static uint32_t main_address = 0;
        const char* me = getenv("MACE_MAIN");
        if (me) main_address = (uint32_t)strtoul(me, nullptr, 0);
        if (main_address != 0) {
            fprintf(stderr, "mace: handing off to main at 0x%08X (loop)\n", main_address);
            recomp_func_t* m = get_function((int32_t)main_address);
            if (m) {
                while (1) m(rdram, ctx);
            }
        }
    }
    // Dump the game's task table (stride 0x1D0) after boot: key at +4, link
    // at +0x1D4, stack base at +0x14, size at +0xC.
    if (getenv("MACE_DUMP_TASKS")) {
        auto rd = [&](uint32_t vram) { return *(int32_t*)(rdram + (vram - 0x80000000)); };
        for (int a = 0; a < 6; a++) {
            uint32_t v = 0x8007E760 + a * 0x1D0;
            fprintf(stderr, "mace: task @0x%08X key=%d link=%d base=%08X size=%X\n", v,
                    rd(v + 4), rd(v + 0x1D4), (uint32_t)rd(v + 0x14), (uint32_t)rd(v + 0xC));
        }
    }
}

// The game's libultra reads the running thread from __osRunningThread (0x80081BD0)
// when it is passed NULL. The native scheduler does not set it, so every game
// thread records itself there as it starts and whenever it resumes.
void mace_thread_start(uint8_t* rdram, recomp_context* ctx) {
    (void)ctx;
    *(uint32_t*)(rdram + (0x80081BD0u - 0x80000000u)) = (uint32_t)(uintptr_t)ultramodern::this_thread();
}

namespace {

// XXH3-64 of "Mace - The Dark Age (USA).z64".
constexpr uint64_t MACE_ROM_HASH = 0x46100E155439376AULL;
constexpr const char* MACE_INTERNAL_NAME = "MACE               "; // 19 chars, as in the ROM header
constexpr const char* MACE_GAME_ID = "mace.n64.us.1.0";
constexpr const char* MACE_MOD_GAME_ID = "mace";

constexpr uint32_t MACE_ENTRYPOINT = 0x80000400;

struct InputState {
    uint16_t buttons = 0;
    float stick_x = 0.0f;
    float stick_y = 0.0f;
    bool connected = false;
};

InputState g_input;

SDL_Window* g_window = nullptr;
SDL_GameController* g_controller = nullptr;

void apply_key(SDL_Keycode key, bool pressed) {
    uint16_t mask = 0;
    switch (key) {
        case SDLK_z: mask = n64_button::A; break;
        case SDLK_x: mask = n64_button::B; break;
        case SDLK_a: mask = n64_button::Z; break;
        case SDLK_s: mask = n64_button::START; break;
        case SDLK_q: mask = n64_button::L; break;
        case SDLK_w: mask = n64_button::R; break;
        case SDLK_UP: mask = n64_button::DPAD_UP; break;
        case SDLK_DOWN: mask = n64_button::DPAD_DOWN; break;
        case SDLK_LEFT: mask = n64_button::DPAD_LEFT; break;
        case SDLK_RIGHT: mask = n64_button::DPAD_RIGHT; break;
        default: return;
    }
    if (pressed) {
        g_input.buttons |= mask;
    }
    else {
        g_input.buttons &= ~mask;
    }
}

void poll_events() {
    if (getenv("MACE_HEADLESS")) return;
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        switch (event.type) {
            case SDL_KEYDOWN:
                if (event.key.repeat == 0) {
                    apply_key(event.key.keysym.sym, true);
                }
                break;
            case SDL_KEYUP:
                apply_key(event.key.keysym.sym, false);
                break;
            case SDL_CONTROLLERDEVICEADDED:
                if (g_controller == nullptr) {
                    g_controller = SDL_GameControllerOpen(event.cdevice.which);
                    g_input.connected = g_controller != nullptr;
                }
                break;
            case SDL_CONTROLLERDEVICEREMOVED:
                if (g_controller != nullptr) {
                    SDL_GameControllerClose(g_controller);
                    g_controller = nullptr;
                    g_input.connected = false;
                }
                break;
            case SDL_CONTROLLERAXISMOTION:
                if (g_controller != nullptr && event.caxis.axis == SDL_CONTROLLER_AXIS_LEFTX) {
                    g_input.stick_x = event.caxis.value / 32767.0f;
                }
                else if (g_controller != nullptr && event.caxis.axis == SDL_CONTROLLER_AXIS_LEFTY) {
                    g_input.stick_y = -event.caxis.value / 32767.0f;
                }
                break;
            case SDL_CONTROLLERBUTTONDOWN:
            case SDL_CONTROLLERBUTTONUP: {
                if (g_controller == nullptr) {
                    break;
                }
                uint16_t mask = 0;
                switch (event.cbutton.button) {
                    case SDL_CONTROLLER_BUTTON_A: mask = n64_button::A; break;
                    case SDL_CONTROLLER_BUTTON_B: mask = n64_button::B; break;
                    case SDL_CONTROLLER_BUTTON_X: mask = n64_button::Z; break;
                    case SDL_CONTROLLER_BUTTON_Y: mask = n64_button::L; break;
                    case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: mask = n64_button::L; break;
                    case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: mask = n64_button::R; break;
                    case SDL_CONTROLLER_BUTTON_DPAD_UP: mask = n64_button::DPAD_UP; break;
                    case SDL_CONTROLLER_BUTTON_DPAD_DOWN: mask = n64_button::DPAD_DOWN; break;
                    case SDL_CONTROLLER_BUTTON_DPAD_LEFT: mask = n64_button::DPAD_LEFT; break;
                    case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: mask = n64_button::DPAD_RIGHT; break;
                    case SDL_CONTROLLER_BUTTON_START: mask = n64_button::START; break;
                    default: break;
                }
                if (event.type == SDL_CONTROLLERBUTTONDOWN) {
                    g_input.buttons |= mask;
                }
                else {
                    g_input.buttons &= ~mask;
                }
                break;
            }
            case SDL_QUIT:
                ultramodern::quit();
                break;
            default:
                break;
        }
    }
}

void update_gfx(void*) { poll_events(); }

ultramodern::renderer::WindowHandle create_window(ultramodern::gfx_callbacks_t::gfx_data_t) {
    if (getenv("MACE_HEADLESS")) {
        fprintf(stderr, "[mace] headless mode: skipping window creation\n");
        return ultramodern::renderer::WindowHandle{ nullptr };
    }
    uint32_t flags = SDL_WINDOW_RESIZABLE;
#if defined(RT64_SDL_WINDOW_VULKAN)
    flags |= SDL_WINDOW_VULKAN;
#endif
    g_window = SDL_CreateWindow("Mace: The Dark Age", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 640, 480, flags);
    if (g_window == nullptr) {
        fprintf(stderr, "Failed to create window: %s\n", SDL_GetError());
        std::exit(EXIT_FAILURE);
    }
    return ultramodern::renderer::WindowHandle{ g_window };
}

ultramodern::gfx_callbacks_t::gfx_data_t create_gfx() {
    if (getenv("MACE_HEADLESS")) {
        fprintf(stderr, "[mace] headless mode: skipping SDL/RT64 init\n");
        return {};
    }
    SDL_SetHint(SDL_HINT_GAMECONTROLLER_USE_BUTTON_LABELS, "0");
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER | SDL_INIT_AUDIO) != 0) {
        fprintf(stderr, "Failed to initialize SDL: %s\n", SDL_GetError());
        std::exit(EXIT_FAILURE);
    }
    if (SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) == 0) {
        for (int i = 0; i < SDL_NumJoysticks(); i++) {
            if (SDL_IsGameController(i)) {
                g_controller = SDL_GameControllerOpen(i);
                g_input.connected = g_controller != nullptr;
                break;
            }
        }
    }
    return {};
}

// Audio: the game produces 16 bit stereo samples, SDL converts them to the
// hardware format.
SDL_AudioDeviceID g_audio_device = 0;
uint32_t g_sample_rate = 48000;

void open_audio_device() {
    SDL_AudioSpec desired{};
    desired.freq = static_cast<int>(g_sample_rate);
    desired.format = AUDIO_S16SYS;
    desired.channels = 2;
    desired.samples = 1024;
    desired.callback = nullptr;

    if (getenv("MACE_HEADLESS")) {
        fprintf(stderr, "[mace] headless mode: skipping audio init\n");
        return;
    }
    g_audio_device = SDL_OpenAudioDevice(nullptr, 0, &desired, nullptr,
                                         SDL_AUDIO_ALLOW_FREQUENCY_CHANGE | SDL_AUDIO_ALLOW_CHANNELS_CHANGE);
    if (g_audio_device == 0) {
        fprintf(stderr, "Failed to open audio device: %s\n", SDL_GetError());
        return;
    }
    SDL_PauseAudioDevice(g_audio_device, 0);
}

void queue_samples(int16_t* audio_data, size_t sample_count) {
    if (g_audio_device == 0) {
        return;
    }
    SDL_QueueAudio(g_audio_device, audio_data, sample_count * sizeof(int16_t));
}

size_t get_frames_remaining() {
    if (g_audio_device == 0) {
        return 0;
    }
    return SDL_GetQueuedAudioSize(g_audio_device) / (2 * sizeof(int16_t));
}

void set_frequency(uint32_t freq) {
    if (freq == 0 || freq == g_sample_rate) {
        return;
    }
    g_sample_rate = freq;
    if (g_audio_device != 0) {
        SDL_CloseAudioDevice(g_audio_device);
        g_audio_device = 0;
    }
    open_audio_device();
}

void poll_input() { poll_events(); }

bool get_input(int controller_num, uint16_t* buttons, float* x, float* y) {
    if (controller_num != 0) {
        return false;
    }
    *buttons = g_input.buttons;
    *x = g_input.stick_x;
    *y = g_input.stick_y;
    return true;
}

void set_rumble(int controller_num, bool rumble) {
    (void)controller_num;
    (void)rumble;
}

ultramodern::input::connected_device_info_t get_connected_device_info(int controller_num) {
    ultramodern::input::connected_device_info_t info{};
    if (controller_num == 0 && g_input.connected) {
        info.connected_device = ultramodern::input::Device::Controller;
        info.connected_pak = ultramodern::input::Pak::None;
    }
    else {
        info.connected_device = ultramodern::input::Device::None;
        info.connected_pak = ultramodern::input::Pak::None;
    }
    return info;
}

} // namespace


#include <signal.h>
#include <ucontext.h>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

// MMIO handling via SIGSEGV: the compiler sometimes bypasses MEM_W_PTR and
// emits direct loads for boot-code register accesses. This handler catches
// those faults and returns 0 for reads in the MMIO range.

static uint8_t* mmio_rdram = nullptr;
static size_t mmio_rdram_size = 0;

// Raw x86_64 register encoding (what instruction decoders produce)
// Maps to gregset_t indices via x86_grab_map below.
enum {
    X86_RAX = 0, X86_RCX = 1, X86_RDX = 2, X86_RBX = 3,
    X86_RSP = 4, X86_RBP = 5, X86_RSI = 6, X86_RDI = 7,
    X86_R8 = 8, X86_R9 = 9, X86_R10 = 10, X86_R11 = 11,
    X86_R12 = 12, X86_R13 = 13, X86_R14 = 14, X86_R15 = 15
};

// Map raw x86 register encoding to gregset_t index (Linux x86_64 layout).
static const int x86_gr_map[16] = {
    14, // RAX -> REG_RAX
    13, // RCX -> REG_RCX
    12, // RDX -> REG_RDX
    11, // RBX -> REG_RBX
    19, // RSP -> REG_RSP
    10, // RBP -> REG_RBP
    9,  // RSI -> REG_RSI
    8,  // RDI -> REG_RDI
    0,  // R8  -> REG_R8
    1,  // R9  -> REG_R9
    2,  // R10 -> REG_R10
    3,  // R11 -> REG_R11
    4,  // R12 -> REG_R12
    5,  // R13 -> REG_R13
    6,  // R14 -> REG_R14
    7,  // R15 -> REG_R15
};


// Decode a common load instruction, returning its length in bytes and the
// destination register index (gregset_t index), or -1 on failure.
static int decode_load(const uint8_t* rip, int* dest_reg) {
    int pos = 0;
    int rex_w = 0, rex_r = 0, rex_b = 0;

    // REX prefix (0x40-0x4F)
    if ((rip[pos] & 0xF0) == 0x40) {
        rex_w = (rip[pos] >> 3) & 1;
        rex_r = (rip[pos] >> 2) & 1;
        rex_b = rip[pos] & 1;
        pos++;
    }

    uint8_t op = rip[pos++];

    // REX.W + 0x63: MOVSLQ r64, r/m32  (7 bytes for [rbp+disp32])
    // 0x8B: MOV r64, r/m64
    if ((rex_w && op == 0x63) || op == 0x8B) {
        uint8_t modrm = rip[pos++];
        int mod = (modrm >> 6) & 3;
        int reg = ((modrm >> 3) & 7) | (rex_r << 3);
        int rm = (modrm & 7) | (rex_b << 3);

        if (mod == 3) return -1; // reg-reg, not a memory access

        if (rm == 4) pos++; // SIB byte

        if (mod == 0 && rm == 5) {
            // [rip+disp32] - but our case is [rbp+disp32] (mod==2, r/m==5)
            pos += 4;
        } else if (mod == 1) {
            pos += 1; // disp8
        } else if (mod == 2) {
            pos += 4; // disp32
        }

        *dest_reg = reg;
        return pos;
    }

    // 0x8A: MOV r8, r/m8  (byte load)
    if (op == 0x8A) {
        uint8_t modrm = rip[pos++];
        int reg = ((modrm >> 3) & 7) | (rex_r << 3);
        int rm = (modrm & 7) | (rex_b << 3);
        int mod = (modrm >> 6) & 3;

        if (mod == 3) return -1;
        if (rm == 4) pos++;
        if (mod == 0 && rm == 5) pos += 4;
        else if (mod == 1) pos += 1;
        else if (mod == 2) pos += 4;

        *dest_reg = reg;
        return pos;
    }

    return -1; // unrecognized
}

// Decode a store instruction (mov [mem], reg using opcode 0x89).
// Returns instruction length and sets *src_reg to the raw register encoding.
static int decode_store(const uint8_t* rip, int* src_reg) {
    int pos = 0;
    int rex_r = 0, rex_b = 0;

    if ((rip[pos] & 0xF0) == 0x40) {
        rex_r = (rip[pos] >> 2) & 1;
        rex_b = rip[pos] & 1;
        pos++;
    }

    uint8_t op = rip[pos++];
    if (op != 0x89) return -1; // not a 32-bit mov [mem], reg

    uint8_t modrm = rip[pos++];
    int mod = (modrm >> 6) & 3;
    int reg = ((modrm >> 3) & 7) | (rex_r << 3);
    int rm = (modrm & 7) | (rex_b << 3);

    if (mod == 3) return -1;
    if (rm == 4) pos++; // SIB
    if (mod == 0 && rm == 5) pos += 4;
    else if (mod == 1) pos += 1;
    else if (mod == 2) pos += 4;

    *src_reg = reg;
    return pos;
}

// PIF RAM base in kseg1. The boot code reads from here to get boot status.
// PIF RAM initialized to indicate "boot complete" status.
static uint8_t pif_ram[4] = { 0x01, 0x00, 0x00, 0x00 };

// Perform a DMA transfer between RDRAM and the cartridge ROM.
static void do_dma(uint8_t* rdram, uint32_t dram_addr, uint32_t cart_addr, uint32_t len, int to_rdram) {
    if (to_rdram) {
        // Cartridge -> RDRAM. ROM is big-endian; RDRAM is read as host-order (little-endian),
        // so byte-swap each 32-bit word during the copy.
        fprintf(stderr, "[dma] cart 0x%08X -> dram 0x%08X len 0x%X\n", cart_addr, dram_addr, len);
        uint32_t dram_off = dram_addr & 0x01FFFFFF;
        for (uint32_t i = 0; i + 3 < len; i += 4) {
            uint32_t cart_off = cart_addr + i;
            if (dram_off + i + 3 < 0x800000 && cart_off + 3 < g_rom_size) {
                // Big-endian ROM to little-endian RDRAM.
                rdram[dram_off + i + 0] = g_rom_data[cart_off + 3];
                rdram[dram_off + i + 1] = g_rom_data[cart_off + 2];
                rdram[dram_off + i + 2] = g_rom_data[cart_off + 1];
                rdram[dram_off + i + 3] = g_rom_data[cart_off + 0];
            }
        }
        fprintf(stderr, "[dma] first 4 bytes at dram_off=%X: %02X%02X%02X%02X\n",
            dram_off, rdram[dram_off], rdram[dram_off+1], rdram[dram_off+2], rdram[dram_off+3]);
    } else {
        // RDRAM -> cartridge (save path, no-op for ROM)
    }
}

// Return the emulated value for a 32-bit MMIO register read.
static uint32_t mmio_read(uint32_t addr) {
    // SP memory: return ROM data at the corresponding offset.
    if (addr >= 0xA0000000 && addr < 0xA0800000) {
        uint32_t offset = addr - 0xA0000000;
        if (g_rom_data && offset < g_rom_size) {
            // ROM is big-endian; read bytes and swap.
            uint8_t* p = g_rom_data + offset;
            uint32_t val = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
            return val;
        }
        return 0;
    }

    switch (addr) {
    case 0xA4800018: {
        // AI status - game polls this waiting for audio DMA idle
        uint32_t val = sp_regs.status;
        if (ai_status_delay > 0) { ai_status_delay--; val = 1; }
        return val;
    }
    case 0xA4040010: {
        uint32_t val = sp_regs.status;
        if (sp_status_delay > 0) { sp_status_delay--; val |= 1; }
        return val;
    }
    case 0xA4040000: return sp_regs.dram_addr;
    case 0xA4040004: return sp_regs.cart_addr;
    case 0xA4300008: return 0; // MI interrupt
    case 0xA450000C: {
        // AI status register
        uint32_t val = ai_regs.dram_addr ? 1 : 0; // busy if DMA pending
        if (ai_status_delay > 0) { ai_status_delay--; val = 1; }
        return val;
    }
    case 0xA4600010: {
        // PI status: report DMA briefly busy after a PIF command, then idle.
        // This lets the game see the DMA "complete".
        if (pi_status_delay > 0) {
            pi_status_delay--;
            if (g_mace_verbose) fprintf(stderr, "[mmio] PI status read = 1 (busy, delay=%d)\n", pi_status_delay);
            return 1; // busy
        }
        if (g_mace_verbose) fprintf(stderr, "[mmio] PI status read = 0 (idle)\n");
        return 0; // idle
    }
    case 0xA4600000: return pi_regs.dram_addr;
    case 0xA4600004: return pi_regs.cart_addr;
    case 0xBFC007FC: return *(uint32_t*)pif_ram;
    default: return 0;
    }
}

// Write to an MMIO register.
static void mmio_write(uint32_t addr, uint32_t value) {
    switch (addr) {
    case 0xA4040000: sp_regs.dram_addr = value; break;
    case 0xA4040004: sp_regs.cart_addr = value; break;
    case 0xA4040008: // SP_RD_LEN: cart -> RDRAM via SP
        do_dma(mmio_rdram, sp_regs.dram_addr, sp_regs.cart_addr, value & 0x0FFFFFFF, 1);
        break;
    case 0xA404000C: // SP_WR_LEN: RDRAM -> cart via SP
        do_dma(mmio_rdram, sp_regs.dram_addr, sp_regs.cart_addr, value & 0x0FFFFFFF, 0);
        break;
    case 0xA4600000: pi_regs.dram_addr = value; break;
    case 0xA4600004: pi_regs.cart_addr = value; break;
    case 0xA4600008: // PI_RD_LEN: cart -> RDRAM
        fprintf(stderr, "[pi] DMA cart 0x%08X -> dram 0x%08X len 0x%X\n", pi_regs.cart_addr, pi_regs.dram_addr, value & 0x0FFFFFFF);
        do_dma(mmio_rdram, pi_regs.dram_addr, pi_regs.cart_addr, value & 0x0FFFFFFF, 1);
        break;
    case 0xA460000C: // PI_WR_LEN: RDRAM -> cart
        do_dma(mmio_rdram, pi_regs.dram_addr, pi_regs.cart_addr, value & 0x0FFFFFFF, 0);
        break;
    // AI registers (audio interface)
    case 0xA4500000: ai_regs.dram_addr = value; break;
    case 0xA4500010: ai_regs.length = value; break;
    case 0xA4500008: // AI_CONTROL: start DMA
        if (value & 1) ai_status_delay = 2;
        break;
    case 0xA450000C: // AI_STATUS: write clears interrupt
        ai_status_delay = 0;
        break;
    case 0xBFC007FC:
        // Trace CIC command sequence during boot.
        fprintf(stderr, "[pif] write value=0x%08X pi_dram=0x%08X pi_cart=0x%08X\n",
            value, pi_regs.dram_addr, pi_regs.cart_addr);
        *(uint32_t*)pif_ram = value;
        // Parse CIC command and perform the requested DMA.
        // Command format: byte 0 = command type, bytes 1-3 = parameters.
        switch (value & 0xFF) {
        case 0x09: { // Boot/DMA command: copy cart data to RDRAM
            // The game expects the cartridge code section (file 0x1000, vram
            // 0x80000400) to be loaded into RDRAM at offset 0x400.
            uint32_t cart = 0x1000;  // ROM file offset of code section
            uint32_t dram = 0x400;   // RDRAM offset (0x80000400 - 0x80000000)
            uint32_t len = 0x7c6000; // Code section size (from symbols.toml)
            fprintf(stderr, "[pif] CIC cmd 0x09: DMA cart 0x%0X -> dram 0x%0X len 0x%0X\n",
                cart, dram, len);
            do_dma(mmio_rdram, dram, cart, len, 1);
            // Write success response.
            *(uint32_t*)pif_ram = 0x00000000;
            fprintf(stderr, "[pif] DMA done, first bytes: %02X %02X %02X %02X\n",
                mmio_rdram[0], mmio_rdram[1], mmio_rdram[2], mmio_rdram[3]);
            break;
        }
        default:
            // Unknown command: just acknowledge.
            *(uint32_t*)pif_ram = 0x00000000;
            break;
        }
        pi_status_delay = 3;
        break;
    }
}

static void mmio_segv(int sig, siginfo_t* info, void* ucontext) {
    ucontext_t* ctx = (ucontext_t*)ucontext;
    size_t fault_addr = (size_t)info->si_addr;
    uint8_t* rip = (uint8_t*)ctx->uc_mcontext.gregs[16];

    // Compute the game virtual address.
    // For RDRAM accesses: fault_addr = mmio_rdram + (game_addr - 0xFFFFFFFF80000000)
    // For MMIO/direct accesses: fault_addr = game_addr (the game accessed the address directly)
    uint32_t game_addr;
    if (fault_addr >= 0xFFFFFFFF80000000ULL) {
        // Direct game address access (MMIO)
        game_addr = (uint32_t)fault_addr;
    } else {
        // RDRAM access through mprotected region
        game_addr = (uint32_t)(fault_addr - (size_t)mmio_rdram + 0xFFFFFFFF80000000ULL);
    }

    fprintf(stderr, "[segv] fault=%p mmio_rdram=%p game_addr=0x%08X\n", (void*)fault_addr, mmio_rdram, game_addr);

    // Check if this is a known MMIO region.
    int is_mmio = 0;
    if (game_addr >= 0xA4040000 && game_addr < 0xA4040020) is_mmio = 1; // SP registers
    else if (game_addr >= 0xA4300000 && game_addr < 0xA4300010) is_mmio = 1; // MI registers
    else if (game_addr >= 0xA4400000 && game_addr < 0xA4400040) is_mmio = 1; // VI registers
    else if (game_addr >= 0xA4600000 && game_addr < 0xA4600020) is_mmio = 1; // PI registers
    else if (game_addr >= 0xA4800000 && game_addr < 0xA4800020) is_mmio = 1; // PIF/CIC
    else if (game_addr >= 0xA5000000 && game_addr < 0xA6000000) is_mmio = 1; // PI cart SRAM/DMA
    else if (game_addr >= 0xA0000000 && game_addr < 0xA0800000) is_mmio = 1; // SP memory
    else if (game_addr >= 0xB0000000 && game_addr < 0xC0000000) is_mmio = 1; // PIF ROM mirror / boot
    else if (game_addr >= 0xBFC00000 && game_addr <= 0xBFC007FF) is_mmio = 1; // PIF ROM/RAM (precise)

    if (!is_mmio) {
        // For any non-MMIO access, try to handle it gracefully.
        // This covers NULL pointer dereferences and other invalid addresses.
        int handled = 0;

        // Try to decode as a load instruction.
        int dest_reg = -1;
        int len = decode_load(rip, &dest_reg);
        if (len > 0 && dest_reg >= 0 && dest_reg <= 15) {
            uint32_t value = 0; // Return 0 for invalid reads
            int gr = x86_gr_map[dest_reg];
            if (gr != 19 /* RSP */ && gr != 16 /* RIP */) {
                ctx->uc_mcontext.gregs[gr] = value;
            }
            ctx->uc_mcontext.gregs[16 /* RIP */] += len;
            handled = 1;
        }

        // Try to decode as a store instruction.
        if (!handled) {
            int src_reg = -1;
            int store_len = decode_store(rip, &src_reg);
            if (store_len > 0 && src_reg >= 0 && src_reg <= 15) {
                // Ignore writes to invalid addresses
                ctx->uc_mcontext.gregs[16 /* RIP */] += store_len;
                handled = 1;
            }
        }

        // Try MOVSLQ (opcode 0x63)
        if (!handled) {
            uint8_t* rip2 = (uint8_t*)ctx->uc_mcontext.gregs[16];
            int pos = 0;
            int rex_r = 0, rex_b = 0;
            if ((rip2[pos] & 0xF0) == 0x40) {
                rex_r = (rip2[pos] >> 2) & 1;
                rex_b = rip2[pos] & 1;
                pos++;
            }
            if (rip2[pos++] == 0x63) {
                uint8_t modrm = rip2[pos++];
                int reg = ((modrm >> 3) & 7) | (rex_r << 3);
                int rm = (modrm & 7) | (rex_b << 3);
                int mod = (modrm >> 6) & 3;
                int gr = x86_gr_map[reg];
                if (gr != 19 && gr != 16) {
                    ctx->uc_mcontext.gregs[gr] = 0;
                }
                ctx->uc_mcontext.gregs[16] += pos;
                handled = 1;
            }
        }

        if (!handled) {
            fprintf(stderr, "[mmio] non-mmio game_addr=0x%08X - skipping instruction\n", game_addr);
            ctx->uc_mcontext.gregs[16 /* RIP */] += 2;
        }
        return;
    }

    int dest_reg = -1;
    int len = decode_load(rip, &dest_reg);

    if (len > 0 && dest_reg >= 0 && dest_reg <= 15) {
        uint32_t game_addr = (uint32_t)(fault_addr - (size_t)mmio_rdram + 0xFFFFFFFF80000000ULL);
        uint32_t value = mmio_read(game_addr);
        int gr = x86_gr_map[dest_reg];
        if (gr != 19 /* RSP */ && gr != 16 /* RIP */) {
            ctx->uc_mcontext.gregs[gr] = value;
        }
        ctx->uc_mcontext.gregs[16 /* RIP */] += len;
        return;
    }

    // Try to decode MOVSLQ (opcode 0x63) - sign-extend 32-bit to 64-bit.
    // The base register is encoded in the ModR/M byte. We need to read it
    // from the gregset to compute the game address.
    {
        uint8_t* rip = (uint8_t*)ctx->uc_mcontext.gregs[16];
        int pos = 0;
        int rex_r = 0, rex_b = 0;
        if ((rip[pos] & 0xF0) == 0x40) {
            rex_r = (rip[pos] >> 2) & 1;
            rex_b = rip[pos] & 1;
            pos++;
        }
        if (rip[pos++] == 0x63) {
            uint8_t modrm = rip[pos++];
            int reg = ((modrm >> 3) & 7) | (rex_r << 3);
            int rm = (modrm & 7) | (rex_b << 3);
            int mod = (modrm >> 6) & 3;
            int disp = 0;
            if (mod == 0 && rm == 5) { disp = (int)*(uint32_t*)(rip+pos); pos += 4; }
            else if (mod == 1) { disp = (int)(signed char)rip[pos]; pos += 1; }
            else if (mod == 2) { disp = (int)*(uint32_t*)(rip+pos); pos += 4; }
            // Read base register value.
            int gr = x86_gr_map[reg];
            uint64_t base = (rm == 5) ? 0 : ctx->uc_mcontext.gregs[x86_gr_map[rm]];
            uint32_t game_addr = (uint32_t)((base + disp) & 0xFFFFFFFF);
            if (reg >= 0 && reg <= 15) {
                uint32_t value = mmio_read(game_addr);
                if (gr != 19 && gr != 16) {
                    ctx->uc_mcontext.gregs[gr] = value;
                }
                ctx->uc_mcontext.gregs[16] += pos;
                return;
            }
        }
    }

    // Try to decode a store (write) instruction.
    int src_reg = -1;
    int store_len = decode_store(rip, &src_reg);
    if (store_len > 0 && src_reg >= 0 && src_reg <= 15) {
        // Write to MMIO: read source register and pass to emulated write.
        uint32_t game_addr = (uint32_t)(fault_addr - (size_t)mmio_rdram + 0xFFFFFFFF80000000ULL);
        uint32_t value = (uint32_t)ctx->uc_mcontext.gregs[x86_gr_map[src_reg]];
        mmio_write(game_addr, value);
        ctx->uc_mcontext.gregs[16 /* RIP */] += store_len;
        return;
    }

    // Unknown instruction - skip a small amount and hope for the best.
    ctx->uc_mcontext.gregs[16 /* RIP */] += 2;
}

static void install_mmio_handler() {
    // Find the rdram mapping from /proc/self/maps (largest anonymous rw region).
    FILE* f = fopen("/proc/self/maps", "r");
    if (!f) {
        fprintf(stderr, "[mmio] cannot open /proc/self/maps\n");
        return;
    }

    char line[512];
    uint8_t* best_base = nullptr;
    size_t best_size = 0;

    while (fgets(line, sizeof(line), f)) {
        uintptr_t start, end;
        char perms[5], path[256] = {0};
        int n = sscanf(line, "%lx-%lx %4s %*s %*s %*s %255[^\n]", &start, &end, perms, path);
        if (n >= 3 && perms[0] == 'r' && perms[1] == 'w' && path[0] == '\0') {
            size_t sz = end - start;
            if (sz > best_size) {
                best_size = sz;
                best_base = (uint8_t*)start;
            }
        }
    }
    fclose(f);

    if (!best_base) {
        fprintf(stderr, "[mmio] no rdram mapping found\n");
        return;
    }

    mmio_rdram = best_base;
    mmio_rdram_size = best_size;

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = mmio_segv;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, nullptr);
}
int main(int argc, char** argv) {
    std::filesystem::path rom_path;
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            printf("usage: %s [rom file]\n", argv[0]);
            return EXIT_SUCCESS;
        }
        if (!arg.empty() && arg[0] != '-') {
            rom_path = arg;
        }
    }

    // The recomp runtime keeps the game configuration (including a copy of the
    // ROM) next to the executable.
    std::filesystem::path config_path = std::filesystem::current_path() / "mace-config";
    if (const char* config_env = getenv("MACE_CONFIG")) {
        config_path = config_env;
    }
    std::error_code ec;
    std::filesystem::create_directories(config_path, ec);
    recomp::register_config_path(config_path);

    recomp::GameEntry game{};
    game.rom_hash = MACE_ROM_HASH;
    game.internal_name = MACE_INTERNAL_NAME;
    game.display_name = "Mace: The Dark Age (USA)";
    game.game_id = u8"MACE_GAME_ID";
    game.mod_game_id = MACE_MOD_GAME_ID;
    game.save_type = recomp::SaveType::None;
    game.is_enabled = true;
    game.entrypoint_address = (gpr)(int64_t)(int32_t)MACE_ENTRYPOINT;
    game.entrypoint = mace_entry;
    game.thread_create_callback = mace_thread_start;
    fprintf(stderr, "[mace] registered thread_create_callback\n");    recomp::register_game(game);

    // Copy the user supplied ROM into the config directory (this also
    // validates its hash).
    if (!rom_path.empty()) {
        recomp::RomValidationError result = recomp::select_rom(rom_path, u8"MACE_GAME_ID");
        if (result != recomp::RomValidationError::Good) {
            fprintf(stderr, "Failed to load ROM '%s' (error %d)\n", rom_path.string(), static_cast<int>(result));
            return EXIT_FAILURE;
        }
    }

    recomp::Version project_version{ 0, 1, 0, "" };

    // Make the recompiled functions known to the runtime.
    mace::register_overlays();

    recomp::rsp::callbacks_t rsp_callbacks{};
    ultramodern::renderer::callbacks_t renderer_callbacks{
        .create_render_context = mace::create_render_context,
    };
    ultramodern::gfx_callbacks_t gfx_callbacks{
        .create_gfx = create_gfx,
        .create_window = create_window,
        .update_gfx = update_gfx,
    };
    ultramodern::audio_callbacks_t audio_callbacks{
        .queue_samples = queue_samples,
        .get_frames_remaining = get_frames_remaining,
        .set_frequency = set_frequency,
    };
    ultramodern::input::callbacks_t input_callbacks{
        .poll_input = poll_input,
        .get_input = get_input,
        .set_rumble = set_rumble,
        .get_connected_device_info = get_connected_device_info,
    };

    // The runtime parses the command line to find the game to run.
    char* fake_argv[] = { argv[0], const_cast<char*>("--game"), const_cast<char*>(MACE_MOD_GAME_ID) };
    recomp::Configuration config{};
    config.argc = 3;
    config.argv = fake_argv;
    config.project_version = project_version;
    config.rsp_callbacks = rsp_callbacks;
    config.renderer_callbacks = renderer_callbacks;
    config.audio_callbacks = audio_callbacks;
    config.input_callbacks = input_callbacks;
    config.gfx_callbacks = gfx_callbacks;

    recomp::start(config);

    if (g_controller != nullptr) {
        SDL_GameControllerClose(g_controller);
    }
    if (g_window != nullptr) {
        SDL_DestroyWindow(g_window);
    }
    SDL_Quit();
    return EXIT_SUCCESS;
}
