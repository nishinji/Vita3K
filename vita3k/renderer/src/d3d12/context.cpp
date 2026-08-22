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

#include <features/state.h>
#include <gxm/functions.h>
#include <renderer/functions.h>
#include <util/log.h>

namespace renderer::d3d12 {

// Per-scene streaming budgets. A scene that overruns one of these is reported
// rather than silently dropping geometry.
constexpr uint64_t VERTEX_STREAM_BUFFER_SIZE = 64 * 1024 * 1024;
constexpr uint64_t INDEX_STREAM_BUFFER_SIZE = 64 * 1024 * 1024;
constexpr uint64_t UNIFORM_STREAM_BUFFER_SIZE = 64 * 1024 * 1024;
constexpr uint64_t INFO_UNIFORM_BUFFER_SIZE = 16 * 1024 * 1024;

DXContext::DXContext(DXState &state, MemState &mem)
    : state(state)
    , mem(mem) {
    memset(&prev_vert_ublock, 0, sizeof(shader::RenderVertUniformBlock));
    memset(&prev_frag_ublock, 0, sizeof(shader::RenderFragUniformBlock));
}

DXContext::~DXContext() = default;

bool DXContext::create_resources() {
    // A vertex buffer view needs no particular alignment beyond the element
    // size, but 256 keeps every suballocation usable as a constant buffer too.
    if (!vertex_stream_buffer.create(state.device.Get(), VERTEX_STREAM_BUFFER_SIZE, 256, L"Vita3K vertex stream"))
        return false;

    // 16- and 32-bit indices only.
    if (!index_stream_buffer.create(state.device.Get(), INDEX_STREAM_BUFFER_SIZE, 256, L"Vita3K index stream"))
        return false;

    // Bound as root SRVs, which the runtime requires to be 4-byte aligned; 256
    // is used throughout so one alignment rule covers every ring.
    if (!vertex_uniform_buffer.create(state.device.Get(), UNIFORM_STREAM_BUFFER_SIZE, 256, L"Vita3K vertex uniforms"))
        return false;
    if (!fragment_uniform_buffer.create(state.device.Get(), UNIFORM_STREAM_BUFFER_SIZE, 256, L"Vita3K fragment uniforms"))
        return false;

    // Constant buffer views must start on a 256-byte boundary.
    if (!vertex_info_buffer.create(state.device.Get(), INFO_UNIFORM_BUFFER_SIZE, 256, L"Vita3K vertex render info"))
        return false;
    if (!fragment_info_buffer.create(state.device.Get(), INFO_UNIFORM_BUFFER_SIZE, 256, L"Vita3K fragment render info"))
        return false;

    // Command slots. The lists are created closed so the first Reset in
    // start_recording is symmetric with every later one.
    for (CommandSlot &slot : command_slots) {
        if (!dx_check(state.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                          IID_PPV_ARGS(&slot.render_allocator)),
                "creating a scene render command allocator"))
            return false;

        if (!dx_check(state.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                          IID_PPV_ARGS(&slot.prerender_allocator)),
                "creating a scene prerender command allocator"))
            return false;

        if (!dx_check(state.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                          slot.render_allocator.Get(), nullptr, IID_PPV_ARGS(&slot.render_cmd_list)),
                "creating a scene render command list"))
            return false;

        if (!dx_check(slot.render_cmd_list->Close(), "closing a scene render command list"))
            return false;

        if (!dx_check(state.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                          slot.prerender_allocator.Get(), nullptr, IID_PPV_ARGS(&slot.prerender_cmd_list)),
                "creating a scene prerender command list"))
            return false;

        if (!dx_check(slot.prerender_cmd_list->Close(), "closing a scene prerender command list"))
            return false;
    }

    // Default viewport and scissor until the game sets its own.
    viewport = D3D12_VIEWPORT{
        .TopLeftX = 0.0f,
        .TopLeftY = 0.0f,
        .Width = DEFAULT_RES_WIDTH * state.res_multiplier,
        .Height = DEFAULT_RES_HEIGHT * state.res_multiplier,
        .MinDepth = 0.0f,
        .MaxDepth = 1.0f,
    };

    scissor = D3D12_RECT{
        .left = 0,
        .top = 0,
        .right = static_cast<LONG>(DEFAULT_RES_WIDTH * state.res_multiplier),
        .bottom = static_cast<LONG>(DEFAULT_RES_HEIGHT * state.res_multiplier),
    };

    return true;
}

void DXContext::start_recording() {
    if (is_recording)
        return;

    // Pick the next slot and wait for whatever it last recorded to retire. With
    // eight slots this only ever blocks when a frame submits an unusual number
    // of scenes.
    current_slot_idx = (current_slot_idx + 1) % COMMAND_SLOT_COUNT;
    CommandSlot &slot = command_slots[current_slot_idx];
    state.wait_for_fence(slot.fence_value);

    if (!dx_check(slot.render_allocator->Reset(), "resetting a scene render command allocator"))
        return;
    if (!dx_check(slot.prerender_allocator->Reset(), "resetting a scene prerender command allocator"))
        return;

    if (!dx_check(slot.render_cmd_list->Reset(slot.render_allocator.Get(), nullptr),
            "resetting the scene render command list"))
        return;
    if (!dx_check(slot.prerender_cmd_list->Reset(slot.prerender_allocator.Get(), nullptr),
            "resetting the scene prerender command list"))
        return;

    render_cmd_list = slot.render_cmd_list.Get();
    prerender_cmd_list = slot.prerender_cmd_list.Get();

    // Now that the GPU has passed this slot, the streaming memory it consumed
    // can be handed out again.
    const uint64_t completed = state.completed_fence_value();
    vertex_stream_buffer.reclaim(completed);
    index_stream_buffer.reclaim(completed);
    vertex_uniform_buffer.reclaim(completed);
    fragment_uniform_buffer.reclaim(completed);
    vertex_info_buffer.reclaim(completed);
    fragment_info_buffer.reclaim(completed);

    // Texture uploads recorded from the shared cache go on the prerender list so
    // they complete before any draw that samples them.
    state.texture_cache.cmd_list = prerender_cmd_list;

    is_recording = true;
    current_pipeline = nullptr;
    textures_dirty = true;
}

void DXContext::start_render_pass() {
    if (in_render_pass || !is_recording)
        return;

    // D3D12 has no render pass object: binding the targets and issuing the
    // clears is the whole equivalent.
    barriers.flush(render_cmd_list);

    render_cmd_list->OMSetRenderTargets(current_color_image ? 1 : 0,
        current_color_image ? &current_rtv : nullptr,
        FALSE,
        current_depth_image ? &current_dsv : nullptr);

    render_cmd_list->RSSetViewports(1, &viewport);
    render_cmd_list->RSSetScissorRects(1, &scissor);

    in_render_pass = true;
}

void DXContext::stop_render_pass() {
    if (!in_render_pass)
        return;

    if (is_in_query && current_visibility_buffer) {
        render_cmd_list->EndQuery(current_visibility_buffer->query_heap.Get(),
            D3D12_QUERY_TYPE_OCCLUSION, current_query_idx);
        is_in_query = false;
    }

    in_render_pass = false;
}

void DXContext::stop_recording(const SceGxmNotification &notification1, const SceGxmNotification &notification2, bool submit) {
    if (!is_recording)
        return;

    stop_render_pass();

    // Queueing a barrier already updated the tracked resource state, so any that
    // are still pending have to be emitted rather than dropped.
    barriers.flush(render_cmd_list);

    state.texture_cache.cmd_list = nullptr;

    if (!dx_check(prerender_cmd_list->Close(), "closing the prerender command list"))
        return;
    if (!dx_check(render_cmd_list->Close(), "closing the render command list"))
        return;

    // Prerender first: it holds the uploads the render list reads.
    ID3D12CommandList *lists[] = { prerender_cmd_list, render_cmd_list };
    last_submit_fence = state.submit(lists, 2);
    command_slots[current_slot_idx].fence_value = last_submit_fence;

    // Every ring that fed this scene is safe to reclaim once the fence passes.
    vertex_stream_buffer.retire(last_submit_fence);
    index_stream_buffer.retire(last_submit_fence);
    vertex_uniform_buffer.retire(last_submit_fence);
    fragment_uniform_buffer.retire(last_submit_fence);
    vertex_info_buffer.retire(last_submit_fence);
    fragment_info_buffer.retire(last_submit_fence);
    // Texture uploads were recorded onto the prerender list of this submission,
    // so their staging memory is reclaimable against the same fence. Without
    // this the staging ring never releases anything and fills up permanently.
    state.texture_cache.staging_buffer.retire(last_submit_fence);

    is_recording = false;
    current_pipeline = nullptr;

    if (submit) {
        // Undo set_context's MSAA doubling, as vulkan does; otherwise every scene doubles the target again.
        if (render_target && render_target->multisample_mode && !record.color_surface.downscale) {
            render_target->width /= 2;
            render_target->height /= 2;
        }

        // The guest may only observe these once the GPU has finished the scene,
        // but waiting for that here would block the render thread on every
        // scene. Hand it to the notification worker instead.
        state.queue_notifications(last_submit_fence, notification1, notification2);
    }
}

void set_context(DXContext &context, MemState &mem, DXRenderTarget *rt, const FeatureState &features) {
    context.render_target = rt;
    context.scene_timestamp++;
    context.state.texture_cache.current_scene_timestamp = context.scene_timestamp;
    context.state.surface_cache.new_scene();

    SceGxmColorSurface *color_surface = &context.record.color_surface;

    // These feed the pipeline key, so they must be set before any draw.
    context.record.color_base_format = gxm::get_base_format(color_surface->colorFormat);
    context.record.is_gamma_corrected = static_cast<bool>(color_surface->gamma);

    if (color_surface->data.address() == 0) {
        // Nothing backs the colour surface; the scene renders to a throwaway
        // target, so reset the state the pipeline key reads.
        color_surface = nullptr;
        context.record.color_surface.downscale = static_cast<bool>(rt->multisample_mode);
        context.record.is_gamma_corrected = false;
        context.record.is_maskupdate = false;
        context.record.color_base_format = SCE_GXM_COLOR_BASE_FORMAT_U8U8U8U8;
    }

    if (rt->multisample_mode && !context.record.color_surface.downscale) {
        // MSAA without downscaling is emulated by doubling the target size, the
        // same approximation the vulkan backend makes.
        rt->width *= 2;
        rt->height *= 2;
    }

    context.state.surface_cache.set_render_target(rt);

    context.start_recording();
    if (!context.is_recording)
        return;

    // Colour target.
    context.current_color_image = nullptr;
    context.current_color_surface = nullptr;
    context.current_color_format = DXGI_FORMAT_UNKNOWN;
    context.fetch_srv = {};
    bool color_created = false;
    if (color_surface) {
        const SurfaceRetrieveResult result = context.state.surface_cache.retrieve_color_surface_for_framebuffer(mem, color_surface);
        if (result.valid()) {
            context.current_color_image = result.image;
            context.current_color_surface = result.color;
            context.current_rtv = result.view;
            context.current_color_format = result.format;
            color_created = result.created;
            context.barriers.transition(*result.image, D3D12_RESOURCE_STATE_RENDER_TARGET);
        }
    }

    // Depth-stencil target. Only a surface with memory behind it is cached; the
    // render target's own buffer stands in for the rest, as in the vulkan backend.
    SceGxmDepthStencilSurface *ds_surface = &context.record.depth_stencil_surface;
    if (!ds_surface->depth_data && !ds_surface->stencil_data)
        ds_surface = nullptr;

    context.current_depth_image = nullptr;
    context.current_depth_format = DXGI_FORMAT_UNKNOWN;
    if (ds_surface) {
        const SurfaceRetrieveResult result = context.state.surface_cache.retrieve_depth_stencil_for_framebuffer(
            ds_surface, rt->width, rt->height);
        if (result.valid()) {
            context.current_depth_image = result.image;
            context.current_dsv = result.view;
            context.current_depth_format = result.format;
            context.barriers.transition(*result.image, D3D12_RESOURCE_STATE_DEPTH_WRITE);
        }
    } else if (rt->depth_stencil) {
        context.current_depth_image = &rt->depth_stencil;
        context.current_dsv = rt->dsv.cpu;
        context.current_depth_format = DXRenderTarget::depth_stencil_format;
        context.barriers.transition(rt->depth_stencil, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    }

    context.current_sample_count = 1;

    context.start_render_pass();

    // D3D12 leaves a new render target undefined; the vulkan backend starts one as transparent black.
    if (color_created) {
        static constexpr float transparent_black[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        context.render_cmd_list->ClearRenderTargetView(context.current_rtv, transparent_black, 0, nullptr);
    }

    // The colour target is deliberately NOT cleared. GXM has no clear operation
    // of its own: a game clears by drawing a fullscreen quad with a shader that
    // writes a uniform colour, and several scenes accumulate into the same
    // surface, so clearing here would wipe what earlier scenes drew. This
    // matches the vulkan backend, whose colour attachment is always eLoad (or
    // eDontCare when there is no surface to preserve), never eClear.
    //
    // Depth-stencil is different: GXM does express a load/clear choice for it
    // through force_load, and the value to clear to through the surface.
    // The render target's own buffer has nothing stored to load.
    const bool load_depth_stencil = ds_surface && context.record.depth_stencil_surface.force_load;
    if (context.current_depth_image && !load_depth_stencil) {
        context.render_cmd_list->ClearDepthStencilView(context.current_dsv,
            D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL,
            context.record.depth_stencil_surface.background_depth,
            static_cast<UINT8>(context.record.depth_stencil_surface.stencil),
            0, nullptr);
    }

    // Textures bound by the previous scene must not leak into this one.
    for (uint32_t i = 0; i < SCE_GXM_MAX_TEXTURE_UNITS; i++) {
        context.vertex_textures[i] = {};
        context.vertex_samplers[i] = {};
        context.fragment_textures[i] = {};
        context.fragment_samplers[i] = {};
    }

    context.textures_dirty = true;
    context.is_first_scene_draw = true;
    context.refresh_pipeline = true;
}

void new_frame(DXContext &context) {
    // The frame slot is advanced by the presentation path, which is the point
    // the swapchain is actually waited on; doing it here as well would recycle
    // pools a frame early.
    context.frame_timestamp++;
}

} // namespace renderer::d3d12
