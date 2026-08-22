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

#include <renderer/gxm_types.h>
#include <renderer/software/common.h>

#include <memory>
#include <unordered_map>
#include <vector>

struct MemState;

namespace renderer::software {

// A color surface as the rasterizer sees it. Unlike the GPU backends, the
// software renderer has no reason to keep a host copy of the color surface: it
// writes the pixels straight into the guest memory the game handed to GXM, in
// the very format the game asked for. That makes every surface read the game
// performs correct by construction, and makes sync_surface_data a no-op.
struct SWColorTarget {
    uint8_t *data = nullptr;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t stride_in_pixels = 0;
    ColorFormatInfo format{};
    SceGxmColorSurfaceType surface_type = SCE_GXM_COLOR_SURFACE_LINEAR;
    bool gamma_corrected = false;
    bool disabled = true;

    bool valid() const {
        return data != nullptr && !disabled && width > 0 && height > 0;
    }

    uint8_t *pixel(uint32_t x, uint32_t y) const {
        return data + color_pixel_offset(surface_type, x, y, stride_in_pixels, height, format.bytes_per_pixel);
    }
};

// Depth and stencil live in a host-side buffer. Their guest layout is tiled and
// hardware specific, and games practically never read it back with the CPU, so
// mirroring it byte for byte would cost far more than it is worth.
struct SWDepthStencilTarget {
    std::vector<float> depth;
    std::vector<uint8_t> stencil;
    uint32_t width = 0;
    uint32_t height = 0;
    bool has_depth = false;
    bool has_stencil = false;

    void resize(uint32_t new_width, uint32_t new_height);
};

class SWSurfaceCache {
public:
    // Resolves a GXM color surface into a target pointing at guest memory.
    SWColorTarget bind_color(const SceGxmColorSurface &surface, MemState &mem);

    // Returns the host depth/stencil buffer for this surface, allocating it the
    // first time the surface is seen and growing it when the scene does.
    SWDepthStencilTarget *bind_depth_stencil(const SceGxmDepthStencilSurface &surface,
        uint32_t width, uint32_t height);

    void cleanup();

private:
    std::unordered_map<uint64_t, std::unique_ptr<SWDepthStencilTarget>> m_depth_targets;
};

} // namespace renderer::software
