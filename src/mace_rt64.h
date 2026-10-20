// RT64 renderer adapter for the Mace recomp.
//
// Implements ultramodern's renderer callbacks on top of the RT64 renderer.
// Adapted from the equivalent glue in Zelda64Recomp, stripped down to what
// the Mace port needs (no texture packs, no config UI).

#pragma once

#include <cstdint>
#include <memory>

#include "ultramodern/renderer_context.hpp"

namespace mace {
    // Creates the RT64 backed renderer context used by the recomp runtime.
    std::unique_ptr<ultramodern::renderer::RendererContext> create_render_context(
        uint8_t* rdram, ultramodern::renderer::WindowHandle window_handle, bool developer_mode);
}
