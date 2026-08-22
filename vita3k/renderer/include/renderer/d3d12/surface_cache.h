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

#include <renderer/d3d12/common.h>
#include <renderer/d3d12/gxm_to_d3d12.h>
#include <renderer/d3d12/resource.h>
#include <renderer/gxm_types.h>

#include <map>
#include <optional>

struct MemState;

namespace renderer::d3d12 {

struct DXState;
struct DXRenderTarget;

// Where the Vita image sits inside the surface backing it.
struct Viewport {
    uint32_t offset_x = 0;
    uint32_t offset_y = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t texture_width = 0;
    uint32_t texture_height = 0;
};

struct ColorSurfaceCacheInfo {
    Resource image;
    DescriptorHandle rtv;
    // Long-lived SRV, copied into the frame ring when the surface is sampled.
    DescriptorHandle srv;
    // Snapshot read as last_frag_data, since D3D12 cannot sample a bound render target.
    Resource fetch_copy;
    DescriptorHandle fetch_srv;

    Ptr<void> data;
    uint32_t width = 0;
    uint32_t height = 0;
    // Dimensions actually allocated, after res_multiplier upscaling.
    uint32_t scaled_width = 0;
    uint32_t scaled_height = 0;
    uint32_t stride_bytes = 0;
    uint32_t total_bytes = 0;

    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    SceGxmColorBaseFormat base_format = SCE_GXM_COLOR_BASE_FORMAT_U8U8U8U8;

    // Scene counter at which the surface was last written, used to evict.
    uint64_t last_used_scene = 0;
};

struct DepthStencilSurfaceCacheInfo {
    Resource image;
    DescriptorHandle dsv;

    SceGxmDepthStencilSurface surface{};
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t scaled_width = 0;
    uint32_t scaled_height = 0;

    DepthStencilFormats formats;
    uint64_t last_used_scene = 0;
};

// What the draw path needs to bind a surface as a render target.
struct SurfaceRetrieveResult {
    Resource *image = nullptr;
    D3D12_CPU_DESCRIPTOR_HANDLE view{};
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    bool created = false;
    ColorSurfaceCacheInfo *color = nullptr;

    bool valid() const {
        return image != nullptr;
    }
};

// What the texture path needs to sample a surface.
struct TextureLookupResult {
    Resource *image = nullptr;
    DescriptorHandle srv;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
};

// Keeps GPU surfaces alive across scenes, keyed on the guest address they are
// backed by, so a render target written in one scene can be sampled or
// presented in the next.
//
// Compared to the vulkan surface cache this deliberately leaves out the
// format-casting and aliasing paths, the CPU readback used by surface sync, and
// depth-as-texture reads. Those are tracked separately; a lookup that would need
// one of them simply misses, which costs accuracy rather than correctness of the
// surfaces that do hit.
class DXSurfaceCache {
public:
    DXState &state;

    static constexpr uint32_t max_surfaces_allowed = 20;

    explicit DXSurfaceCache(DXState &state);

    void cleanup();

    SurfaceRetrieveResult retrieve_color_surface_for_framebuffer(MemState &mem, SceGxmColorSurface *color);
    SurfaceRetrieveResult retrieve_depth_stencil_for_framebuffer(SceGxmDepthStencilSurface *depth_stencil,
        uint32_t width, uint32_t height);

    // Copy `region` of the surface into its fetch snapshot and return the SRV that reads it.
    DescriptorHandle snapshot_for_fetch(ColorSurfaceCacheInfo &surface, ID3D12GraphicsCommandList *cmd_list,
        BarrierBatcher &barriers, const D3D12_RECT &region);

    // Look for a colour surface backing the memory this texture reads from.
    std::optional<TextureLookupResult> retrieve_color_surface_as_texture(const SceGxmTexture &texture,
        SceGxmColorBaseFormat base_format);

    // The surface holding the frame to present, or nullptr when the frame was
    // never rendered through a tracked render target.
    ColorSurfaceCacheInfo *sourcing_color_surface_for_presentation(Ptr<const void> address, uint32_t pitch,
        Viewport &viewport);

    void set_render_target(DXRenderTarget *new_target) {
        target = new_target;
    }

    void new_scene() {
        scene_timestamp++;
    }

private:
    void destroy_surface(ColorSurfaceCacheInfo &info);
    void destroy_surface(DepthStencilSurfaceCacheInfo &info);
    // Drop the least recently used surfaces once the cache is over its limit.
    void evict_surfaces();

    // Keyed on the guest address the surface is backed by.
    std::map<Address, ColorSurfaceCacheInfo> color_surfaces;
    std::map<Address, DepthStencilSurfaceCacheInfo> depth_stencil_surfaces;

    DXRenderTarget *target = nullptr;
    uint64_t scene_timestamp = 1;
};

} // namespace renderer::d3d12
