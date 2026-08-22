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

#include <renderer/d3d12/functions.h>
#include <renderer/d3d12/gxm_to_d3d12.h>
#include <renderer/d3d12/state.h>
#include <renderer/d3d12/types.h>

#include <gxm/functions.h>
#include <renderer/functions.h>
#include <util/log.h>

#include <algorithm>
#include <optional>

namespace renderer::d3d12 {

void sync_clipping(DXContext &context) {
    if (!context.render_target)
        return;

    const float res_multiplier = context.state.res_multiplier;

    const int scissor_x = context.record.region_clip_min.x;
    const int scissor_y = context.record.region_clip_min.y;
    const int scissor_w = std::max(context.record.region_clip_max.x - context.record.region_clip_min.x + 1, 0);
    const int scissor_h = std::max(context.record.region_clip_max.y - context.record.region_clip_min.y + 1, 0);

    const LONG target_width = static_cast<LONG>(context.render_target->width);
    const LONG target_height = static_cast<LONG>(context.render_target->height);

    switch (context.record.region_clip_mode) {
    case SCE_GXM_REGION_CLIP_NONE:
        context.scissor = D3D12_RECT{ 0, 0, target_width, target_height };
        break;

    case SCE_GXM_REGION_CLIP_ALL:
        context.scissor = D3D12_RECT{ 0, 0, 0, 0 };
        break;

    case SCE_GXM_REGION_CLIP_OUTSIDE:
        context.scissor = D3D12_RECT{
            .left = static_cast<LONG>(scissor_x * res_multiplier),
            .top = static_cast<LONG>(scissor_y * res_multiplier),
            .right = static_cast<LONG>((scissor_x + scissor_w) * res_multiplier),
            .bottom = static_cast<LONG>((scissor_y + scissor_h) * res_multiplier),
        };
        break;

    case SCE_GXM_REGION_CLIP_INSIDE:
        // TODO: Implement SCE_GXM_REGION_CLIP_INSIDE
        LOG_WARN_ONCE("STUB SCE_GXM_REGION_CLIP_INSIDE");
        context.scissor = D3D12_RECT{ 0, 0, target_width, target_height };
        break;
    }

    // D3D12 rejects a scissor with a negative origin or an inverted extent.
    context.scissor.left = std::max<LONG>(context.scissor.left, 0);
    context.scissor.top = std::max<LONG>(context.scissor.top, 0);
    context.scissor.right = std::max(context.scissor.right, context.scissor.left);
    context.scissor.bottom = std::max(context.scissor.bottom, context.scissor.top);

    if (!context.is_recording)
        return;

    context.render_cmd_list->RSSetScissorRects(1, &context.scissor);
}

void sync_stencil_func(DXContext &context, const bool is_back) {
    // D3D12 bakes the compare and write masks into the pipeline state, so only
    // the reference value can be set here. The masks reach the PSO through the
    // pipeline key instead; see PipelineCache::retrieve_pipeline.
    context.refresh_pipeline = true;

    if (!context.is_recording)
        return;

    if (context.record.two_sided == SCE_GXM_TWO_SIDED_DISABLED && is_back)
        return;

    // There is only one stencil reference in D3D12, shared by both faces.
    const GxmStencilStateValues &values = (is_back && context.record.two_sided == SCE_GXM_TWO_SIDED_ENABLED)
        ? context.record.back_stencil_state_values
        : context.record.front_stencil_state_values;

    context.render_cmd_list->OMSetStencilRef(values.ref);
}

void sync_depth_bias(DXContext &context) {
    // Depth bias is rasterizer state in D3D12, not dynamic state, so the only
    // thing to do is force a pipeline lookup with the new values.
    context.refresh_pipeline = true;
}

void sync_depth_data(DXContext &context) {
    if (context.record.depth_stencil_surface.force_load)
        return;

    if (!context.is_recording || !context.current_depth_image)
        return;

    context.render_cmd_list->ClearDepthStencilView(context.current_dsv, D3D12_CLEAR_FLAG_DEPTH,
        context.record.depth_stencil_surface.background_depth, 0, 0, nullptr);
}

void sync_stencil_data(DXContext &context, const MemState &mem) {
    if (context.record.depth_stencil_surface.force_load)
        return;

    if (!context.is_recording || !context.current_depth_image)
        return;

    context.render_cmd_list->ClearDepthStencilView(context.current_dsv, D3D12_CLEAR_FLAG_STENCIL,
        0.0f, static_cast<UINT8>(context.record.depth_stencil_surface.stencil), 0, nullptr);
}

void sync_point_line_width(DXContext &context, const bool is_front) {
    // D3D12 has no line width control at all; lines are always one pixel wide.
    if (is_front && context.record.line_width != 1)
        LOG_WARN_ONCE("D3D12: line width is not supported, lines will be 1 pixel wide");
}

void sync_viewport_flat(DXContext &context) {
    if (!context.render_target)
        return;

    context.viewport = D3D12_VIEWPORT{
        .TopLeftX = 0.0f,
        .TopLeftY = 0.0f,
        .Width = static_cast<float>(context.render_target->width),
        .Height = static_cast<float>(context.render_target->height),
        .MinDepth = 0.0f,
        .MaxDepth = 1.0f,
    };

    if (!context.is_recording)
        return;

    context.render_cmd_list->RSSetViewports(1, &context.viewport);
}

void sync_viewport_real(DXContext &context, const float xOffset, const float yOffset, const float zOffset,
    const float xScale, const float yScale, const float zScale) {
    if (xScale < 0)
        LOG_ERROR("Game is using a viewport with negative width!");

    const float w = std::abs(2 * xScale);
    const float h = 2 * yScale;
    const float x = xOffset - std::abs(xScale);
    const float y = yOffset - yScale;

    const float res_multiplier = context.state.res_multiplier;

    // Same derivation as the vulkan backend: gxm computes
    //   x_f = xOffset + xScale * (x / w)
    // which matches a viewport of origin (xOffset - |xScale|) and width 2|xScale|.
    // The depth range is applied in the shader, so it stays 0..1 here.
    context.viewport = D3D12_VIEWPORT{
        .TopLeftX = x * res_multiplier,
        .TopLeftY = y * res_multiplier,
        .Width = w * res_multiplier,
        .Height = h * res_multiplier,
        .MinDepth = 0.0f,
        .MaxDepth = 1.0f,
    };

    if (!context.is_recording)
        return;

    context.render_cmd_list->RSSetViewports(1, &context.viewport);
}

void sync_visibility_buffer(DXContext &context, Ptr<uint32_t> buffer, uint32_t stride) {
    if (!buffer) {
        context.current_visibility_buffer = nullptr;
        return;
    }

    auto it = context.visibility_buffers.find(buffer.address());
    if (it == context.visibility_buffers.end()) {
        const uint32_t query_count = stride / sizeof(uint32_t);
        if (query_count == 0)
            return;

        VisibilityBuffer visibility_buffer;
        visibility_buffer.address = buffer.address();
        visibility_buffer.size = query_count;

        const D3D12_QUERY_HEAP_DESC heap_desc{
            .Type = D3D12_QUERY_HEAP_TYPE_OCCLUSION,
            .Count = query_count,
            .NodeMask = 0,
        };

        if (!dx_check(context.state.device->CreateQueryHeap(&heap_desc, IID_PPV_ARGS(&visibility_buffer.query_heap)),
                "creating an occlusion query heap"))
            return;

        // Occlusion results resolve into a buffer as 64-bit counts.
        const D3D12_HEAP_PROPERTIES heap_props{
            .Type = D3D12_HEAP_TYPE_READBACK,
            .CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN,
            .MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN,
            .CreationNodeMask = 1,
            .VisibleNodeMask = 1,
        };

        const D3D12_RESOURCE_DESC buffer_desc{
            .Dimension = D3D12_RESOURCE_DIMENSION_BUFFER,
            .Alignment = 0,
            .Width = static_cast<UINT64>(query_count) * sizeof(uint64_t),
            .Height = 1,
            .DepthOrArraySize = 1,
            .MipLevels = 1,
            .Format = DXGI_FORMAT_UNKNOWN,
            .SampleDesc = { .Count = 1, .Quality = 0 },
            .Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
            .Flags = D3D12_RESOURCE_FLAG_NONE,
        };

        if (!dx_check(context.state.device->CreateCommittedResource(&heap_props, D3D12_HEAP_FLAG_NONE, &buffer_desc,
                          D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&visibility_buffer.result_buffer)),
                "creating an occlusion query result buffer"))
            return;

        visibility_buffer.queries_used.resize(query_count + 1, false);

        it = context.visibility_buffers.emplace(buffer.address(), std::move(visibility_buffer)).first;
    }

    context.current_visibility_buffer = &it->second;
}

void sync_visibility_index(DXContext &context, bool enable, uint32_t index, bool is_increment) {
    if (context.current_visibility_buffer == nullptr) {
        context.current_query_idx = enable ? static_cast<int>(index) : -1;
        context.is_query_op_increment = is_increment;
        return;
    }

    if (index >= context.current_visibility_buffer->size) {
        LOG_WARN_ONCE("Using visibility index {} which is too big for the buffer", index);
        index = 0;
    }

    if (!enable) {
        if (context.is_in_query && context.is_recording) {
            context.render_cmd_list->EndQuery(context.current_visibility_buffer->query_heap.Get(),
                D3D12_QUERY_TYPE_OCCLUSION, context.current_query_idx);
            context.is_in_query = false;
        }

        context.current_query_idx = -1;
        return;
    }

    // Leave an in-flight query alone when the index has not moved.
    if (context.is_in_query && context.current_query_idx != static_cast<int>(index) && context.is_recording) {
        context.render_cmd_list->EndQuery(context.current_visibility_buffer->query_heap.Get(),
            D3D12_QUERY_TYPE_OCCLUSION, context.current_query_idx);
        context.is_in_query = false;
    }

    context.current_query_idx = static_cast<int>(index);
    context.is_query_op_increment = is_increment;
}

void sync_texture(DXContext &context, MemState &mem, std::size_t index, SceGxmTexture texture, const Config &config) {
    const SceGxmTextureFormat format = gxm::get_format(texture);
    const SceGxmTextureBaseFormat base_format = gxm::get_base_format(format);

    if (gxm::is_paletted_format(base_format) && texture.palette_addr == 0) {
        LOG_WARN_ONCE("Ignoring null palette texture");
        return;
    }

    const bool is_vertex = index >= SCE_GXM_MAX_TEXTURE_UNITS;
    const size_t slot = is_vertex ? index - SCE_GXM_MAX_TEXTURE_UNITS : index;
    if (slot >= SCE_GXM_MAX_TEXTURE_UNITS)
        return;

    // The recompiler needs the real format of every sampled texture, so this has
    // to be recorded before the shader for this draw is compiled.
    if (is_vertex)
        context.shader_hints.vertex_textures[slot] = format;
    else
        context.shader_hints.fragment_textures[slot] = format;

    DescriptorHandle *textures = is_vertex ? context.vertex_textures : context.fragment_textures;
    DescriptorHandle *samplers = is_vertex ? context.vertex_samplers : context.fragment_samplers;

    // A colour surface being sampled is served straight from the surface cache
    // rather than re-uploaded from guest memory.
    std::optional<TextureLookupResult> lookup;
    SceGxmColorBaseFormat color_format;
    if (::renderer::texture::convert_base_texture_format_to_base_color_format(base_format, color_format))
        lookup = context.state.surface_cache.retrieve_color_surface_as_texture(texture, color_format);

    // A surface cannot be sampled while it is bound as a render target: D3D12 has
    // no resource state that is both, so transitioning it would leave the draw
    // reading and writing the same resource and the runtime rejects it. The
    // texture is read out of guest memory instead, which is stale but valid.
    if (lookup.has_value()
        && (lookup->image == context.current_color_image || lookup->image == context.current_depth_image)) {
        LOG_WARN_ONCE("D3D12: a shader samples the surface it is rendering to; "
                      "the read comes from guest memory and will be stale");
        lookup.reset();
    }

    if (lookup.has_value()) {
        context.barriers.transition(*lookup->image,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        if (context.is_recording)
            context.barriers.flush(context.render_cmd_list);

        // Surfaces read back as textures must not be filtered across their edges.
        context.state.texture_cache.cache_and_bind_sampler(texture, true);
        const DescriptorHandle sampler = context.state.texture_cache.get_retrieved_sampler();

        if (textures[slot].cpu.ptr != lookup->srv.cpu.ptr || samplers[slot].cpu.ptr != sampler.cpu.ptr)
            context.textures_dirty = true;

        textures[slot] = lookup->srv;
        samplers[slot] = sampler;
        return;
    }

    context.state.texture_cache.cache_and_bind_texture(texture, mem);

    if (context.state.texture_cache.current_texture) {
        const DescriptorHandle srv = context.state.texture_cache.current_texture->srv;
        const DescriptorHandle sampler = context.state.texture_cache.get_retrieved_sampler();

        if (textures[slot].cpu.ptr != srv.cpu.ptr || samplers[slot].cpu.ptr != sampler.cpu.ptr)
            context.textures_dirty = true;

        textures[slot] = srv;
        samplers[slot] = sampler;
    }
}

void refresh_pipeline(DXContext &context) {
    context.refresh_pipeline = true;
}

} // namespace renderer::d3d12
