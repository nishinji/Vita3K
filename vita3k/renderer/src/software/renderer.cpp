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

#include <renderer/software/functions.h>
#include <renderer/software/rasterizer.h>
#include <renderer/software/state.h>

#include <config/state.h>
#include <display/state.h>
#include <glutil/gl.h>
#include <mem/state.h>
#include <overlay/display_manager.h>
#include <renderer/state.h>
#include <shader/spirv_recompiler.h>
#include <util/log.h>

#include <thread>

namespace renderer::software {

SWState::SWState()
    : texture_cache(*this) {
}
SWState::~SWState() = default;

bool SWState::init() {
    if (!screen_renderer.init(static_assets)) {
        LOG_ERROR("Software renderer: failed to initialize the screen renderer");
        return false;
    }

    init_overlay_font_dirs();

    if (!overlay_renderer.init(static_assets, vita_fs_path, sys_lang)) {
        LOG_WARN("Software renderer: failed to initialize the overlay renderer, overlays will be disabled");
    }

    rasterizer = std::make_unique<Rasterizer>();

    shader_version = fmt::format("v{}", shader::CURRENT_VERSION);
    gpu_name = fmt::format("Software renderer ({} threads)", std::max(1u, std::thread::hardware_concurrency()));

    return true;
}

void SWState::late_init(const Config &cfg, const std::string_view game_id, MemState &mem) {
    mem_state = &mem;
    texture_cache.init(true, texture_folder(), game_id);
}

void SWState::render_frame(DisplayState &display, const GxmState &gxm, MemState &mem) {
    should_display = false;

    DisplayFrameInfo display_frame;
    {
        std::lock_guard<std::mutex> guard(display.display_info_mutex);
        display_frame = display.next_rendered_frame;
    }

    const bool has_overlays = overlay_manager && overlay_manager->has_visible();
    if (!display_frame.base && !has_overlays)
        return;

    SceFVector2 vp_pos = { 0.0f, 0.0f };
    SceFVector2 vp_size = { 0.0f, 0.0f };

    const GLuint default_fbo = frame->default_fbo();
    const float fb_w = static_cast<float>(frame->drawable_width());
    const float fb_h = static_cast<float>(frame->drawable_height());

    if (fb_h > 0.0f) {
        const float window_aspect = fb_w / fb_h;
        constexpr float vita_aspect = static_cast<float>(DEFAULT_RES_WIDTH) / DEFAULT_RES_HEIGHT;
        const bool pixel_perfect = fullscreen_hd_res_pixel_perfect && fullscreen
            && !(static_cast<int>(fb_w) % DEFAULT_RES_WIDTH)
            && !(static_cast<int>(fb_h) % (DEFAULT_RES_HEIGHT - 4));

        if (stretch_the_display_area && !pixel_perfect) {
            vp_pos = { 0.0f, 0.0f };
            vp_size = { fb_w, fb_h };
        } else if ((window_aspect > vita_aspect) && !pixel_perfect) {
            vp_size.x = fb_h * vita_aspect;
            vp_size.y = fb_h;
            vp_pos.x = (fb_w - vp_size.x) / 2.0f;
            vp_pos.y = 0.0f;
        } else {
            vp_size.x = fb_w;
            vp_size.y = fb_w / vita_aspect;
            vp_pos.x = 0.0f;
            vp_pos.y = (fb_h - vp_size.y) / 2.0f;
        }
    }

    // Store the viewport so the touch code can map window coordinates back.
    display.viewport_drawable_w = static_cast<int>(fb_w);
    display.viewport_drawable_h = static_cast<int>(fb_h);
    display.viewport_x = vp_pos.x;
    display.viewport_y = vp_pos.y;
    display.viewport_w = vp_size.x;
    display.viewport_h = vp_size.y;

    if (display_frame.base) {
        // The rasterizer wrote the frame into guest memory, so presenting is
        // always a plain upload; there is no host surface to source from.
        const void *pixels = display_frame.base.cast<void>().get(mem);

        // TEMPORARY: one line that says what the draw path did and what reached
        // the window, reported on a timer so it lands in the tail of the log.
        const auto now = std::chrono::steady_clock::now();
        if (now - last_counter_report >= std::chrono::seconds(2)) {
            last_counter_report = now;

            const uint32_t *words = static_cast<const uint32_t *>(pixels);
            const size_t center = static_cast<size_t>(display_frame.image_size.y / 2) * display_frame.pitch
                + display_frame.image_size.x / 2;

            LOG_INFO("SW frame: draws={} (no_surface={} no_prog={} mask={} bad_shader={} no_idx={} no_prims={})"
                     " rasterized={} prims={} pixels={} w!=0={} | present base=0x{:08X} pitch={} {}x{}"
                     " first=0x{:08X} center=0x{:08X}",
                counters.entered.load(), counters.no_color_target.load(), counters.no_program.load(),
                counters.mask_skipped.load(), counters.shader_invalid.load(), counters.no_indices.load(),
                counters.no_primitives.load(), counters.rasterized.load(), counters.primitives.load(),
                counters.pixels.load(), counters.nonzero_w.load(),
                display_frame.base.address(), display_frame.pitch, display_frame.image_size.x,
                display_frame.image_size.y, pixels ? words[0] : 0u, pixels ? words[center] : 0u);

            counters.reset();
        }
        GLint last_texture = 0;
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &last_texture);

        const GLuint texture = screen_renderer.get_resident_texture();
        glBindTexture(GL_TEXTURE_2D, texture);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, display_frame.pitch);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, display_frame.image_size.x, display_frame.image_size.y,
            0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        glBindTexture(GL_TEXTURE_2D, last_texture);

        const SceFVector2 texture_size = {
            static_cast<float>(display_frame.image_size.x),
            static_cast<float>(display_frame.image_size.y)
        };

        screen_renderer.render(vp_pos, vp_size, nullptr, texture, texture_size, default_fbo);
    }

    update_overlays();
    if (overlay_manager && overlay_manager->has_visible()) {
        overlay_renderer.render(*overlay_manager,
            vp_pos.x, vp_pos.y, vp_size.x, vp_size.y,
            fb_w, fb_h, default_fbo);
    }
}

void SWState::swap_window() {
    const int pending = pending_vsync.exchange(-1, std::memory_order_relaxed);
    if (pending >= 0 && !frame->set_vsync(pending != 0))
        LOG_WARN("Software renderer: failed to update the swap interval");

    frame->swap_buffers();
}

bool SWState::set_current() {
    if (context_is_current && (frame->drawable_width() <= 0 || frame->drawable_height() <= 0))
        done_current();

    if (context_is_current)
        return true;

    if (!frame->make_current()) {
        LOG_ERROR("Software renderer: set_current failed");
        context_is_current = false;
        return false;
    }

    context_is_current = true;
    return true;
}

void SWState::done_current() {
    frame->done_current();
    context_is_current = false;
}

std::vector<uint32_t> SWState::dump_frame(DisplayState &display, uint32_t &width, uint32_t &height) {
    DisplayFrameInfo frame_info;
    {
        std::lock_guard<std::mutex> guard(display.display_info_mutex);
        frame_info = display.next_rendered_frame;
    }

    width = static_cast<uint32_t>(frame_info.image_size.x);
    height = static_cast<uint32_t>(frame_info.image_size.y);

    std::vector<uint32_t> pixels(static_cast<size_t>(width) * height, 0);
    if (!frame_info.base || !mem_state)
        return pixels;

    // Reading the frame back out of the OpenGL texture would round-trip it
    // through the driver for nothing; the authoritative pixels are the guest
    // ones the rasterizer just wrote.
    const uint32_t *source = frame_info.base.cast<const uint32_t>().get(*mem_state);
    if (!source)
        return pixels;

    for (uint32_t y = 0; y < height; y++)
        std::memcpy(&pixels[static_cast<size_t>(y) * width], source + static_cast<size_t>(y) * frame_info.pitch,
            static_cast<size_t>(width) * sizeof(uint32_t));

    return pixels;
}

int SWState::get_supported_filters() {
    // Presentation goes through the GL screen renderer, so the GL backend's filters apply as-is.
    return static_cast<int>(Filter::NEAREST) | static_cast<int>(Filter::BILINEAR)
        | static_cast<int>(Filter::BICUBIC) | static_cast<int>(Filter::FXAA);
}

void SWState::set_screen_filter(const std::string_view &filter) {
    if (filter == "Nearest")
        screen_renderer.filter = gl::ScreenRenderer::Filter::Nearest;
    else if (filter == "FXAA")
        screen_renderer.filter = gl::ScreenRenderer::Filter::FXAA;
    else if (filter == "Bicubic")
        screen_renderer.filter = gl::ScreenRenderer::Filter::Bicubic;
    else
        screen_renderer.filter = gl::ScreenRenderer::Filter::Bilinear;
}

int SWState::get_max_anisotropic_filtering() {
    // Anisotropic filtering is not implemented by the CPU sampler.
    return 1;
}

void SWState::set_anisotropic_filtering(int anisotropic_filtering) {
    texture_cache.anisotropic_filtering = 1;
}

int SWState::get_max_2d_texture_width() {
    return 4096;
}

std::string_view SWState::get_gpu_name() {
    return gpu_name;
}

void SWState::precompile_shader(const ShadersHash &hash) {
    // Shaders are recompiled and parsed the first time a draw needs them, and
    // parsing a module costs a fraction of what a driver compile does, so there
    // is nothing worth doing ahead of time here.
}

void SWState::preclose_action() {}

void SWState::cleanup() {
    set_current();

    context = nullptr;

    rasterizer.reset();
    shader_cache.clear();
    texture_cache.cleanup();
    surface_cache.cleanup();

    screen_renderer.destroy();
    overlay_renderer.destroy();
}

} // namespace renderer::software
