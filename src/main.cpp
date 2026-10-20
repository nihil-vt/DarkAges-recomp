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

#include "SDL.h"

#include "ultramodern/ultramodern.hpp"
#include "librecomp/game.hpp"
#include "recomp.h"

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
uint32_t g_entry_address = 0x80001000;

// Wrapper around the game's entry point: the runtime calls this instead of the
// recompiled entry function so that the entry point can be changed without
// recompiling.
extern "C" void mace_entry(uint8_t* rdram, recomp_context* ctx) {
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
    recomp_func_t* func = get_function((int32_t)g_entry_address);
    func(rdram, ctx);
}

namespace {

// XXH3-64 of "Mace - The Dark Age (USA).z64".
constexpr uint64_t MACE_ROM_HASH = 0x46100E155439376AULL;
constexpr const char* MACE_INTERNAL_NAME = "MACE               "; // 19 chars, as in the ROM header
constexpr const char* MACE_GAME_ID = "mace.n64.us.1.0";
constexpr const char* MACE_MOD_GAME_ID = "mace";

constexpr uint32_t MACE_ENTRYPOINT = 0x80001000;

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
    recomp::register_game(game);

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
