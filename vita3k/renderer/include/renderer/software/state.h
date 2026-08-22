// Vita3K emulator project
// Copyright (C) 2026 Vita3K team
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License along
// with this program; if not, write to the Free Software Foundation, Inc.,
// 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.

#pragma once

#include <renderer/gl/overlay_renderer.h>
#include <renderer/gl/screen_render.h>
#include <renderer/software/surface_cache.h>
#include <renderer/software/texture.h>
#include <renderer/software/types.h>
#include <renderer/state.h>

#include <atomic>
#include <chrono>
#include <string>
#include <string_view>
#include <vector>

namespace renderer::software {

class Rasterizer;

// TEMPORARY: why draws do or do not reach the rasterizer. Every early exit in
// the draw path bumps one of these, so a single reported line says where the
// frames are being lost instead of leaving it to guesswork.
struct SWDrawCounters {
    std::atomic<uint64_t> entered{ 0 };
    std::atomic<uint64_t> no_color_target{ 0 };
    std::atomic<uint64_t> no_program{ 0 };
    std::atomic<uint64_t> mask_skipped{ 0 };
    std::atomic<uint64_t> shader_invalid{ 0 };
    std::atomic<uint64_t> no_indices{ 0 };
    std::atomic<uint64_t> no_primitives{ 0 };
    std::atomic<uint64_t> rasterized{ 0 };
    std::atomic<uint64_t> primitives{ 0 };
    std::atomic<uint64_t> pixels{ 0 };
    std::atomic<uint64_t> nonzero_w{ 0 };

    void reset() {
        entered = 0;
        no_color_target = 0;
        no_program = 0;
        mask_skipped = 0;
        shader_invalid = 0;
        no_indices = 0;
        no_primitives = 0;
        rasterized = 0;
        primitives = 0;
        pixels = 0;
        nonzero_w = 0;
    }
};

// The CPU renderer. It rasterizes into the guest color surface and only borrows
// OpenGL to get the finished frame onto the window, which is why it reuses the
// screen and overlay renderers of the GL backend verbatim.
struct SWState : public renderer::State {
    SWState();
    ~SWState() override;

    SWTextureCache texture_cache;
    SWSurfaceCache surface_cache;
    SWShaderCache shader_cache;

    gl::ScreenRenderer screen_renderer;
    gl::OverlayRenderer overlay_renderer;

    std::unique_ptr<Rasterizer> rasterizer;

    bool context_is_current = false;
    // Name reported in the UI, built once from the host CPU.
    std::string gpu_name;
    // Guest memory, kept from late_init so the presentation path can reach the
    // frame the rasterizer wrote without being handed a MemState every time.
    MemState *mem_state = nullptr;

    SWDrawCounters counters;
    std::chrono::steady_clock::time_point last_counter_report{};

    bool init() override;
    void cleanup() override;
    void late_init(const Config &cfg, const std::string_view game_id, MemState &mem) override;

    TextureCache *get_texture_cache() override {
        return &texture_cache;
    }

    void render_frame(DisplayState &display, const GxmState &gxm, MemState &mem) override;
    void swap_window() override;
    bool set_current() override;
    void done_current() override;
    std::vector<uint32_t> dump_frame(DisplayState &display, uint32_t &width, uint32_t &height) override;

    int get_supported_filters() override;
    void set_screen_filter(const std::string_view &filter) override;
    int get_max_anisotropic_filtering() override;
    void set_anisotropic_filtering(int anisotropic_filtering) override;
    int get_max_2d_texture_width() override;

    std::string_view get_gpu_name() override;

    void precompile_shader(const ShadersHash &hash) override;
    void preclose_action() override;
};

} // namespace renderer::software
