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

#include <renderer/software/surface_cache.h>

#include <mem/state.h>
#include <util/log.h>

#include <algorithm>

namespace renderer::software {

void SWDepthStencilTarget::resize(uint32_t new_width, uint32_t new_height) {
    if (new_width == width && new_height == height)
        return;

    width = new_width;
    height = new_height;

    const size_t pixel_count = static_cast<size_t>(width) * height;
    if (has_depth)
        depth.assign(pixel_count, 1.0f);
    if (has_stencil)
        stencil.assign(pixel_count, 0);
}

SWColorTarget SWSurfaceCache::bind_color(const SceGxmColorSurface &surface, MemState &mem) {
    SWColorTarget target;

    target.disabled = surface.disabled;
    if (target.disabled)
        return target;

    target.data = surface.data.cast<uint8_t>().get(mem);
    target.width = surface.width;
    target.height = surface.height;
    target.stride_in_pixels = surface.strideInPixels;
    target.format = get_color_format_info(surface.colorFormat);
    target.surface_type = surface.surfaceType;
    target.gamma_corrected = surface.gamma != 0;

    if (target.stride_in_pixels == 0)
        target.stride_in_pixels = target.width;

    return target;
}

SWDepthStencilTarget *SWSurfaceCache::bind_depth_stencil(const SceGxmDepthStencilSurface &surface,
    uint32_t width, uint32_t height) {
    if (surface.disabled())
        return nullptr;

    const uint64_t key = (static_cast<uint64_t>(surface.depth_data.address()) << 32)
        | surface.stencil_data.address();

    auto &slot = m_depth_targets[key];
    if (!slot) {
        slot = std::make_unique<SWDepthStencilTarget>();

        switch (surface.get_format()) {
        case SCE_GXM_DEPTH_STENCIL_FORMAT_S8:
            slot->has_stencil = true;
            break;
        case SCE_GXM_DEPTH_STENCIL_FORMAT_DF32:
        case SCE_GXM_DEPTH_STENCIL_FORMAT_DF32M:
        case SCE_GXM_DEPTH_STENCIL_FORMAT_D16:
            slot->has_depth = true;
            break;
        default:
            // DF32_S8, DF32M_S8 and S8D24 all carry both.
            slot->has_depth = true;
            slot->has_stencil = true;
            break;
        }
    }

    slot->resize(width, height);
    return slot.get();
}

void SWSurfaceCache::cleanup() {
    m_depth_targets.clear();
}

} // namespace renderer::software
