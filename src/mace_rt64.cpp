// RT64 renderer adapter for the Mace recomp.
//
// Implements ultramodern's renderer callbacks on top of the RT64 renderer.
// Adapted from the equivalent glue in Zelda64Recomp, stripped down to what
// the Mace port needs (no texture packs, no config UI).

#include "mace_rt64.h"

#include <cassert>
#include <cstdio>
#include <cstdlib>

#define HLSL_CPU
#include "hle/rt64_application.h"

#include "ultramodern/config.hpp"
#include "ultramodern/renderer_context.hpp"

namespace {

static uint8_t DMEM[0x1000];
static uint8_t IMEM[0x1000];

static unsigned int MI_INTR_REG = 0;
static unsigned int DPC_START_REG = 0;
static unsigned int DPC_END_REG = 0;
static unsigned int DPC_CURRENT_REG = 0;
static unsigned int DPC_STATUS_REG = 0;
static unsigned int DPC_CLOCK_REG = 0;
static unsigned int DPC_BUFBUSY_REG = 0;
static unsigned int DPC_PIPEBUSY_REG = 0;
static unsigned int DPC_TMEM_REG = 0;

void dummy_check_interrupts() {}

class RT64Context : public ultramodern::renderer::RendererContext {
public:
    RT64Context(uint8_t* rdram, ultramodern::renderer::WindowHandle window_handle, bool developer_mode) {
        static unsigned char dummy_rom_header[0x40];
        memset(dummy_rom_header, 0, sizeof(dummy_rom_header));

        RT64::Application::Core appCore{};
#if defined(_WIN32)
        appCore.window = window_handle.window;
#elif defined(__linux__) || defined(__ANDROID__)
        appCore.window = window_handle;
#elif defined(__APPLE__)
        appCore.window.window = window_handle.window;
        appCore.window.view = window_handle.view;
#endif
        appCore.checkInterrupts = dummy_check_interrupts;
        appCore.HEADER = dummy_rom_header;
        appCore.RDRAM = rdram;
        appCore.DMEM = DMEM;
        appCore.IMEM = IMEM;
        appCore.MI_INTR_REG = &MI_INTR_REG;
        appCore.DPC_START_REG = &DPC_START_REG;
        appCore.DPC_END_REG = &DPC_END_REG;
        appCore.DPC_CURRENT_REG = &DPC_CURRENT_REG;
        appCore.DPC_STATUS_REG = &DPC_STATUS_REG;
        appCore.DPC_CLOCK_REG = &DPC_CLOCK_REG;
        appCore.DPC_BUFBUSY_REG = &DPC_BUFBUSY_REG;
        appCore.DPC_PIPEBUSY_REG = &DPC_PIPEBUSY_REG;
        appCore.DPC_TMEM_REG = &DPC_TMEM_REG;

        ultramodern::renderer::ViRegs* vi_regs = ultramodern::renderer::get_vi_regs();
        appCore.VI_STATUS_REG = &vi_regs->VI_STATUS_REG;
        appCore.VI_ORIGIN_REG = &vi_regs->VI_ORIGIN_REG;
        appCore.VI_WIDTH_REG = &vi_regs->VI_WIDTH_REG;
        appCore.VI_INTR_REG = &vi_regs->VI_INTR_REG;
        appCore.VI_V_CURRENT_LINE_REG = &vi_regs->VI_V_CURRENT_LINE_REG;
        appCore.VI_TIMING_REG = &vi_regs->VI_TIMING_REG;
        appCore.VI_V_SYNC_REG = &vi_regs->VI_V_SYNC_REG;
        appCore.VI_H_SYNC_REG = &vi_regs->VI_H_SYNC_REG;
        appCore.VI_LEAP_REG = &vi_regs->VI_LEAP_REG;
        appCore.VI_H_START_REG = &vi_regs->VI_H_START_REG;
        appCore.VI_V_START_REG = &vi_regs->VI_V_START_REG;
        appCore.VI_V_BURST_REG = &vi_regs->VI_V_BURST_REG;
        appCore.VI_X_SCALE_REG = &vi_regs->VI_X_SCALE_REG;
        appCore.VI_Y_SCALE_REG = &vi_regs->VI_Y_SCALE_REG;

        RT64::ApplicationConfiguration appConfig;
        appConfig.useConfigurationFile = false;

        app = std::make_unique<RT64::Application>(appCore, appConfig);

        const auto& cur_config = ultramodern::renderer::get_graphics_config();
        app->userConfig.resolution = RT64::UserConfiguration::Resolution::WindowIntegerScale;
        app->userConfig.downsampleMultiplier = 1;
        app->userConfig.aspectRatio = RT64::UserConfiguration::AspectRatio::Original;
        app->userConfig.antialiasing = RT64::UserConfiguration::Antialiasing::None;
        app->userConfig.refreshRate = RT64::UserConfiguration::RefreshRate::Original;
        app->userConfig.internalColorFormat = RT64::UserConfiguration::InternalColorFormat::Standard;
        app->userConfig.displayBuffering = RT64::UserConfiguration::DisplayBuffering::Triple;
        app->userConfig.developerMode = developer_mode;
        app->enhancementConfig.f3dex.forceBranch = true;
        app->enhancementConfig.textureLOD.scale = true;

        uint32_t thread_id = 0;
#ifdef _WIN32
        thread_id = window_handle.thread_id;
#endif
        setup_result = map_setup_result(app->setup(thread_id));
        chosen_api = map_graphics_api(app->chosenGraphicsAPI);
        if (setup_result != ultramodern::renderer::SetupResult::Success) {
            app = nullptr;
            return;
        }

        app->setFullScreen(cur_config.wm_option == ultramodern::renderer::WindowMode::Fullscreen);
    }

    ~RT64Context() override = default;

    bool valid() override { return setup_result == ultramodern::renderer::SetupResult::Success; }

    bool update_config(const ultramodern::renderer::GraphicsConfig& old_config,
                       const ultramodern::renderer::GraphicsConfig& new_config) override {
        if (old_config == new_config || app == nullptr) {
            return false;
        }
        if (new_config.wm_option != old_config.wm_option) {
            app->setFullScreen(new_config.wm_option == ultramodern::renderer::WindowMode::Fullscreen);
        }
        return true;
    }

    void enable_instant_present() override {
        app->enhancementConfig.presentation.mode = RT64::EnhancementConfiguration::Presentation::Mode::PresentEarly;
        app->updateEnhancementConfig();
    }

    void send_dl(const OSTask* task) override {
        app->state->rsp->reset();
        app->interpreter->loadUCodeGBI(task->t.ucode & 0x3FFFFFF, task->t.ucode_data & 0x3FFFFFF, true);
        app->processDisplayLists(app->core.RDRAM, task->t.data_ptr & 0x3FFFFFF, 0, true);
    }

    void send_dummy_workload(uint32_t fb_address) override {
        // The game asked for a frame without a display list (e.g. a blit).
        // RT64 has no equivalent, so nothing is rendered for it.
        (void)fb_address;
    }

    void update_screen() override { app->updateScreen(); }

    void shutdown() override {
        if (app != nullptr) {
            app->end();
        }
    }

    uint32_t get_display_framerate() const override { return app->presentQueue->ext.sharedResources->swapChainRate; }

    float get_resolution_scale() const override {
        constexpr int ReferenceHeight = 240;
        if (app->userConfig.resolution == RT64::UserConfiguration::Resolution::WindowIntegerScale) {
            if (app->sharedQueueResources->swapChainHeight > 0) {
                return std::max(float((app->sharedQueueResources->swapChainHeight + ReferenceHeight - 1) / ReferenceHeight), 1.0f);
            }
        }
        return 1.0f;
    }

private:
    static ultramodern::renderer::SetupResult map_setup_result(RT64::Application::SetupResult rt64_result) {
        switch (rt64_result) {
            case RT64::Application::SetupResult::Success:
                return ultramodern::renderer::SetupResult::Success;
            case RT64::Application::SetupResult::DynamicLibrariesNotFound:
                return ultramodern::renderer::SetupResult::DynamicLibrariesNotFound;
            case RT64::Application::SetupResult::InvalidGraphicsAPI:
                return ultramodern::renderer::SetupResult::InvalidGraphicsAPI;
            case RT64::Application::SetupResult::GraphicsAPINotFound:
                return ultramodern::renderer::SetupResult::GraphicsAPINotFound;
            case RT64::Application::SetupResult::GraphicsDeviceNotFound:
                return ultramodern::renderer::SetupResult::GraphicsDeviceNotFound;
        }
        fprintf(stderr, "Unhandled `RT64::Application::SetupResult`\n");
        assert(false);
        std::exit(EXIT_FAILURE);
    }

    static ultramodern::renderer::GraphicsApi map_graphics_api(RT64::UserConfiguration::GraphicsAPI api) {
        switch (api) {
            case RT64::UserConfiguration::GraphicsAPI::D3D12:
                return ultramodern::renderer::GraphicsApi::D3D12;
            case RT64::UserConfiguration::GraphicsAPI::Vulkan:
                return ultramodern::renderer::GraphicsApi::Vulkan;
            case RT64::UserConfiguration::GraphicsAPI::Metal:
                return ultramodern::renderer::GraphicsApi::Metal;
            case RT64::UserConfiguration::GraphicsAPI::Automatic:
                return ultramodern::renderer::GraphicsApi::Auto;
        }
        fprintf(stderr, "Unhandled `RT64::UserConfiguration::GraphicsAPI`\n");
        assert(false);
        std::exit(EXIT_FAILURE);
    }

    std::unique_ptr<RT64::Application> app;
};

} // namespace

namespace mace {

std::unique_ptr<ultramodern::renderer::RendererContext> create_render_context(
    uint8_t* rdram, ultramodern::renderer::WindowHandle window_handle, bool developer_mode) {
    return std::make_unique<RT64Context>(rdram, window_handle, developer_mode);
}

} // namespace mace
