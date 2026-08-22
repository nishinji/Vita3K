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

#include <renderer/d3d12/surface_cache.h>

#include <renderer/d3d12/state.h>
#include <renderer/d3d12/types.h>

#include <gxm/functions.h>
#include <mem/state.h>
#include <util/align.h>
#include <util/log.h>

#include <algorithm>

namespace renderer::d3d12 {

DXSurfaceCache::DXSurfaceCache(DXState &state)
    : state(state) {
}

void DXSurfaceCache::destroy_surface(ColorSurfaceCacheInfo &info) {
    if (info.rtv.valid()) {
        state.rtv_heap.free(info.rtv);
        info.rtv = {};
    }
    if (info.srv.valid()) {
        state.srv_staging_heap.free(info.srv);
        info.srv = {};
    }
    if (info.fetch_srv.valid()) {
        state.srv_staging_heap.free(info.fetch_srv);
        info.fetch_srv = {};
    }

    // The surface may still be referenced by an in-flight frame.
    if (info.image)
        state.frame().destroy_queue.push_back(info.image.resource);
    if (info.fetch_copy)
        state.frame().destroy_queue.push_back(info.fetch_copy.resource);

    info.image.reset();
    info.fetch_copy.reset();
}

void DXSurfaceCache::destroy_surface(DepthStencilSurfaceCacheInfo &info) {
    if (info.dsv.valid()) {
        state.dsv_heap.free(info.dsv);
        info.dsv = {};
    }

    if (info.image)
        state.frame().destroy_queue.push_back(info.image.resource);

    info.image.reset();
}

void DXSurfaceCache::cleanup() {
    for (auto &[address, info] : color_surfaces)
        destroy_surface(info);
    for (auto &[address, info] : depth_stencil_surfaces)
        destroy_surface(info);

    color_surfaces.clear();
    depth_stencil_surfaces.clear();
}

void DXSurfaceCache::evict_surfaces() {
    while (color_surfaces.size() > max_surfaces_allowed) {
        auto oldest = std::min_element(color_surfaces.begin(), color_surfaces.end(),
            [](const auto &lhs, const auto &rhs) { return lhs.second.last_used_scene < rhs.second.last_used_scene; });

        destroy_surface(oldest->second);
        color_surfaces.erase(oldest);
    }

    while (depth_stencil_surfaces.size() > max_surfaces_allowed) {
        auto oldest = std::min_element(depth_stencil_surfaces.begin(), depth_stencil_surfaces.end(),
            [](const auto &lhs, const auto &rhs) { return lhs.second.last_used_scene < rhs.second.last_used_scene; });

        destroy_surface(oldest->second);
        depth_stencil_surfaces.erase(oldest);
    }
}

SurfaceRetrieveResult DXSurfaceCache::retrieve_color_surface_for_framebuffer(MemState &mem, SceGxmColorSurface *color) {
    if (!color || !color->data)
        return {};

    const Address address = color->data.address();
    const SceGxmColorBaseFormat base_format = gxm::get_base_format(color->colorFormat);
    const DXGI_FORMAT format = color::translate_format(base_format);
    if (format == DXGI_FORMAT_UNKNOWN)
        return {};

    const uint32_t width = color->width;
    const uint32_t height = color->height;
    const uint32_t scaled_width = static_cast<uint32_t>(width * state.res_multiplier);
    const uint32_t scaled_height = static_cast<uint32_t>(height * state.res_multiplier);

    auto it = color_surfaces.find(address);
    if (it != color_surfaces.end()) {
        ColorSurfaceCacheInfo &info = it->second;
        // A surface is reusable only when everything baked into the resource
        // matches; anything else has to be recreated at the new description.
        if (info.format == format && info.scaled_width == scaled_width && info.scaled_height == scaled_height) {
            info.last_used_scene = scene_timestamp;
            return SurfaceRetrieveResult{ &info.image, info.rtv.cpu, info.format, false, &info };
        }

        destroy_surface(info);
        color_surfaces.erase(it);
    }

    ColorSurfaceCacheInfo info;
    info.data = color->data;
    info.width = width;
    info.height = height;
    info.scaled_width = scaled_width;
    info.scaled_height = scaled_height;
    info.stride_bytes = color->strideInPixels * ((gxm::bits_per_pixel(base_format) + 7) / 8);
    info.total_bytes = info.stride_bytes * height;
    info.format = format;
    info.base_format = base_format;
    info.last_used_scene = scene_timestamp;

    const D3D12_HEAP_PROPERTIES heap_props{
        .Type = D3D12_HEAP_TYPE_DEFAULT,
        .CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN,
        .MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN,
        .CreationNodeMask = 1,
        .VisibleNodeMask = 1,
    };

    const D3D12_RESOURCE_DESC desc{
        .Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D,
        .Alignment = 0,
        .Width = scaled_width,
        .Height = scaled_height,
        .DepthOrArraySize = 1,
        .MipLevels = 1,
        .Format = format,
        .SampleDesc = { .Count = 1, .Quality = 0 },
        .Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN,
        .Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
    };

    // A matching clear value avoids the driver warning about mismatched clears
    // and lets fast clear paths kick in. It is the value set_context clears a new surface to.
    const D3D12_CLEAR_VALUE clear_value{
        .Format = format,
        .Color = { 0.0f, 0.0f, 0.0f, 0.0f },
    };

    if (!dx_check(state.device->CreateCommittedResource(&heap_props, D3D12_HEAP_FLAG_NONE, &desc,
                      D3D12_RESOURCE_STATE_RENDER_TARGET, &clear_value, IID_PPV_ARGS(&info.image.resource)),
            "creating a colour surface"))
        return {};

    info.image.state = D3D12_RESOURCE_STATE_RENDER_TARGET;
    info.image.resource->SetName(L"Vita3K colour surface");

    info.rtv = state.rtv_heap.allocate();
    if (!info.rtv.valid()) {
        destroy_surface(info);
        return {};
    }
    state.device->CreateRenderTargetView(info.image.get(), nullptr, info.rtv.cpu);

    info.srv = state.srv_staging_heap.allocate();
    if (info.srv.valid()) {
        const D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc{
            .Format = format,
            .ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D,
            .Shader4ComponentMapping = color::translate_swizzle(color->colorFormat),
            .Texture2D = { .MostDetailedMip = 0, .MipLevels = 1, .PlaneSlice = 0, .ResourceMinLODClamp = 0.0f },
        };
        state.device->CreateShaderResourceView(info.image.get(), &srv_desc, info.srv.cpu);
    }

    auto [inserted, ok] = color_surfaces.insert_or_assign(address, std::move(info));
    evict_surfaces();

    // evict_surfaces may have dropped other entries but never the one just
    // inserted, since it carries the newest timestamp.
    ColorSurfaceCacheInfo &stored = color_surfaces.at(address);
    return SurfaceRetrieveResult{ &stored.image, stored.rtv.cpu, stored.format, true, &stored };
}

DescriptorHandle DXSurfaceCache::snapshot_for_fetch(ColorSurfaceCacheInfo &surface, ID3D12GraphicsCommandList *cmd_list,
    BarrierBatcher &barriers, const D3D12_RECT &region) {
    if (!surface.fetch_copy) {
        const D3D12_HEAP_PROPERTIES heap_props{
            .Type = D3D12_HEAP_TYPE_DEFAULT,
            .CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN,
            .MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN,
            .CreationNodeMask = 1,
            .VisibleNodeMask = 1,
        };

        const D3D12_RESOURCE_DESC desc{
            .Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D,
            .Alignment = 0,
            .Width = surface.scaled_width,
            .Height = surface.scaled_height,
            .DepthOrArraySize = 1,
            .MipLevels = 1,
            .Format = surface.format,
            .SampleDesc = { .Count = 1, .Quality = 0 },
            .Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN,
            .Flags = D3D12_RESOURCE_FLAG_NONE,
        };

        if (!dx_check(state.device->CreateCommittedResource(&heap_props, D3D12_HEAP_FLAG_NONE, &desc,
                          D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&surface.fetch_copy.resource)),
                "creating a framebuffer fetch snapshot"))
            return {};

        surface.fetch_copy.state = D3D12_RESOURCE_STATE_COPY_DEST;
        surface.fetch_copy.resource->SetName(L"Vita3K framebuffer fetch snapshot");

        surface.fetch_srv = state.srv_staging_heap.allocate();
        if (!surface.fetch_srv.valid())
            return {};

        // Unlike the sampling SRV, no swizzle: the shader reads the channels in the order it wrote them.
        const D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc{
            .Format = surface.format,
            .ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D,
            .Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING,
            .Texture2D = { .MostDetailedMip = 0, .MipLevels = 1, .PlaneSlice = 0, .ResourceMinLODClamp = 0.0f },
        };
        state.device->CreateShaderResourceView(surface.fetch_copy.get(), &srv_desc, surface.fetch_srv.cpu);
    }

    // Nothing outside the scissor can be rasterized, so nothing outside it is read either.
    const D3D12_BOX box{
        .left = static_cast<UINT>(std::clamp<LONG>(region.left, 0, static_cast<LONG>(surface.scaled_width))),
        .top = static_cast<UINT>(std::clamp<LONG>(region.top, 0, static_cast<LONG>(surface.scaled_height))),
        .front = 0,
        .right = static_cast<UINT>(std::clamp<LONG>(region.right, 0, static_cast<LONG>(surface.scaled_width))),
        .bottom = static_cast<UINT>(std::clamp<LONG>(region.bottom, 0, static_cast<LONG>(surface.scaled_height))),
        .back = 1,
    };

    if (box.right > box.left && box.bottom > box.top) {
        barriers.transition(surface.image, D3D12_RESOURCE_STATE_COPY_SOURCE);
        barriers.transition(surface.fetch_copy, D3D12_RESOURCE_STATE_COPY_DEST);
        barriers.flush(cmd_list);

        const D3D12_TEXTURE_COPY_LOCATION destination{
            .pResource = surface.fetch_copy.get(),
            .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,
            .SubresourceIndex = 0,
        };
        const D3D12_TEXTURE_COPY_LOCATION source{
            .pResource = surface.image.get(),
            .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,
            .SubresourceIndex = 0,
        };
        cmd_list->CopyTextureRegion(&destination, box.left, box.top, 0, &source, &box);
    }

    barriers.transition(surface.image, D3D12_RESOURCE_STATE_RENDER_TARGET);
    barriers.transition(surface.fetch_copy, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    barriers.flush(cmd_list);

    return surface.fetch_srv;
}

SurfaceRetrieveResult DXSurfaceCache::retrieve_depth_stencil_for_framebuffer(SceGxmDepthStencilSurface *depth_stencil,
    uint32_t width, uint32_t height) {
    if (!depth_stencil)
        return {};

    // Depth and stencil can live at different addresses; the depth one is what
    // identifies the pair, falling back to the stencil address for
    // stencil-only surfaces.
    const Address address = depth_stencil->depth_data
        ? depth_stencil->depth_data.address()
        : depth_stencil->stencil_data.address();
    if (address == 0)
        return {};

    const DepthStencilFormats formats = translate_depth_stencil_format(depth_stencil->get_format());
    const uint32_t scaled_width = static_cast<uint32_t>(width * state.res_multiplier);
    const uint32_t scaled_height = static_cast<uint32_t>(height * state.res_multiplier);

    auto it = depth_stencil_surfaces.find(address);
    if (it != depth_stencil_surfaces.end()) {
        DepthStencilSurfaceCacheInfo &info = it->second;
        if (info.formats.resource == formats.resource
            && info.scaled_width == scaled_width && info.scaled_height == scaled_height) {
            info.last_used_scene = scene_timestamp;
            return SurfaceRetrieveResult{ &info.image, info.dsv.cpu, info.formats.dsv };
        }

        destroy_surface(info);
        depth_stencil_surfaces.erase(it);
    }

    DepthStencilSurfaceCacheInfo info;
    info.surface = *depth_stencil;
    info.width = width;
    info.height = height;
    info.scaled_width = scaled_width;
    info.scaled_height = scaled_height;
    info.formats = formats;
    info.last_used_scene = scene_timestamp;

    const D3D12_HEAP_PROPERTIES heap_props{
        .Type = D3D12_HEAP_TYPE_DEFAULT,
        .CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN,
        .MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN,
        .CreationNodeMask = 1,
        .VisibleNodeMask = 1,
    };

    // Typeless, so the same resource can later be read as a texture.
    const D3D12_RESOURCE_DESC desc{
        .Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D,
        .Alignment = 0,
        .Width = scaled_width,
        .Height = scaled_height,
        .DepthOrArraySize = 1,
        .MipLevels = 1,
        .Format = formats.resource,
        .SampleDesc = { .Count = 1, .Quality = 0 },
        .Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN,
        .Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL,
    };

    const D3D12_CLEAR_VALUE clear_value{
        .Format = formats.dsv,
        .DepthStencil = { .Depth = 1.0f, .Stencil = 0 },
    };

    if (!dx_check(state.device->CreateCommittedResource(&heap_props, D3D12_HEAP_FLAG_NONE, &desc,
                      D3D12_RESOURCE_STATE_DEPTH_WRITE, &clear_value, IID_PPV_ARGS(&info.image.resource)),
            "creating a depth stencil surface"))
        return {};

    info.image.state = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    info.image.resource->SetName(L"Vita3K depth stencil surface");

    info.dsv = state.dsv_heap.allocate();
    if (!info.dsv.valid()) {
        destroy_surface(info);
        return {};
    }

    const D3D12_DEPTH_STENCIL_VIEW_DESC dsv_desc{
        .Format = formats.dsv,
        .ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D,
        .Flags = D3D12_DSV_FLAG_NONE,
        .Texture2D = { .MipSlice = 0 },
    };
    state.device->CreateDepthStencilView(info.image.get(), &dsv_desc, info.dsv.cpu);

    depth_stencil_surfaces.insert_or_assign(address, std::move(info));
    evict_surfaces();

    DepthStencilSurfaceCacheInfo &stored = depth_stencil_surfaces.at(address);
    return SurfaceRetrieveResult{ &stored.image, stored.dsv.cpu, stored.formats.dsv };
}

std::optional<TextureLookupResult> DXSurfaceCache::retrieve_color_surface_as_texture(const SceGxmTexture &texture,
    SceGxmColorBaseFormat base_format) {
    const Address address = texture.data_addr << 2;
    if (address == 0)
        return std::nullopt;

    // Only an exact base address hit is handled. The vulkan cache additionally
    // resolves reads that land partway into a surface; those miss here and fall
    // back to reading the texture out of guest memory.
    auto it = color_surfaces.find(address);
    if (it == color_surfaces.end())
        return std::nullopt;

    ColorSurfaceCacheInfo &info = it->second;
    if (!info.srv.valid())
        return std::nullopt;

    if (info.base_format != base_format)
        // Reinterpreting a surface in another format needs the casting path.
        return std::nullopt;

    info.last_used_scene = scene_timestamp;

    return TextureLookupResult{ &info.image, info.srv, info.format };
}

ColorSurfaceCacheInfo *DXSurfaceCache::sourcing_color_surface_for_presentation(Ptr<const void> address, uint32_t pitch,
    Viewport &viewport) {
    if (!address)
        return nullptr;

    // The frame to present is normally the exact surface last rendered to, but
    // games can also present a sub-rectangle of a bigger surface.
    auto it = color_surfaces.upper_bound(address.address());
    if (it == color_surfaces.begin())
        return nullptr;

    --it;
    ColorSurfaceCacheInfo &info = it->second;

    if (info.stride_bytes == 0 || !info.image)
        return nullptr;

    const uint32_t bytes_per_pixel = (gxm::bits_per_pixel(info.base_format) + 7) / 8;
    if (info.stride_bytes != pitch * bytes_per_pixel)
        return nullptr;

    const uint32_t offset_bytes = address.address() - it->first;
    if (offset_bytes >= info.total_bytes)
        return nullptr;

    // Turn the byte offset into a pixel offset inside the surface.
    const uint32_t offset_y = offset_bytes / info.stride_bytes;
    const uint32_t offset_x = (offset_bytes % info.stride_bytes) / bytes_per_pixel;

    if (offset_x + viewport.width > info.width || offset_y + viewport.height > info.height)
        return nullptr;

    const float multiplier = state.res_multiplier;
    viewport.offset_x = static_cast<uint32_t>(offset_x * multiplier);
    viewport.offset_y = static_cast<uint32_t>(offset_y * multiplier);
    viewport.width = static_cast<uint32_t>(viewport.width * multiplier);
    viewport.height = static_cast<uint32_t>(viewport.height * multiplier);
    viewport.texture_width = info.scaled_width;
    viewport.texture_height = info.scaled_height;

    info.last_used_scene = scene_timestamp;
    return &info;
}

} // namespace renderer::d3d12
