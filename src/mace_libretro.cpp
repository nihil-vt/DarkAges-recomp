// Mace: The Dark Age - Libretro Frontend
// Loads a libretro N64 core and plays via SDL2.

#include <SDL2/SDL.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <dlfcn.h>

#include "libretro.h"

typedef void* LibHandle;
#define DL_OPEN(p) dlopen(p, RTLD_LOCAL | RTLD_LAZY)
#define DL_SYM(h, n) dlsym(h, n)
#define DL_CLOSE(h) dlclose(h)

// --- Logging ----------------------------------------------------------------

static void log_cb(enum retro_log_level level, const char *fmt, ...) {
    (void)level;
    va_list va;
    va_start(va, fmt);
    fprintf(stderr, "[core] ");
    vfprintf(stderr, fmt, va);
    fprintf(stderr, "\n");
    va_end(va);
}

// --- State ------------------------------------------------------------------

static struct State {
    LibHandle core = nullptr;

    void (*retro_init)(void) = nullptr;
    void (*retro_deinit)(void) = nullptr;
    void (*retro_get_system_av_info)(struct retro_system_av_info*) = nullptr;
    void (*retro_set_environment)(retro_environment_t) = nullptr;
    void (*retro_set_video_refresh)(retro_video_refresh_t) = nullptr;
    void (*retro_set_audio_sample)(retro_audio_sample_t) = nullptr;
    void (*retro_set_audio_sample_batch)(retro_audio_sample_batch_t) = nullptr;
    void (*retro_set_input_poll)(retro_input_poll_t) = nullptr;
    void (*retro_set_input_state)(retro_input_state_t) = nullptr;
    void (*retro_run)(void) = nullptr;
    bool (*retro_load_game)(const struct retro_game_info*) = nullptr;
    void (*retro_unload_game)(void) = nullptr;

    SDL_Window *window = nullptr;
    SDL_Renderer *renderer = nullptr;
    SDL_Texture *texture = nullptr;
    SDL_AudioDeviceID audio = 0;

    unsigned tex_w = 0, tex_h = 0;

    bool keys[SDL_NUM_SCANCODES] = {};
    bool running = true;
    bool hw_render = false;

    std::vector<uint8_t> rom;
    std::string rom_path;
    std::string sys_dir;
} g;

// --- Environment ------------------------------------------------------------

static bool on_environ(unsigned cmd, void *data) {
    switch (cmd) {
    case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY: {
        auto dir = (const char **)data;
        *dir = g.sys_dir.c_str();
        return true;
    }
    case RETRO_ENVIRONMENT_GET_LOG_INTERFACE: {
        auto cb = (struct retro_log_callback *)data;
        cb->log = log_cb;
        return true;
    }
    case RETRO_ENVIRONMENT_SET_VARIABLES:
        return true;
    case RETRO_ENVIRONMENT_GET_VARIABLE: {
        auto var = (struct retro_variable *)data;
        if (var && var->key) {
            if (strcmp(var->key, "mupen64plus-rdp-plugin") == 0) {
                static const char *val = "angrylion";
                var->value = val;
            } else if (strcmp(var->key, "mupen64plus-cpucore") == 0) {
                static const char *val = "dynamic_recompiler";
                var->value = val;
            } else if (strcmp(var->key, "mupen64plus-angrylion-multithread") == 0) {
                static const char *val = "all threads";
                var->value = val;
            } else {
                var->value = nullptr;
            }
        }
        return true;
    }
    case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
        *(bool *)data = false;
        return true;
    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT: {
        auto fmt = *(const enum retro_pixel_format *)data;
        fprintf(stderr, "[env] pixel format: %d\n", fmt);
        return true;
    }
    case RETRO_ENVIRONMENT_GET_CAN_DUPE:
        *(bool *)data = true;
        return true;
    case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS:
        return true;
    case RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME:
        return false;
    case RETRO_ENVIRONMENT_SET_FRAME_TIME_CALLBACK:
        return true;
    case RETRO_ENVIRONMENT_SET_AUDIO_CALLBACK:
        return true;
    case RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO:
        return true;
    case RETRO_ENVIRONMENT_SET_GEOMETRY:
        return true;
    case RETRO_ENVIRONMENT_SET_CONTROLLER_INFO:
        return true;
    case RETRO_ENVIRONMENT_SET_MEMORY_MAPS:
        return true;
    case RETRO_ENVIRONMENT_SET_KEYBOARD_CALLBACK:
        return true;
    case RETRO_ENVIRONMENT_SET_HW_RENDER: {
        auto cb = (struct retro_hw_render_callback *)data;
        cb->context_reset = []() {};
        cb->context_destroy = []() {};
        cb->get_current_framebuffer = []() -> uintptr_t { return 0; };
        cb->get_proc_address = [](const char *s) -> retro_proc_address_t {
            return (retro_proc_address_t)SDL_GL_GetProcAddress(s);
        };
        cb->depth = false;
        cb->stencil = false;
        cb->bottom_left_origin = true;
        cb->version_major = 3;
        cb->version_minor = 3;
        cb->cache_context = false;
        cb->context_type = RETRO_HW_CONTEXT_OPENGL;
        g.hw_render = true;
        return true;
    }
    default:
        return false;
    }
}

// --- Video ------------------------------------------------------------------

static void on_video(const void *data, unsigned w, unsigned h, size_t pitch) {
    static int frame_count = 0;
    if (!data) return;

    frame_count++;
    if (frame_count <= 3 || frame_count % 30 == 0)
        fprintf(stderr, "[video] frame %d: %ux%u\n", frame_count, w, h);

    if (g.hw_render) return;
    if (w == 0 || h == 0) return;

    if (!g.texture || w != g.tex_w || h != g.tex_h) {
        if (g.texture) SDL_DestroyTexture(g.texture);
        g.texture = SDL_CreateTexture(g.renderer, SDL_PIXELFORMAT_RGB565,
                                      SDL_TEXTUREACCESS_STREAMING, w, h);
        g.tex_w = w;
        g.tex_h = h;
    }

    if (g.texture) {
        SDL_UpdateTexture(g.texture, nullptr, data, (int)pitch);
        SDL_RenderClear(g.renderer);
        SDL_RenderCopy(g.renderer, g.texture, nullptr, nullptr);
        SDL_RenderPresent(g.renderer);
    }
}

// --- Audio ------------------------------------------------------------------

static void on_audio_sample(int16_t l, int16_t r) { (void)l; (void)r; }

static size_t on_audio_batch(const int16_t *data, size_t frames) {
    if (g.audio && data && frames)
        SDL_QueueAudio(g.audio, data, frames * 2 * sizeof(int16_t));
    return frames;
}

// --- Input ------------------------------------------------------------------

static void on_poll(void) {}

static int16_t on_input(unsigned port, unsigned device, unsigned idx, unsigned id) {
    (void)idx;
    if (port != 0 || device != RETRO_DEVICE_JOYPAD) return 0;
    switch (id) {
    case RETRO_DEVICE_ID_JOYPAD_B:      return g.keys[SDL_SCANCODE_Z];
    case RETRO_DEVICE_ID_JOYPAD_Y:      return g.keys[SDL_SCANCODE_X];
    case RETRO_DEVICE_ID_JOYPAD_SELECT: return g.keys[SDL_SCANCODE_RSHIFT];
    case RETRO_DEVICE_ID_JOYPAD_START:  return g.keys[SDL_SCANCODE_RETURN];
    case RETRO_DEVICE_ID_JOYPAD_UP:     return g.keys[SDL_SCANCODE_UP];
    case RETRO_DEVICE_ID_JOYPAD_DOWN:   return g.keys[SDL_SCANCODE_DOWN];
    case RETRO_DEVICE_ID_JOYPAD_LEFT:   return g.keys[SDL_SCANCODE_LEFT];
    case RETRO_DEVICE_ID_JOYPAD_RIGHT:  return g.keys[SDL_SCANCODE_RIGHT];
    case RETRO_DEVICE_ID_JOYPAD_A:      return g.keys[SDL_SCANCODE_C];
    case RETRO_DEVICE_ID_JOYPAD_X:      return g.keys[SDL_SCANCODE_V];
    case RETRO_DEVICE_ID_JOYPAD_L:      return g.keys[SDL_SCANCODE_Q];
    case RETRO_DEVICE_ID_JOYPAD_R:      return g.keys[SDL_SCANCODE_E];
    }
    return 0;
}

// --- Core binding ------------------------------------------------------------

#define BIND(fn) g.fn = decltype(g.fn)(DL_SYM(g.core, #fn))

static bool load_core(const char *path) {
    g.core = DL_OPEN(path);
    if (!g.core) return false;
    BIND(retro_init); BIND(retro_deinit); BIND(retro_get_system_av_info);
    BIND(retro_set_environment); BIND(retro_set_video_refresh);
    BIND(retro_set_audio_sample); BIND(retro_set_audio_sample_batch);
    BIND(retro_set_input_poll); BIND(retro_set_input_state);
    BIND(retro_run); BIND(retro_load_game); BIND(retro_unload_game);
    return g.retro_init && g.retro_load_game && g.retro_run;
}

static bool load_rom(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    size_t sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    g.rom.resize(sz);
    if (fread(g.rom.data(), 1, sz, f) != sz) { fclose(f); return false; }
    fclose(f);
    g.rom_path = path;
    return true;
}

// --- Main --------------------------------------------------------------------

int main(int argc, char **argv) {
    const char *core = argc > 1 ? argv[1] : "cores/mupen64plus_next_libretro.so";
    const char *rom  = argc > 2 ? argv[2] : "roms/Mace - The Dark Age (USA).z64";

    fprintf(stderr, "[mace] core: %s\n[mace] rom: %s\n", core, rom);

    if (!load_core(core)) return 1;
    if (!load_rom(rom)) return 1;

    SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO);

    g.window = SDL_CreateWindow("Mace: The Dark Age",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 960, 720,
        SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);

    if (!g.window) return 1;

    g.renderer = SDL_CreateRenderer(g.window, -1,
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!g.renderer) return 1;

    SDL_AudioSpec want = {};
    want.freq = 48000; want.format = AUDIO_S16SYS;
    want.channels = 2; want.samples = 1024;
    g.audio = SDL_OpenAudioDevice(nullptr, 0, &want, nullptr, 0);
    if (g.audio) SDL_PauseAudioDevice(g.audio, 0);

    g.sys_dir = ".";

    g.retro_set_environment(on_environ);
    g.retro_set_video_refresh(on_video);
    g.retro_set_audio_sample(on_audio_sample);
    g.retro_set_audio_sample_batch(on_audio_batch);
    g.retro_set_input_poll(on_poll);
    g.retro_set_input_state(on_input);

    g.retro_init();

    retro_game_info info = {};
    info.path = g.rom_path.c_str();
    info.data = g.rom.data();
    info.size = g.rom.size();

    if (!g.retro_load_game(&info)) return 1;

    retro_system_av_info av = {};
    g.retro_get_system_av_info(&av);
    fprintf(stderr, "[mace] %ux%u @ %.1f fps\n",
            av.geometry.base_width, av.geometry.base_height, av.timing.fps);

    while (g.running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) g.running = false;
            else if (e.type == SDL_KEYDOWN) {
                if (e.key.keysym.scancode == SDL_SCANCODE_ESCAPE) g.running = false;
                if (e.key.keysym.scancode < SDL_NUM_SCANCODES)
                    g.keys[e.key.keysym.scancode] = true;
            } else if (e.type == SDL_KEYUP) {
                if (e.key.keysym.scancode < SDL_NUM_SCANCODES)
                    g.keys[e.key.keysym.scancode] = false;
            }
        }

        g.retro_run();
    }

    g.retro_unload_game();
    g.retro_deinit();

    if (g.audio) SDL_CloseAudioDevice(g.audio);
    if (g.texture) SDL_DestroyTexture(g.texture);
    if (g.renderer) SDL_DestroyRenderer(g.renderer);
    if (g.window) SDL_DestroyWindow(g.window);
    SDL_Quit();
    if (g.core) DL_CLOSE(g.core);

    return 0;
}
