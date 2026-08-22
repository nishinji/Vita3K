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
#include <gxm/functions.h>
#include <mem/state.h>
#include <shader/spirv_recompiler.h>
#include <util/log.h>

#include <SPIRV/spirv.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>

namespace renderer::software {

namespace {

// Reads one component of a vertex attribute and normalizes it the way the GPU
// would before it reaches the shader.
float read_attribute_component(SceGxmAttributeFormat format, const uint8_t *data, uint32_t index) {
    switch (format) {
    case SCE_GXM_ATTRIBUTE_FORMAT_U8:
        return static_cast<float>(data[index]);
    case SCE_GXM_ATTRIBUTE_FORMAT_S8:
        return static_cast<float>(static_cast<int8_t>(data[index]));
    case SCE_GXM_ATTRIBUTE_FORMAT_U8N:
        return data[index] / 255.0f;
    case SCE_GXM_ATTRIBUTE_FORMAT_S8N:
        return std::max(static_cast<int8_t>(data[index]) / 127.0f, -1.0f);
    case SCE_GXM_ATTRIBUTE_FORMAT_U16: {
        uint16_t value;
        std::memcpy(&value, data + index * sizeof(uint16_t), sizeof(value));
        return static_cast<float>(value);
    }
    case SCE_GXM_ATTRIBUTE_FORMAT_S16: {
        int16_t value;
        std::memcpy(&value, data + index * sizeof(int16_t), sizeof(value));
        return static_cast<float>(value);
    }
    case SCE_GXM_ATTRIBUTE_FORMAT_U16N: {
        uint16_t value;
        std::memcpy(&value, data + index * sizeof(uint16_t), sizeof(value));
        return value / 65535.0f;
    }
    case SCE_GXM_ATTRIBUTE_FORMAT_S16N: {
        int16_t value;
        std::memcpy(&value, data + index * sizeof(int16_t), sizeof(value));
        return std::max(value / 32767.0f, -1.0f);
    }
    case SCE_GXM_ATTRIBUTE_FORMAT_F16: {
        uint16_t value;
        std::memcpy(&value, data + index * sizeof(uint16_t), sizeof(value));
        return half_to_float(value);
    }
    case SCE_GXM_ATTRIBUTE_FORMAT_F32: {
        float value;
        std::memcpy(&value, data + index * sizeof(float), sizeof(value));
        return value;
    }
    default: {
        // Untyped attributes are handed to the shader as raw bits.
        uint32_t value;
        std::memcpy(&value, data + index * sizeof(uint32_t), sizeof(value));
        float result;
        std::memcpy(&result, &value, sizeof(result));
        return result;
    }
    }
}

uint32_t vertices_per_primitive(SceGxmPrimitiveType type) {
    switch (type) {
    case SCE_GXM_PRIMITIVE_POINTS:
        return 1;
    case SCE_GXM_PRIMITIVE_LINES:
        return 2;
    default:
        return 3;
    }
}

// Expands an index list into the flat triple-per-triangle form the rasterizer
// consumes, so strips and fans never reach it.
void assemble_primitives(SceGxmPrimitiveType type, const std::vector<uint32_t> &indices,
    std::vector<uint32_t> &out) {
    const size_t count = indices.size();

    switch (type) {
    case SCE_GXM_PRIMITIVE_TRIANGLE_STRIP:
        for (size_t i = 0; i + 2 < count; i++) {
            // Every other triangle of a strip has its winding flipped.
            if (i % 2 == 0) {
                out.push_back(indices[i]);
                out.push_back(indices[i + 1]);
                out.push_back(indices[i + 2]);
            } else {
                out.push_back(indices[i + 1]);
                out.push_back(indices[i]);
                out.push_back(indices[i + 2]);
            }
        }
        break;

    case SCE_GXM_PRIMITIVE_TRIANGLE_FAN:
        for (size_t i = 1; i + 1 < count; i++) {
            out.push_back(indices[0]);
            out.push_back(indices[i]);
            out.push_back(indices[i + 1]);
        }
        break;

    case SCE_GXM_PRIMITIVE_POINTS:
        for (size_t i = 0; i < count; i++)
            out.push_back(indices[i]);
        break;

    case SCE_GXM_PRIMITIVE_LINES:
        for (size_t i = 0; i + 1 < count; i += 2) {
            out.push_back(indices[i]);
            out.push_back(indices[i + 1]);
        }
        break;

    default:
        for (size_t i = 0; i + 2 < count; i += 3) {
            out.push_back(indices[i]);
            out.push_back(indices[i + 1]);
            out.push_back(indices[i + 2]);
        }
        break;
    }
}

// Signed distance of a clip space position to the planes of Vulkan's depth
// clip volume: w above zero, then 0 <= z <= w. Behind the eye the perspective
// divide would turn a primitive inside out instead of cutting it off.
float clip_distance(const Vec4f &clip, uint32_t plane) {
    constexpr float MIN_W = 1e-6f;
    switch (plane) {
    case 0:
        return clip[3] - MIN_W;
    case 1:
        return clip[2];
    default:
        return clip[3] - clip[2];
    }
}

// Cuts the primitives down to the clip volume, adding the vertices where they
// cross a plane. Varyings are interpolated in clip space, as a GPU does.
void clip_primitives(std::vector<ShadedVertex> &vertices, std::vector<float> &varyings, uint32_t floats_per_vertex,
    std::vector<uint32_t> &indices, uint32_t per_primitive) {
    const auto inside = [&](uint32_t index) {
        const Vec4f &clip = vertices[index].clip;
        return clip_distance(clip, 0) >= 0.0f && clip_distance(clip, 1) >= 0.0f && clip_distance(clip, 2) >= 0.0f;
    };

    // Draws that stay inside, which is nearly all of them, pay only for this.
    if (std::all_of(indices.begin(), indices.end(), inside))
        return;

    const auto interpolate = [&](uint32_t a, uint32_t b, float t) {
        const ShadedVertex from = vertices[a];
        const ShadedVertex to = vertices[b];

        ShadedVertex vertex;
        for (uint32_t i = 0; i < 4; i++)
            vertex.clip[i] = from.clip[i] + (to.clip[i] - from.clip[i]) * t;
        vertex.point_size = from.point_size + (to.point_size - from.point_size) * t;
        vertex.varying_index = static_cast<uint32_t>(vertices.size());

        if (floats_per_vertex) {
            varyings.resize(varyings.size() + floats_per_vertex);
            const float *source = varyings.data() + static_cast<size_t>(from.varying_index) * floats_per_vertex;
            const float *target = varyings.data() + static_cast<size_t>(to.varying_index) * floats_per_vertex;
            float *result = varyings.data() + static_cast<size_t>(vertex.varying_index) * floats_per_vertex;
            for (uint32_t i = 0; i < floats_per_vertex; i++)
                result[i] = source[i] + (target[i] - source[i]) * t;
        }

        vertices.push_back(vertex);
        return vertex.varying_index;
    };

    std::vector<uint32_t> output;
    output.reserve(indices.size());
    std::vector<uint32_t> polygon;
    std::vector<uint32_t> clipped;

    for (size_t first = 0; first + per_primitive <= indices.size(); first += per_primitive) {
        polygon.assign(indices.begin() + first, indices.begin() + first + per_primitive);

        bool keep = true;
        for (uint32_t plane = 0; plane < 3 && keep; plane++) {
            const bool crosses = std::any_of(polygon.begin(), polygon.end(), [&](uint32_t index) {
                return clip_distance(vertices[index].clip, plane) < 0.0f;
            });
            if (!crosses)
                continue;

            if (per_primitive == 1) {
                keep = false;
                break;
            }

            if (per_primitive == 2) {
                const float from = clip_distance(vertices[polygon[0]].clip, plane);
                const float to = clip_distance(vertices[polygon[1]].clip, plane);
                if (from < 0.0f && to < 0.0f) {
                    keep = false;
                    break;
                }
                const uint32_t cut = interpolate(polygon[0], polygon[1], from / (from - to));
                polygon[(from < 0.0f) ? 0 : 1] = cut;
                continue;
            }

            clipped.clear();
            for (size_t i = 0; i < polygon.size(); i++) {
                const uint32_t a = polygon[i];
                const uint32_t b = polygon[(i + 1) % polygon.size()];
                const float from = clip_distance(vertices[a].clip, plane);
                const float to = clip_distance(vertices[b].clip, plane);
                if (from >= 0.0f)
                    clipped.push_back(a);
                if ((from >= 0.0f) != (to >= 0.0f))
                    clipped.push_back(interpolate(a, b, from / (from - to)));
            }
            polygon.swap(clipped);
            keep = polygon.size() >= 3;
        }

        if (!keep)
            continue;

        if (per_primitive == 3) {
            // The clipped polygon is convex and keeps the winding, so a fan covers it.
            for (size_t k = 1; k + 1 < polygon.size(); k++) {
                output.push_back(polygon[0]);
                output.push_back(polygon[k]);
                output.push_back(polygon[k + 1]);
            }
        } else {
            output.insert(output.end(), polygon.begin(), polygon.end());
        }
    }

    indices.swap(output);
}

} // namespace

bool set_uniform_buffer(SWContext &context, const ShaderProgram *program, bool is_vertex, int block_num,
    uint32_t size, const uint8_t *data) {
    const uint32_t offset = program->uniform_buffer_data_offsets.at(block_num);
    if (offset == static_cast<uint32_t>(-1))
        return true;

    std::vector<uint8_t> &storage = is_vertex ? context.vertex_uniform_storage : context.fragment_uniform_storage;
    const size_t needed = program->max_total_uniform_buffer_storage * 4;
    if (storage.size() < needed)
        storage.resize(needed, 0);

    const size_t copy = std::min<size_t>(size, program->uniform_buffer_sizes.at(block_num) * 4ull);
    const size_t start = static_cast<size_t>(offset) * 4;
    if (start + copy > storage.size() || !data)
        return false;

    std::memcpy(storage.data() + start, data, copy);
    return true;
}

void set_texture(SWContext &context, uint32_t index, const SceGxmTexture &texture) {
    if (index >= context.textures.size())
        return;

    context.textures[index] = texture;
}

void sync_viewport_real(SWContext &context, float xOffset, float yOffset, float xScale, float yScale) {
    context.viewport_offset = { xOffset, yOffset };
    context.viewport_scale = { xScale, yScale };
}

void set_context(SWState &state, SWContext &context, MemState &mem, SWRenderTarget *rt) {
    context.render_target = rt;

    context.color_target = state.surface_cache.bind_color(context.record.color_surface, mem);

    const uint32_t width = context.color_target.valid()
        ? context.color_target.width
        : (rt ? rt->width : DEFAULT_RES_WIDTH);
    const uint32_t height = context.color_target.valid()
        ? context.color_target.height
        : (rt ? rt->height : DEFAULT_RES_HEIGHT);

    context.depth_stencil_target = state.surface_cache.bind_depth_stencil(context.record.depth_stencil_surface,
        width, height);

    // GXM clears the depth and stencil at the start of a scene unless the game
    // explicitly asked for the previous contents to be loaded back.
    if (context.depth_stencil_target && !context.record.depth_stencil_surface.force_load) {
        if (context.depth_stencil_target->has_depth)
            std::fill(context.depth_stencil_target->depth.begin(), context.depth_stencil_target->depth.end(),
                context.record.depth_stencil_surface.background_depth);
        if (context.depth_stencil_target->has_stencil)
            std::fill(context.depth_stencil_target->stencil.begin(), context.depth_stencil_target->stencil.end(),
                static_cast<uint8_t>(context.record.depth_stencil_surface.stencil));
    }
}

void mid_scene_flush(SWContext &context, const SceGxmNotification notification) {
    // Nothing is queued up on a device, so a mid scene flush has already
    // happened by the time the command reaches this backend.
}

void draw(SWState &state, SWContext &context, SceGxmPrimitiveType type, SceGxmIndexFormat format,
    const void *indices, size_t count, uint32_t instance_count, MemState &mem, const Config &config) {
    state.counters.entered.fetch_add(1, std::memory_order_relaxed);

    if (!context.color_target.valid()) {
        state.counters.no_color_target.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    const SceGxmFragmentProgram &gxm_fragment_program = *context.record.fragment_program.get(mem);
    const SceGxmVertexProgram &gxm_vertex_program = *context.record.vertex_program.get(mem);

    const auto *fragment_program = static_cast<const SWFragmentProgram *>(gxm_fragment_program.renderer_data.get());
    const auto *vertex_program = static_cast<const SWVertexProgram *>(gxm_vertex_program.renderer_data.get());
    if (!fragment_program || !vertex_program || !fragment_program->program || !vertex_program->program) {
        state.counters.no_program.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    if (gxm_fragment_program.is_maskupdate && !state.features.use_mask_bit) {
        state.counters.mask_skipped.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    // Recompile and parse both stages, or take them from the cache.
    shader::Hints hints{
        .attributes = &gxm_vertex_program.attributes,
        .color_format = context.record.color_surface.colorFormat,
    };
    for (uint32_t i = 0; i < SCE_GXM_MAX_TEXTURE_UNITS; i++) {
        hints.fragment_textures[i] = gxm::get_format(context.textures[i]);
        hints.vertex_textures[i] = gxm::get_format(context.textures[SCE_GXM_MAX_TEXTURE_UNITS + i]);
    }

    SWShaderKey vertex_key;
    vertex_key.hash = vertex_program->hash;
    SWShaderKey fragment_key;
    fragment_key.hash = fragment_program->hash;
    fragment_key.color_format = static_cast<uint32_t>(context.record.color_surface.colorFormat);
    fragment_key.is_maskupdate = gxm_fragment_program.is_maskupdate;

    const SWShaderPtr vertex_shader = state.shader_cache.get(vertex_key, *vertex_program->program,
        state.features, hints, false, hex_string(vertex_program->hash));
    const SWShaderPtr fragment_shader = state.shader_cache.get(fragment_key, *fragment_program->program,
        state.features, hints, gxm_fragment_program.is_maskupdate, hex_string(fragment_program->hash));

    if (!vertex_shader || !vertex_shader->valid || !fragment_shader || !fragment_shader->valid) {
        state.counters.shader_invalid.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    // Render info blocks, exactly as the GPU backends build them.
    shader::RenderVertUniformBlock &vert_block = context.current_vert_render_info;
    vert_block.viewport_flip = context.record.viewport_flip;
    vert_block.viewport_flag = context.record.viewport_flat ? 0.0f : 1.0f;
    vert_block.z_offset = context.record.z_offset;
    vert_block.z_scale = context.record.z_scale;
    vert_block.screen_width = static_cast<float>(context.record.color_surface.width);
    vert_block.screen_height = static_cast<float>(context.record.color_surface.height);

    shader::RenderFragUniformBlock &frag_block = context.current_frag_render_info;
    const bool both_sides_disabled = (context.record.front_side_fragment_program_mode == SCE_GXM_FRAGMENT_PROGRAM_DISABLED)
        && ((context.record.back_side_fragment_program_mode == SCE_GXM_FRAGMENT_PROGRAM_DISABLED)
            || (context.record.two_sided == SCE_GXM_TWO_SIDED_DISABLED));

    if (both_sides_disabled) {
        frag_block.front_disabled = 0.0f;
        frag_block.back_disabled = 0.0f;
    } else {
        frag_block.front_disabled = (context.record.front_side_fragment_program_mode == SCE_GXM_FRAGMENT_PROGRAM_DISABLED) ? 1.0f : 0.0f;
        frag_block.back_disabled = (context.record.two_sided == SCE_GXM_TWO_SIDED_DISABLED)
            ? frag_block.front_disabled
            : ((context.record.back_side_fragment_program_mode == SCE_GXM_FRAGMENT_PROGRAM_DISABLED) ? 1.0f : 0.0f);
    }
    frag_block.writing_mask = context.record.writing_mask;
    frag_block.use_raw_image = 0.0f;
    frag_block.res_multiplier = state.res_multiplier;

    // Descriptor set 0 holds, in order: the vertex render info, the fragment
    // render info, then the two uniform buffer containers.
    SpirvBindings vertex_bindings;
    vertex_bindings.uniform_buffers.resize(4);
    vertex_bindings.uniform_buffers[0] = { reinterpret_cast<const uint8_t *>(&vert_block), sizeof(vert_block) };
    vertex_bindings.uniform_buffers[2] = { context.vertex_uniform_storage.data(),
        static_cast<uint32_t>(context.vertex_uniform_storage.size()) };

    SpirvBindings fragment_bindings;
    fragment_bindings.uniform_buffers.resize(4);
    fragment_bindings.uniform_buffers[1] = { reinterpret_cast<const uint8_t *>(&frag_block), sizeof(frag_block) };
    fragment_bindings.uniform_buffers[3] = { context.fragment_uniform_storage.data(),
        static_cast<uint32_t>(context.fragment_uniform_storage.size()) };
    fragment_bindings.spec_constants[shader::GAMMA_CORRECTION_SPECIALIZATION_ID] = context.record.is_gamma_corrected ? 1u : 0u;

    // Upload every texture the two stages use before any invocation runs.
    state.texture_cache.reset_bindings();
    for (uint32_t i = 0; i < fragment_program->texture_count && i < SCE_GXM_MAX_TEXTURE_UNITS; i++) {
        if (fragment_program->textures_used[i])
            state.texture_cache.bind(false, i, context.textures[i], mem);
    }
    for (uint32_t i = 0; i < vertex_program->texture_count && i < SCE_GXM_MAX_TEXTURE_UNITS; i++) {
        if (vertex_program->textures_used[i])
            state.texture_cache.bind(true, i, context.textures[SCE_GXM_MAX_TEXTURE_UNITS + i], mem);
    }

    // Gather the indices, then work out how many distinct vertices to shade.
    std::vector<uint32_t> index_list(count);
    if (format == SCE_GXM_INDEX_FORMAT_U16) {
        const uint16_t *source = static_cast<const uint16_t *>(indices);
        for (size_t i = 0; i < count; i++)
            index_list[i] = source[i];
    } else {
        const uint32_t *source = static_cast<const uint32_t *>(indices);
        for (size_t i = 0; i < count; i++)
            index_list[i] = source[i];
    }

    if (index_list.empty()) {
        state.counters.no_indices.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    const uint32_t highest_index = *std::max_element(index_list.begin(), index_list.end());
    const uint32_t vertex_count = highest_index + 1;

    const VaryingLayout varyings = build_varying_layout(fragment_shader->module);

    // Run the vertex stage. Each vertex is shaded once per instance, which is
    // what the index list addresses into.
    SpirvInterpreter vertex_interpreter;
    vertex_interpreter.set_program(&vertex_shader->module, &vertex_bindings);

    // The sampler callback of the vertex stage only ever reaches real textures.
    const SWTextureCache *textures = &state.texture_cache;
    vertex_bindings.sample_texture = [textures](uint32_t descriptor_set, uint32_t binding, uint32_t lane_mask,
                                         const float *coords, uint32_t coord_count, const float *lods, bool has_lod,
                                         float *out_rgba) {
        const SWSampler *sampler = textures->bound_sampler(true, binding);
        for (uint32_t lane = 0; lane < SPIRV_MAX_LANES; lane++) {
            if (!(lane_mask & (1u << lane)))
                continue;
            float *out = out_rgba + lane * 4;
            if (!sampler) {
                out[0] = out[1] = out[2] = 0.0f;
                out[3] = 1.0f;
                continue;
            }

            const float *coord = coords + lane * 4;
            const Vec4f sampled = sampler->sample(coord[0], (coord_count > 1) ? coord[1] : 0.0f,
                has_lod ? lods[lane] : 0.0f);
            for (uint32_t i = 0; i < 4; i++)
                out[i] = sampled[i];
        }
    };

    // Resolve the vertex stage interface once.
    struct AttributeBinding {
        SpirvLaneStorage storage;
        const uint8_t *stream = nullptr;
        uint32_t stride = 0;
        uint32_t offset = 0;
        uint32_t component_count = 0;
        SceGxmAttributeFormat format = SCE_GXM_ATTRIBUTE_FORMAT_F32;
        bool per_instance = false;
    };

    std::vector<AttributeBinding> attribute_bindings;
    for (const SceGxmVertexAttribute &attribute : gxm_vertex_program.attributes) {
        const auto info = vertex_program->attribute_infos.find(attribute.regIndex);
        if (info == vertex_program->attribute_infos.end())
            continue;

        const GXMStreamInfo &stream_info = context.record.vertex_streams[attribute.streamIndex];
        if (!stream_info.data)
            continue;

        AttributeBinding binding;
        binding.storage = vertex_interpreter.input_by_location(info->second.location);
        binding.stream = stream_info.data.get(mem);
        binding.stride = gxm_vertex_program.streams[attribute.streamIndex].stride;
        binding.offset = attribute.offset;
        binding.component_count = attribute.componentCount;
        binding.format = attribute.format;
        const uint16_t index_source = gxm_vertex_program.streams[attribute.streamIndex].indexSource;
        binding.per_instance = (index_source == SCE_GXM_INDEX_SOURCE_EACH_INSTANCE_16BIT)
            || (index_source == SCE_GXM_INDEX_SOURCE_EACH_INSTANCE_32BIT);

        if (binding.storage && binding.stream)
            attribute_bindings.push_back(binding);
    }

    SpirvLaneStorage vertex_index_builtin = vertex_interpreter.builtin_storage(spv::BuiltInVertexIndex);
    if (!vertex_index_builtin)
        vertex_index_builtin = vertex_interpreter.builtin_storage(spv::BuiltInVertexId);
    SpirvLaneStorage instance_index_builtin = vertex_interpreter.builtin_storage(spv::BuiltInInstanceIndex);
    if (!instance_index_builtin)
        instance_index_builtin = vertex_interpreter.builtin_storage(spv::BuiltInInstanceId);

    const SpirvLaneStorage position_output = vertex_interpreter.builtin_storage(spv::BuiltInPosition);
    const SpirvLaneStorage point_size_output = vertex_interpreter.builtin_storage(spv::BuiltInPointSize);

    std::vector<SpirvLaneStorage> varying_outputs(varyings.slots.size());
    for (size_t i = 0; i < varyings.slots.size(); i++)
        varying_outputs[i] = vertex_interpreter.output_by_location(varyings.slots[i].location);

    // A flat viewport spans the surface, which undoes the pixel-to-NDC mapping the shader applied.
    const float surface_width = static_cast<float>(context.color_target.width);
    const float surface_height = static_cast<float>(context.color_target.height);
    const std::array<float, 2> viewport_offset = context.record.viewport_flat
        ? std::array<float, 2>{ surface_width * 0.5f, surface_height * 0.5f }
        : context.viewport_offset;
    const std::array<float, 2> viewport_scale = context.record.viewport_flat
        ? std::array<float, 2>{ surface_width * 0.5f, surface_height * 0.5f }
        : context.viewport_scale;

    std::vector<ShadedVertex> shaded;
    std::vector<float> varying_storage;
    varying_storage.resize(static_cast<size_t>(vertex_count) * instance_count * varyings.total_floats, 0.0f);

    shaded.resize(static_cast<size_t>(vertex_count) * instance_count);

    for (uint32_t instance = 0; instance < instance_count; instance++) {
        for (uint32_t first = 0; first < vertex_count; first += SPIRV_MAX_LANES) {
            const uint32_t lane_count = std::min(vertex_count - first, SPIRV_MAX_LANES);

            for (uint32_t lane = 0; lane < lane_count; lane++) {
                const uint32_t vertex = first + lane;
                if (instance_index_builtin)
                    std::memcpy(instance_index_builtin.lane(lane), &instance, sizeof(instance));
                if (vertex_index_builtin)
                    std::memcpy(vertex_index_builtin.lane(lane), &vertex, sizeof(vertex));

                for (const AttributeBinding &binding : attribute_bindings) {
                    const uint32_t element = binding.per_instance ? instance : vertex;
                    const uint8_t *source = binding.stream + static_cast<size_t>(element) * binding.stride
                        + binding.offset;

                    float components[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
                    for (uint32_t i = 0; i < std::min(binding.component_count, 4u); i++)
                        components[i] = read_attribute_component(binding.format, source, i);

                    std::memcpy(binding.storage.lane(lane), components, sizeof(components));
                }
            }

            vertex_interpreter.execute((1u << lane_count) - 1u);

            for (uint32_t lane = 0; lane < lane_count; lane++) {
                ShadedVertex &output = shaded[static_cast<size_t>(instance) * vertex_count + first + lane];
                output.varying_index = instance * vertex_count + first + lane;

                output.clip = { 0.0f, 0.0f, 0.0f, 1.0f };
                if (position_output)
                    std::memcpy(output.clip.data(), position_output.lane(lane), sizeof(float) * 4);

                if (point_size_output)
                    std::memcpy(&output.point_size, point_size_output.lane(lane), sizeof(float));

                float *destination = varying_storage.data()
                    + static_cast<size_t>(output.varying_index) * varyings.total_floats;
                for (size_t i = 0; i < varyings.slots.size(); i++) {
                    if (!varying_outputs[i])
                        continue;
                    std::memcpy(destination + varyings.slots[i].offset, varying_outputs[i].lane(lane),
                        static_cast<size_t>(varyings.slots[i].float_count) * sizeof(float));
                }
            }
        }
    }

    // Assemble the primitives, rebasing the indices onto the instance that
    // produced them.
    std::vector<uint32_t> primitive_indices;
    primitive_indices.reserve(index_list.size());

    for (uint32_t instance = 0; instance < instance_count; instance++) {
        std::vector<uint32_t> instance_indices(index_list.size());
        for (size_t i = 0; i < index_list.size(); i++)
            instance_indices[i] = index_list[i] + instance * vertex_count;

        assemble_primitives(type, instance_indices, primitive_indices);
    }

    const uint32_t per_primitive = vertices_per_primitive(type);
    clip_primitives(shaded, varying_storage, varyings.total_floats, primitive_indices, per_primitive);
    if (primitive_indices.size() < per_primitive) {
        state.counters.no_primitives.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    // Perspective divide and the viewport transform. The recompiler targets
    // Vulkan clip space, so Z already runs from zero to one.
    for (ShadedVertex &vertex : shaded) {
        const float inverse_w = (vertex.clip[3] != 0.0f) ? (1.0f / vertex.clip[3]) : 0.0f;
        vertex.position[0] = viewport_offset[0] + viewport_scale[0] * vertex.clip[0] * inverse_w;
        vertex.position[1] = viewport_offset[1] + viewport_scale[1] * vertex.clip[1] * inverse_w;
        vertex.position[2] = vertex.clip[2] * inverse_w;
        vertex.position[3] = inverse_w;
    }

    DrawCall call;
    call.fragment_module = &fragment_shader->module;
    call.fragment_bindings = &fragment_bindings;
    call.textures = &state.texture_cache;
    call.color = context.color_target;
    call.depth_stencil = context.depth_stencil_target;
    call.varyings = varyings;
    call.varying_storage = varying_storage.data();
    call.vertices = shaded.data();
    call.primitive_indices = primitive_indices.data();
    call.primitive_count = static_cast<uint32_t>(primitive_indices.size() / per_primitive);
    call.primitive_type = type;

    call.front.depth_func = context.record.front_depth_func;
    call.front.depth_write = context.record.front_depth_write_mode == SCE_GXM_DEPTH_WRITE_ENABLED;
    call.front.stencil_op = context.record.front_stencil_state_op;
    call.front.stencil_values = context.record.front_stencil_state_values;
    call.front.fragment_program_enabled = context.record.front_side_fragment_program_mode == SCE_GXM_FRAGMENT_PROGRAM_ENABLED;
    call.front.polygon_mode = context.record.front_polygon_mode;

    call.back.depth_func = context.record.back_depth_func;
    call.back.depth_write = context.record.back_depth_write_mode == SCE_GXM_DEPTH_WRITE_ENABLED;
    call.back.stencil_op = context.record.back_stencil_state_op;
    call.back.stencil_values = context.record.back_stencil_state_values;
    call.back.fragment_program_enabled = context.record.back_side_fragment_program_mode == SCE_GXM_FRAGMENT_PROGRAM_ENABLED;
    call.back.polygon_mode = context.record.back_polygon_mode;

    call.cull_mode = context.record.cull_mode;
    call.two_sided = context.record.two_sided == SCE_GXM_TWO_SIDED_ENABLED;
    call.blend = fragment_program->blend;

    call.region_clip_mode = context.record.region_clip_mode;
    call.clip_min_x = context.record.region_clip_min.x;
    call.clip_min_y = context.record.region_clip_min.y;
    call.clip_max_x = context.record.region_clip_max.x;
    call.clip_max_y = context.record.region_clip_max.y;
    if (call.region_clip_mode == SCE_GXM_REGION_CLIP_NONE
        || call.region_clip_mode == SCE_GXM_REGION_CLIP_ALL
        || call.clip_max_x <= call.clip_min_x
        || call.clip_max_y <= call.clip_min_y) {
        call.clip_min_x = 0;
        call.clip_min_y = 0;
        call.clip_max_x = static_cast<int32_t>(context.color_target.width) - 1;
        call.clip_max_y = static_cast<int32_t>(context.color_target.height) - 1;
    }

    // GXM stores the bias in units of the depth buffer resolution.
    call.depth_bias_units = static_cast<float>(context.record.depth_bias_unit) / 65536.0f;
    call.depth_bias_slope = static_cast<float>(context.record.depth_bias_slope);
    call.line_width = static_cast<float>(std::max(1u, context.record.line_width));

    call.depth_enabled = !context.record.depth_stencil_surface.disabled() && !context.record.is_maskupdate;
    call.stencil_enabled = call.depth_enabled;
    call.is_maskupdate = context.record.is_maskupdate;

    std::atomic<uint32_t> visible_pixels{ 0 };
    call.visible_counter = &visible_pixels;

    // Otherwise every worker faults at once on a surface the texture cache protects, and all but one fault find the protection already gone.
    const bool writes_color = !call.is_maskupdate && (call.front.fragment_program_enabled || call.back.fragment_program_enabled);
    if (writes_color) {
        const SWColorTarget &color = context.color_target;
        const uint32_t surface_size = color_pixel_offset(color.surface_type, color.width - 1, color.height - 1,
                                          color.stride_in_pixels, color.height, color.format.bytes_per_pixel)
            + color.format.bytes_per_pixel;
        unprotect_for_write(mem, context.record.color_surface.data.address(), surface_size);
    }

    state.rasterizer->run(call);

    {
        const ShadedVertex &first = shaded.front();
        state.counters.rasterized.fetch_add(1, std::memory_order_relaxed);
        state.counters.primitives.fetch_add(call.primitive_count, std::memory_order_relaxed);
        state.counters.pixels.fetch_add(visible_pixels.load(std::memory_order_relaxed),
            std::memory_order_relaxed);
        if (first.position[3] != 0.0f)
            state.counters.nonzero_w.fetch_add(1, std::memory_order_relaxed);
    }
    if (context.visibility_enabled && context.visibility_buffer) {
        uint32_t *visibility = context.visibility_buffer.get(mem);
        if (visibility) {
            const size_t slot = static_cast<size_t>(context.visibility_index) * context.visibility_stride
                / sizeof(uint32_t);
            const uint32_t counted = visible_pixels.load(std::memory_order_relaxed);
            if (context.visibility_increment)
                visibility[slot] += counted;
            else
                visibility[slot] = counted;
        }
    }

    context.last_draw_vertex_program_hash = vertex_program->hash;
    context.last_draw_fragment_program_hash = fragment_program->hash;
}

} // namespace renderer::software
