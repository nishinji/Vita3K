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
#include <renderer/d3d12/pipeline_cache.h>
#include <renderer/d3d12/state.h>
#include <renderer/d3d12/types.h>

#include <config/state.h>
#include <gxm/functions.h>
#include <renderer/functions.h>
#include <util/align.h>
#include <util/log.h>

#include <algorithm>

namespace renderer::d3d12 {

void set_uniform_buffer(DXContext &context, MemState &mem, const ShaderProgram *program, const bool vertex_shader,
    const int block_num, const int size, Ptr<uint8_t> data) {
    const uint32_t offset = program->uniform_buffer_data_offsets.at(block_num);
    if (offset == static_cast<uint32_t>(-1))
        return;

    const uint32_t data_size_upload = std::min<uint32_t>(size, program->uniform_buffer_sizes.at(block_num) * 4);
    const uint32_t offset_start_upload = offset * 4;

    UploadRingBuffer &ring = vertex_shader ? context.vertex_uniform_buffer : context.fragment_uniform_buffer;
    UploadRingBuffer::Allocation &allocation = vertex_shader ? context.vertex_uniform_alloc : context.fragment_uniform_alloc;
    bool &allocated = vertex_shader ? context.vertex_uniform_storage_allocated : context.fragment_uniform_storage_allocated;

    if (!allocated) {
        // One region holds every uniform block of the program; the draw resets
        // this so the next draw gets a fresh region.
        allocation = ring.allocate(program->max_total_uniform_buffer_storage * 4);
        if (!allocation.valid()) {
            LOG_ERROR_ONCE("D3D12: the {} uniform ring buffer is full", vertex_shader ? "vertex" : "fragment");
            return;
        }
        allocated = true;
    }

    if (offset_start_upload + data_size_upload > allocation.size)
        return;

    const uint8_t *source = data.get(mem);
    if (!source)
        return;

    memcpy(allocation.cpu + offset_start_upload, source, data_size_upload);
}

void mid_scene_flush(DXContext &context, const SceGxmNotification notification) {
    if (!context.is_recording)
        return;

    // Shader stores written by the vertex stage may be read back as vertex data
    // in the next draw. D3D12 expresses that with a UAV barrier rather than the
    // pipeline barrier the vulkan backend uses.
    context.barriers.uav(nullptr);
    context.barriers.flush(context.render_cmd_list);

    // Same conservative choice as the vulkan backend: always end the scene so
    // the barrier definitely takes effect across the whole pipeline.
    const SceGxmNotification empty_notification = { Ptr<uint32_t>(0), 0 };
    const bool submit = notification.address.address() != 0;

    context.stop_recording(notification, empty_notification, submit);
    context.start_recording();
    context.start_render_pass();
    context.scene_timestamp++;
    context.refresh_pipeline = true;
}

// Copy the CPU descriptors for one stage into the frame ring and return the GPU
// handle the descriptor table should be pointed at.
//
// The table is always the full width the root signature declares, so slots the
// shader does not read still hold a valid descriptor rather than a stale one.
template <D3D12_DESCRIPTOR_HEAP_TYPE HeapType>
static DescriptorHandle build_table(DXContext &context, ShaderVisibleDescriptorRing &ring,
    std::unordered_map<uint64_t, DescriptorHandle> &cache,
    const DescriptorHandle *sources, uint16_t count, DescriptorHandle fallback, uint32_t width) {
    // Identify the table by the descriptors it would hold. The same material
    // bound again produces the same key and reuses the table already built this
    // frame, which is what keeps the heaps from filling up.
    uint64_t key = 1469598103934665603ull;
    auto mix = [&key](SIZE_T value) {
        key = (key ^ static_cast<uint64_t>(value)) * 1099511628211ull;
    };

    mix(width);
    for (uint32_t i = 0; i < width; i++) {
        const DescriptorHandle source = (sources && i < count && sources[i].valid()) ? sources[i] : fallback;
        mix(source.cpu.ptr);
    }

    const auto cached = cache.find(key);
    if (cached != cache.end())
        return cached->second;

    const DescriptorHandle table = ring.allocate(width);
    if (!table.valid()) {
        LOG_ERROR_ONCE("D3D12: ran out of frame descriptors while drawing");
        return {};
    }

    for (uint32_t i = 0; i < width; i++) {
        const DescriptorHandle source = (sources && i < count && sources[i].valid()) ? sources[i] : fallback;
        if (!source.valid())
            continue;

        // The allocation is contiguous, so the i-th descriptor is just an offset
        // from the start of the table.
        D3D12_CPU_DESCRIPTOR_HANDLE destination = table.cpu;
        destination.ptr += static_cast<SIZE_T>(i) * ring.increment();

        context.state.device->CopyDescriptorsSimple(1, destination, source.cpu, HeapType);
    }

    cache.emplace(key, table);
    return table;
}

static void bind_descriptors(DXContext &context, MemState &mem) {
    ID3D12GraphicsCommandList *cmd_list = context.render_cmd_list;
    FrameObject &frame_object = context.state.frame();

    ID3D12DescriptorHeap *heaps[] = {
        frame_object.view_descriptors.handle(),
        frame_object.sampler_descriptors.handle(),
    };
    cmd_list->SetDescriptorHeaps(2, heaps);
    cmd_list->SetGraphicsRootSignature(context.state.pipeline_cache.root_signature());

    // Root descriptors carry the dynamic offsets directly, which is what makes
    // them the natural fit for the ring-buffer uniforms.
    if (context.vertex_info_alloc.valid())
        cmd_list->SetGraphicsRootConstantBufferView(ROOT_PARAM_VERTEX_UNIFORM_BLOCK, context.vertex_info_alloc.gpu);
    if (context.fragment_info_alloc.valid())
        cmd_list->SetGraphicsRootConstantBufferView(ROOT_PARAM_FRAGMENT_UNIFORM_BLOCK, context.fragment_info_alloc.gpu);
    if (context.vertex_uniform_alloc.valid())
        cmd_list->SetGraphicsRootShaderResourceView(ROOT_PARAM_VERTEX_BUFFERS, context.vertex_uniform_alloc.gpu);
    if (context.fragment_uniform_alloc.valid())
        cmd_list->SetGraphicsRootShaderResourceView(ROOT_PARAM_FRAGMENT_BUFFERS, context.fragment_uniform_alloc.gpu);

    // Rebuilding the tables every draw would exhaust the shader-visible sampler
    // heap, which D3D12 caps at 2048 descriptors, after a few dozen draws. They
    // are therefore only rebuilt when a bound texture or sampler changed.
    if (context.textures_dirty) {
        const uint16_t vertex_count = context.record.vertex_program.get(mem)->renderer_data->texture_count;
        const uint16_t fragment_count = context.record.fragment_program.get(mem)->renderer_data->texture_count;

        context.cached_vertex_texture_table = build_table<D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV>(
            context, frame_object.view_descriptors, frame_object.view_table_cache,
            context.vertex_textures, vertex_count,
            context.state.default_srv, SCE_GXM_MAX_TEXTURE_UNITS);
        context.cached_fragment_texture_table = build_table<D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV>(
            context, frame_object.view_descriptors, frame_object.view_table_cache,
            context.fragment_textures, fragment_count,
            context.state.default_srv, SCE_GXM_MAX_TEXTURE_UNITS);

        context.cached_vertex_sampler_table = build_table<D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER>(
            context, frame_object.sampler_descriptors, frame_object.sampler_table_cache,
            context.vertex_samplers, vertex_count,
            context.state.default_sampler, SCE_GXM_MAX_TEXTURE_UNITS);
        context.cached_fragment_sampler_table = build_table<D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER>(
            context, frame_object.sampler_descriptors, frame_object.sampler_table_cache,
            context.fragment_samplers, fragment_count,
            context.state.default_sampler, SCE_GXM_MAX_TEXTURE_UNITS);

        // The attachment table (colour attachment, mask, raw colour) is only read
        // by shaders doing framebuffer fetch; the colour slot holds the snapshot
        // taken for the latest such draw.
        context.cached_attachment_table = build_table<D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV>(
            context, frame_object.view_descriptors, frame_object.view_table_cache,
            &context.fetch_srv, 1, context.state.default_srv, 3);

        context.textures_dirty = false;
    }

    if (context.cached_vertex_texture_table.valid())
        cmd_list->SetGraphicsRootDescriptorTable(ROOT_PARAM_VERTEX_TEXTURES, context.cached_vertex_texture_table.gpu);
    if (context.cached_vertex_sampler_table.valid())
        cmd_list->SetGraphicsRootDescriptorTable(ROOT_PARAM_VERTEX_SAMPLERS, context.cached_vertex_sampler_table.gpu);
    if (context.cached_fragment_texture_table.valid())
        cmd_list->SetGraphicsRootDescriptorTable(ROOT_PARAM_FRAGMENT_TEXTURES, context.cached_fragment_texture_table.gpu);
    if (context.cached_fragment_sampler_table.valid())
        cmd_list->SetGraphicsRootDescriptorTable(ROOT_PARAM_FRAGMENT_SAMPLERS, context.cached_fragment_sampler_table.gpu);
    if (context.cached_attachment_table.valid())
        cmd_list->SetGraphicsRootDescriptorTable(ROOT_PARAM_ATTACHMENTS, context.cached_attachment_table.gpu);
}

static void bind_vertex_streams(DXContext &context, MemState &mem) {
    GxmRecordState &record = context.record;
    const SceGxmVertexProgram &vertex_program = *record.vertex_program.get(mem);
    const VertexProgram *vp = vertex_program.renderer_data.get();

    int max_stream_idx = -1;
    for (const SceGxmVertexAttribute &attribute : vertex_program.attributes) {
        if (!vp->attribute_infos.contains(attribute.regIndex))
            continue;
        max_stream_idx = std::max<int>(max_stream_idx, attribute.streamIndex);
    }
    max_stream_idx++;

    if (max_stream_idx == 0)
        return;

    const std::array<StreamRepack, SCE_GXM_MAX_VERTEX_STREAMS> repack = plan_stream_repack(vertex_program);

    for (int i = 0; i < max_stream_idx; i++) {
        if (!record.vertex_streams[i].data)
            continue;

        const uint8_t *stream = record.vertex_streams[i].data.get(mem);
        const uint32_t stream_size = static_cast<uint32_t>(record.vertex_streams[i].size);
        const uint32_t guest_stride = vertex_program.streams[i].stride;

        uint32_t upload_size = stream_size;
        uint32_t vertex_count = 1;
        if (repack[i].repacked) {
            if (guest_stride != 0)
                vertex_count = (stream_size + guest_stride - 1) / guest_stride;
            upload_size = std::max(vertex_count * repack[i].stride, 4u);
        }

        if (stream && stream_size > 0) {
            const UploadRingBuffer::Allocation allocation = context.vertex_stream_buffer.allocate(upload_size);
            if (allocation.valid()) {
                if (repack[i].repacked) {
                    memset(allocation.cpu, 0, upload_size);
                    for (uint32_t vertex = 0; vertex < vertex_count; vertex++) {
                        const uint32_t source_base = vertex * guest_stride;
                        uint8_t *destination = allocation.cpu + vertex * repack[i].stride;
                        for (const StreamRepack::Copy &copy : repack[i].copies) {
                            const uint32_t source = source_base + copy.source;
                            if (source < stream_size)
                                memcpy(destination + copy.destination, stream + source, std::min(copy.size, stream_size - source));
                        }
                    }
                } else {
                    memcpy(allocation.cpu, stream, stream_size);
                }

                context.vertex_stream_views[i] = D3D12_VERTEX_BUFFER_VIEW{
                    .BufferLocation = allocation.gpu,
                    .SizeInBytes = upload_size,
                    .StrideInBytes = repack[i].repacked ? repack[i].stride : guest_stride,
                };
            } else {
                LOG_ERROR_ONCE("D3D12: the vertex stream ring buffer is full");
            }
        }

        record.vertex_streams[i].data = nullptr;
        record.vertex_streams[i].size = 0;
    }

    context.render_cmd_list->IASetVertexBuffers(0, max_stream_idx, context.vertex_stream_views);
}

// D3D12 has no triangle fan topology, so the indices are rewritten as a list.
// A fan of N vertices becomes N-2 triangles all sharing index 0.
template <typename Index>
static void expand_triangle_fan(const Index *source, size_t count, std::vector<uint8_t> &output) {
    if (count < 3)
        return;

    const size_t triangles = count - 2;
    output.resize(triangles * 3 * sizeof(Index));
    Index *destination = reinterpret_cast<Index *>(output.data());

    for (size_t i = 0; i < triangles; i++) {
        destination[i * 3 + 0] = source[0];
        destination[i * 3 + 1] = source[i + 1];
        destination[i * 3 + 2] = source[i + 2];
    }
}

void draw(DXContext &context, SceGxmPrimitiveType type, SceGxmIndexFormat format,
    Ptr<void> indices, size_t count, uint32_t instance_count, MemState &mem, const Config &config) {
    if (!context.is_recording)
        return;

    if (!context.in_render_pass)
        context.start_render_pass();

    ID3D12GraphicsCommandList *cmd_list = context.render_cmd_list;

    // Occlusion queries.
    if (context.current_visibility_buffer && context.current_query_idx != -1 && !context.is_in_query) {
        if (context.current_visibility_buffer->queries_used[context.current_query_idx])
            LOG_WARN_ONCE("Visibility buffer entry is used more than once in a scene");

        context.current_visibility_buffer->queries_used[context.current_query_idx] = true;
        cmd_list->BeginQuery(context.current_visibility_buffer->query_heap.Get(),
            D3D12_QUERY_TYPE_OCCLUSION, context.current_query_idx);
        context.is_in_query = true;
    }

    if (context.refresh_pipeline || type != context.last_primitive) {
        context.refresh_pipeline = false;
        context.last_primitive = type;

        ID3D12PipelineState *new_pipeline = context.state.pipeline_cache.retrieve_pipeline(context, type, mem);
        if (new_pipeline != context.current_pipeline) {
            context.current_pipeline = new_pipeline;
            if (new_pipeline)
                cmd_list->SetPipelineState(new_pipeline);
        }
    }

    if (!context.current_pipeline)
        return;

    // Earlier draws must be visible to last_frag_data, as the vulkan backend's barrier makes them.
    const SceGxmProgram &fragment_gxp = *context.record.fragment_program.get(mem)->program.get(mem);
    if (fragment_gxp.is_frag_color_used() && context.current_color_surface) {
        const DescriptorHandle srv = context.state.surface_cache.snapshot_for_fetch(*context.current_color_surface,
            cmd_list, context.barriers, context.scissor);
        if (srv.cpu.ptr != context.fetch_srv.cpu.ptr) {
            context.fetch_srv = srv;
            context.textures_dirty = true;
        }
    }

    if (config.log_active_shaders) {
        const std::string hash_text_f = hex_string(context.record.fragment_program.get(mem)->renderer_data->hash);
        const std::string hash_text_v = hex_string(context.record.vertex_program.get(mem)->renderer_data->hash);
        LOG_DEBUG("\nVertex  : {}\nFragment: {}", hash_text_v, hash_text_f);
    }

    // Render info uniform blocks, rebuilt only when something they depend on
    // actually changed.
    auto &vert_ublock = context.curr_vert_ublock.base_block;
    vert_ublock.viewport_flip = context.record.viewport_flip;
    vert_ublock.viewport_flag = context.record.viewport_flat ? 0.0f : 1.0f;
    vert_ublock.z_offset = context.record.z_offset;
    vert_ublock.z_scale = context.record.z_scale;
    vert_ublock.screen_width = context.render_target->width / context.state.res_multiplier;
    vert_ublock.screen_height = context.render_target->height / context.state.res_multiplier;

    if (context.curr_vert_ublock.changed || memcmp(&context.prev_vert_ublock, &vert_ublock, sizeof(vert_ublock)) != 0) {
        context.curr_vert_ublock.copy_to(context.shader_info_temp);
        const uint32_t block_size = context.curr_vert_ublock.get_size();
        context.vertex_info_alloc = context.vertex_info_buffer.allocate(block_size);
        if (context.vertex_info_alloc.valid())
            memcpy(context.vertex_info_alloc.cpu, context.shader_info_temp, block_size);

        memcpy(&context.prev_vert_ublock, &vert_ublock, sizeof(vert_ublock));
    }

    auto &frag_ublock = context.curr_frag_ublock.base_block;
    frag_ublock.writing_mask = context.record.writing_mask;
    frag_ublock.res_multiplier = context.state.res_multiplier;
    const bool has_msaa = context.render_target->multisample_mode;
    const bool has_downscale = context.record.color_surface.downscale;
    if (has_msaa && !has_downscale)
        frag_ublock.res_multiplier *= 2;
    else if (!has_msaa && has_downscale)
        frag_ublock.res_multiplier /= 2;

    if (context.curr_frag_ublock.changed || memcmp(&context.prev_frag_ublock, &frag_ublock, sizeof(frag_ublock)) != 0) {
        context.curr_frag_ublock.copy_to(context.shader_info_temp);
        const uint32_t block_size = context.curr_frag_ublock.get_size();
        context.fragment_info_alloc = context.fragment_info_buffer.allocate(block_size);
        if (context.fragment_info_alloc.valid())
            memcpy(context.fragment_info_alloc.cpu, context.shader_info_temp, block_size);

        memcpy(&context.prev_frag_ublock, &frag_ublock, sizeof(frag_ublock));
    }

    bind_descriptors(context, mem);

    // Index data.
    const size_t index_size = (format == SCE_GXM_INDEX_FORMAT_U16) ? 2 : 4;
    const DXGI_FORMAT index_format = (format == SCE_GXM_INDEX_FORMAT_U16) ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT;

    const void *indices_ptr = indices.get(mem);
    if (!indices_ptr)
        return;

    size_t index_count = count;
    std::vector<uint8_t> expanded_indices;
    if (type == SCE_GXM_PRIMITIVE_TRIANGLE_FAN) {
        if (format == SCE_GXM_INDEX_FORMAT_U16)
            expand_triangle_fan(static_cast<const uint16_t *>(indices_ptr), count, expanded_indices);
        else
            expand_triangle_fan(static_cast<const uint32_t *>(indices_ptr), count, expanded_indices);

        if (expanded_indices.empty())
            return;

        indices_ptr = expanded_indices.data();
        index_count = expanded_indices.size() / index_size;
    }

    const size_t index_buffer_size = index_size * index_count;
    const UploadRingBuffer::Allocation index_allocation = context.index_stream_buffer.allocate(index_buffer_size);
    if (!index_allocation.valid()) {
        LOG_ERROR_ONCE("D3D12: the index ring buffer is full");
        return;
    }

    memcpy(index_allocation.cpu, indices_ptr, index_buffer_size);

    const D3D12_INDEX_BUFFER_VIEW index_view{
        .BufferLocation = index_allocation.gpu,
        .SizeInBytes = static_cast<UINT>(index_buffer_size),
        .Format = index_format,
    };
    cmd_list->IASetIndexBuffer(&index_view);

    bind_vertex_streams(context, mem);

    cmd_list->IASetPrimitiveTopology(translate_primitive(type));
    cmd_list->OMSetStencilRef(context.record.front_stencil_state_values.ref);

    cmd_list->DrawIndexedInstanced(static_cast<UINT>(index_count), instance_count, 0, 0, 0);

    // The next draw gets its own uniform region.
    context.vertex_uniform_storage_allocated = false;
    context.fragment_uniform_storage_allocated = false;
}

void signal_sync_object(DXState &state, SceGxmSyncObject *sync_object, uint32_t timestamp) {
    // Everything recorded so far has been submitted by stop_recording, so the
    // subject can be marked done once that submission retires.
    ::renderer::subject_done(sync_object, timestamp);
}

} // namespace renderer::d3d12
