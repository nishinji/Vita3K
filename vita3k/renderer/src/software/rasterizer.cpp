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

#include <renderer/software/rasterizer.h>

#include <renderer/software/texture.h>
#include <util/log.h>

#include <SPIRV/spirv.hpp>

#include <algorithm>
#include <cmath>

namespace renderer::software {

namespace {

bool depth_test_passes(SceGxmDepthFunc func, float source, float destination) {
    switch (func) {
    case SCE_GXM_DEPTH_FUNC_NEVER:
        return false;
    case SCE_GXM_DEPTH_FUNC_LESS:
        return source < destination;
    case SCE_GXM_DEPTH_FUNC_EQUAL:
        return source == destination;
    case SCE_GXM_DEPTH_FUNC_LESS_EQUAL:
        return source <= destination;
    case SCE_GXM_DEPTH_FUNC_GREATER:
        return source > destination;
    case SCE_GXM_DEPTH_FUNC_NOT_EQUAL:
        return source != destination;
    case SCE_GXM_DEPTH_FUNC_GREATER_EQUAL:
        return source >= destination;
    default:
        return true;
    }
}

bool stencil_test_passes(SceGxmStencilFunc func, uint8_t reference, uint8_t value, uint8_t mask) {
    const uint8_t masked_reference = reference & mask;
    const uint8_t masked_value = value & mask;

    switch (func) {
    case SCE_GXM_STENCIL_FUNC_NEVER:
        return false;
    case SCE_GXM_STENCIL_FUNC_LESS:
        return masked_reference < masked_value;
    case SCE_GXM_STENCIL_FUNC_EQUAL:
        return masked_reference == masked_value;
    case SCE_GXM_STENCIL_FUNC_LESS_EQUAL:
        return masked_reference <= masked_value;
    case SCE_GXM_STENCIL_FUNC_GREATER:
        return masked_reference > masked_value;
    case SCE_GXM_STENCIL_FUNC_NOT_EQUAL:
        return masked_reference != masked_value;
    case SCE_GXM_STENCIL_FUNC_GREATER_EQUAL:
        return masked_reference >= masked_value;
    default:
        return true;
    }
}

uint8_t apply_stencil_op(SceGxmStencilOp op, uint8_t current, uint8_t reference) {
    switch (op) {
    case SCE_GXM_STENCIL_OP_ZERO:
        return 0;
    case SCE_GXM_STENCIL_OP_REPLACE:
        return reference;
    case SCE_GXM_STENCIL_OP_INCR:
        return (current == 0xFF) ? 0xFF : static_cast<uint8_t>(current + 1);
    case SCE_GXM_STENCIL_OP_DECR:
        return (current == 0) ? 0 : static_cast<uint8_t>(current - 1);
    case SCE_GXM_STENCIL_OP_INVERT:
        return static_cast<uint8_t>(~current);
    case SCE_GXM_STENCIL_OP_INCR_WRAP:
        return static_cast<uint8_t>(current + 1);
    case SCE_GXM_STENCIL_OP_DECR_WRAP:
        return static_cast<uint8_t>(current - 1);
    default:
        return current;
    }
}

float blend_factor(SceGxmBlendFactor factor, const Vec4f &source, const Vec4f &destination, uint32_t channel) {
    switch (factor) {
    case SCE_GXM_BLEND_FACTOR_ZERO:
        return 0.0f;
    case SCE_GXM_BLEND_FACTOR_ONE:
        return 1.0f;
    case SCE_GXM_BLEND_FACTOR_SRC_COLOR:
        return source[channel];
    case SCE_GXM_BLEND_FACTOR_ONE_MINUS_SRC_COLOR:
        return 1.0f - source[channel];
    case SCE_GXM_BLEND_FACTOR_SRC_ALPHA:
        return source[3];
    case SCE_GXM_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA:
        return 1.0f - source[3];
    case SCE_GXM_BLEND_FACTOR_DST_COLOR:
        return destination[channel];
    case SCE_GXM_BLEND_FACTOR_ONE_MINUS_DST_COLOR:
        return 1.0f - destination[channel];
    case SCE_GXM_BLEND_FACTOR_DST_ALPHA:
        return destination[3];
    case SCE_GXM_BLEND_FACTOR_ONE_MINUS_DST_ALPHA:
        return 1.0f - destination[3];
    case SCE_GXM_BLEND_FACTOR_SRC_ALPHA_SATURATE:
        return (channel == 3) ? 1.0f : std::min(source[3], 1.0f - destination[3]);
    case SCE_GXM_BLEND_FACTOR_DST_ALPHA_SATURATE:
        return (channel == 3) ? 1.0f : std::min(destination[3], 1.0f - source[3]);
    default:
        return 1.0f;
    }
}

float blend_channel(SceGxmBlendFunc func, float source, float destination) {
    switch (func) {
    case SCE_GXM_BLEND_FUNC_SUBTRACT:
        return source - destination;
    case SCE_GXM_BLEND_FUNC_REVERSE_SUBTRACT:
        return destination - source;
    case SCE_GXM_BLEND_FUNC_MIN:
        return std::min(source, destination);
    case SCE_GXM_BLEND_FUNC_MAX:
        return std::max(source, destination);
    default:
        return source + destination;
    }
}

Vec4f apply_blend(const SWBlendState &blend, const Vec4f &source, const Vec4f &destination) {
    Vec4f result = source;

    for (uint32_t channel = 0; channel < 3; channel++) {
        if (blend.color_func == SCE_GXM_BLEND_FUNC_NONE)
            break;

        const float source_factor = blend_factor(blend.color_src, source, destination, channel);
        const float destination_factor = blend_factor(blend.color_dst, source, destination, channel);

        // The min and max functions ignore the factors entirely.
        const bool factorless = (blend.color_func == SCE_GXM_BLEND_FUNC_MIN)
            || (blend.color_func == SCE_GXM_BLEND_FUNC_MAX);
        result[channel] = factorless
            ? blend_channel(blend.color_func, source[channel], destination[channel])
            : blend_channel(blend.color_func, source[channel] * source_factor,
                  destination[channel] * destination_factor);
    }

    if (blend.alpha_func != SCE_GXM_BLEND_FUNC_NONE) {
        const bool factorless = (blend.alpha_func == SCE_GXM_BLEND_FUNC_MIN)
            || (blend.alpha_func == SCE_GXM_BLEND_FUNC_MAX);
        result[3] = factorless
            ? blend_channel(blend.alpha_func, source[3], destination[3])
            : blend_channel(blend.alpha_func,
                  source[3] * blend_factor(blend.alpha_src, source, destination, 3),
                  destination[3] * blend_factor(blend.alpha_dst, source, destination, 3));
    }

    return result;
}

// Signed area of the triangle in screen space, doubled. With Y growing
// downwards, positive means counter-clockwise on screen: the sign vulkan's
// framebuffer-space area takes, and GXM front faces are counter-clockwise.
float edge_function(const Vec4f &a, const Vec4f &b, const Vec4f &c) {
    return (c[0] - a[0]) * (b[1] - a[1]) - (c[1] - a[1]) * (b[0] - a[0]);
}

// One edge of a triangle, anchored at whichever end sorts first so the two
// triangles sharing it compute exactly opposite values: no pixel falls between
// them, and the top-left rule hands a pixel right on the edge to only one.
struct Edge {
    float x0 = 0.0f;
    float y0 = 0.0f;
    float dx = 0.0f;
    float dy = 0.0f;
    bool negate = false;
    bool top_left = false;

    Edge() = default;

    Edge(const Vec4f &a, const Vec4f &b, bool positive_area) {
        const bool swap = (b[1] < a[1]) || (b[1] == a[1] && b[0] < a[0]);
        const Vec4f &first = swap ? b : a;
        const Vec4f &second = swap ? a : b;
        x0 = first[0];
        y0 = first[1];
        dx = second[0] - first[0];
        dy = second[1] - first[1];
        negate = swap;

        // Walked so the inside is positive, a left edge goes down the screen and a top edge goes left.
        const float walk_x = positive_area ? (b[0] - a[0]) : (a[0] - b[0]);
        const float walk_y = positive_area ? (b[1] - a[1]) : (a[1] - b[1]);
        top_left = walk_y > 0.0f || (walk_y == 0.0f && walk_x < 0.0f);
    }

    float row_term(float py) const {
        return (py - y0) * dx;
    }

    // edge_function(a, b, pixel), given row_term(pixel.y).
    float at(float px, float row) const {
        const float value = (px - x0) * dy - row;
        return negate ? -value : value;
    }
};

// Unorm 8 to float, exact, without a division per channel.
const std::array<float, 256> unorm8_to_float = [] {
    std::array<float, 256> table{};
    for (uint32_t i = 0; i < table.size(); i++)
        table[i] = static_cast<float>(i) / 255.0f;
    return table;
}();

Vec4f read_destination_pixel(const ColorFormatInfo &format, const uint8_t *pixel) {
    if (format.base_format == SCE_GXM_COLOR_BASE_FORMAT_U8U8U8U8) {
        Vec4f color;
        for (uint32_t i = 0; i < 4; i++)
            color[format.swizzle[i]] = unorm8_to_float[pixel[i]];
        return color;
    }
    return read_color_pixel(format, pixel);
}

} // namespace

VaryingLayout build_varying_layout(const SpirvModule &fragment_module) {
    VaryingLayout layout;

    for (const uint32_t id : fragment_module.input_variables) {
        const auto &variable = fragment_module.variables.at(id);
        if (variable.location == SPIRV_NO_VALUE)
            continue;

        VaryingSlot slot;
        slot.location = variable.location;
        slot.float_count = std::max(1u, variable.byte_size / 4u);
        slot.offset = layout.total_floats;
        layout.total_floats += slot.float_count;
        layout.slots.push_back(slot);
    }

    std::sort(layout.slots.begin(), layout.slots.end(),
        [](const VaryingSlot &a, const VaryingSlot &b) { return a.location < b.location; });

    // Sorting shuffled the offsets, so lay them out again in location order.
    uint32_t offset = 0;
    for (auto &slot : layout.slots) {
        slot.offset = offset;
        offset += slot.float_count;
    }
    layout.total_floats = offset;

    return layout;
}

Rasterizer::Rasterizer() {
    const uint32_t concurrency = std::max(1u, std::thread::hardware_concurrency());
    // The calling thread takes a share of the rows as well, so it needs an
    // interpreter of its own on top of the worker ones.
    const uint32_t worker_count = concurrency - 1;

    m_interpreters.reserve(worker_count + 1);
    for (uint32_t i = 0; i <= worker_count; i++)
        m_interpreters.push_back(std::make_unique<SpirvInterpreter>());

    m_threads.reserve(worker_count);
    for (uint32_t i = 0; i < worker_count; i++)
        m_threads.emplace_back([this, i] { worker_loop(i); });
}

Rasterizer::~Rasterizer() {
    {
        std::lock_guard<std::mutex> guard(m_mutex);
        m_shutdown = true;
    }
    m_work_ready.notify_all();

    for (auto &thread : m_threads) {
        if (thread.joinable())
            thread.join();
    }
}

void Rasterizer::worker_loop(uint32_t index) {
    uint32_t seen_generation = 0;

    while (true) {
        const DrawCall *call = nullptr;
        {
            std::unique_lock<std::mutex> guard(m_mutex);
            m_work_ready.wait(guard, [this, seen_generation] {
                return m_shutdown || m_generation != seen_generation;
            });

            if (m_shutdown)
                return;

            seen_generation = m_generation;
            call = m_current;
        }

        if (call)
            rasterize_rows(*call, *m_interpreters[index], index + 1,
                static_cast<uint32_t>(m_threads.size()) + 1);

        {
            std::lock_guard<std::mutex> guard(m_mutex);
            if (--m_pending == 0)
                m_work_done.notify_one();
        }
    }
}

void Rasterizer::run(const DrawCall &call) {
    const uint32_t worker_count = static_cast<uint32_t>(m_threads.size());

    if (worker_count > 0) {
        {
            std::lock_guard<std::mutex> guard(m_mutex);
            m_current = &call;
            m_pending = worker_count;
            m_generation++;
        }
        m_work_ready.notify_all();
    }

    // The caller owns row 0 of every group, so it is never left idle waiting.
    rasterize_rows(call, *m_interpreters[worker_count], 0, worker_count + 1);

    if (worker_count > 0) {
        std::unique_lock<std::mutex> guard(m_mutex);
        m_work_done.wait(guard, [this] { return m_pending == 0; });
        m_current = nullptr;
    }
}

void Rasterizer::rasterize_rows(const DrawCall &call, SpirvInterpreter &interpreter, uint32_t row_offset,
    uint32_t row_step) {
    if (!call.fragment_module || !call.color.valid() || call.primitive_count == 0)
        return;

    // What each lane of a batch carries from coverage to the output merger.
    struct Fragment {
        int32_t x = 0;
        int32_t y = 0;
        float depth = 0.0f;
        float inverse_w = 0.0f;
        float perspective[3] = {};
        size_t pixel_index = 0;
        bool depth_passed = true;
        bool test_stencil = false;
        uint8_t *destination = nullptr;
    };
    std::array<Fragment, SPIRV_MAX_LANES> batch;
    // Read before the shader runs: it may want it for programmable blending,
    // and the blend stage needs it too.
    std::array<Vec4f, SPIRV_MAX_LANES> destination_colors{};
    uint32_t batch_count = 0;

    // Each worker gets its own copy of the bindings so that the sampler callback
    // can close over per-lane state without any sharing.
    SpirvBindings bindings = *call.fragment_bindings;

    const SWTextureCache *textures = call.textures;
    bindings.sample_texture = [&destination_colors, textures](uint32_t descriptor_set, uint32_t binding,
                                  uint32_t lane_mask, const float *coords, uint32_t coord_count, const float *lods,
                                  bool has_lod, float *out_rgba) {
        const SWSampler *sampler = (descriptor_set != 1 && textures) ? textures->bound_sampler(false, binding) : nullptr;

        for (uint32_t lane = 0; lane < SPIRV_MAX_LANES; lane++) {
            if (!(lane_mask & (1u << lane)))
                continue;
            float *out = out_rgba + lane * 4;

            // Descriptor set 1 is the render target itself, which the recompiler
            // reads for programmable blending. The CPU path has it right here.
            if (descriptor_set == 1) {
                for (uint32_t i = 0; i < 4; i++)
                    out[i] = destination_colors[lane][i];
                continue;
            }

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

    interpreter.set_program(call.fragment_module, &bindings);

    // Resolve the storage the invocation reads and writes once per draw; the
    // interpreter keeps these pointers stable until the next set_program.
    std::vector<SpirvLaneStorage> input_storage(call.varyings.slots.size());
    for (size_t i = 0; i < call.varyings.slots.size(); i++)
        input_storage[i] = interpreter.input_by_location(call.varyings.slots[i].location);

    const SpirvLaneStorage frag_coord = interpreter.builtin_storage(spv::BuiltInFragCoord);
    const SpirvLaneStorage front_facing = interpreter.builtin_storage(spv::BuiltInFrontFacing);
    const SpirvLaneStorage color_output = interpreter.output_by_location(0);

    const int32_t surface_max_x = std::min<int32_t>(call.clip_max_x, static_cast<int32_t>(call.color.width) - 1);
    const int32_t surface_max_y = std::min<int32_t>(call.clip_max_y, static_cast<int32_t>(call.color.height) - 1);
    const int32_t surface_min_x = std::max<int32_t>(call.clip_min_x, 0);
    const int32_t surface_min_y = std::max<int32_t>(call.clip_min_y, 0);

    const uint32_t vertices_per_primitive = (call.primitive_type == SCE_GXM_PRIMITIVE_LINES) ? 2
        : (call.primitive_type == SCE_GXM_PRIMITIVE_POINTS)                                  ? 1
                                                                                             : 3;

    std::vector<float> interpolated(call.varyings.total_floats, 0.0f);

    // Summed here and published once: a shared counter bumped per pixel would bounce between every worker.
    uint32_t visible_pixels = 0;

    const bool full_color_mask = call.blend.color_mask[0] && call.blend.color_mask[1] && call.blend.color_mask[2]
        && call.blend.color_mask[3];

    for (uint32_t primitive = 0; primitive < call.primitive_count; primitive++) {
        const uint32_t *indices = call.primitive_indices + static_cast<size_t>(primitive) * vertices_per_primitive;

        // Points and lines are drawn as the degenerate triangles of their
        // footprint, which keeps a single fragment path for every primitive.
        const ShadedVertex &v0 = call.vertices[indices[0]];
        const ShadedVertex &v1 = call.vertices[indices[vertices_per_primitive > 1 ? 1 : 0]];
        const ShadedVertex &v2 = call.vertices[indices[vertices_per_primitive > 2 ? 2 : 0]];

        const float area = edge_function(v0.position, v1.position, v2.position);

        int32_t min_x, max_x, min_y, max_y;
        bool is_wide_primitive = false;

        if (vertices_per_primitive == 3) {
            if (area == 0.0f)
                continue;

            const bool counter_clockwise = area > 0.0f;
            if ((call.cull_mode == SCE_GXM_CULL_CW && !counter_clockwise)
                || (call.cull_mode == SCE_GXM_CULL_CCW && counter_clockwise))
                continue;

            min_x = static_cast<int32_t>(std::floor(std::min({ v0.position[0], v1.position[0], v2.position[0] })));
            max_x = static_cast<int32_t>(std::ceil(std::max({ v0.position[0], v1.position[0], v2.position[0] })));
            min_y = static_cast<int32_t>(std::floor(std::min({ v0.position[1], v1.position[1], v2.position[1] })));
            max_y = static_cast<int32_t>(std::ceil(std::max({ v0.position[1], v1.position[1], v2.position[1] })));
        } else {
            // A line or a point covers a band around its footprint; the fragment
            // coverage test below falls back to a distance check for these.
            const float half_width = std::max(call.line_width, v0.point_size) * 0.5f + 0.5f;
            min_x = static_cast<int32_t>(std::floor(std::min(v0.position[0], v1.position[0]) - half_width));
            max_x = static_cast<int32_t>(std::ceil(std::max(v0.position[0], v1.position[0]) + half_width));
            min_y = static_cast<int32_t>(std::floor(std::min(v0.position[1], v1.position[1]) - half_width));
            max_y = static_cast<int32_t>(std::ceil(std::max(v0.position[1], v1.position[1]) + half_width));
            is_wide_primitive = true;
        }

        min_x = std::max(min_x, surface_min_x);
        max_x = std::min(max_x, surface_max_x);
        min_y = std::max(min_y, surface_min_y);
        max_y = std::min(max_y, surface_max_y);

        if (min_x > max_x || min_y > max_y)
            continue;

        // Lines and points have no area, and like on any GPU they count as front facing.
        const bool front_face = is_wide_primitive || area > 0.0f;
        const SideState &side = (call.two_sided && !front_face) ? call.back : call.front;

        Edge edges[3];
        if (!is_wide_primitive) {
            edges[0] = Edge(v1.position, v2.position, area > 0.0f);
            edges[1] = Edge(v2.position, v0.position, area > 0.0f);
            edges[2] = Edge(v0.position, v1.position, area > 0.0f);
        }
        const float inverse_area = is_wide_primitive ? 0.0f : 1.0f / area;
        const float inside_sign = (area > 0.0f) ? 1.0f : -1.0f;

        // Only blending, a partial color mask or a shader reading it back needs the old pixel.
        const bool read_destination = side.fragment_program_enabled && !call.is_maskupdate
            && (call.blend.enabled || !full_color_mask || call.fragment_module->reads_destination);

        const bool has_depth = call.depth_enabled && call.depth_stencil && call.depth_stencil->has_depth;
        const bool has_stencil = call.stencil_enabled && call.depth_stencil && call.depth_stencil->has_stencil;

        const float *varyings0 = call.varying_storage + static_cast<size_t>(v0.varying_index) * call.varyings.total_floats;
        const float *varyings1 = call.varying_storage + static_cast<size_t>(v1.varying_index) * call.varyings.total_floats;
        const float *varyings2 = call.varying_storage + static_cast<size_t>(v2.varying_index) * call.varyings.total_floats;

        // Shades the collected lanes together, then runs the per-pixel tests
        // and the output merger on each of them in turn.
        const auto flush = [&]() {
            if (batch_count == 0)
                return;

            const uint32_t mask = (1u << batch_count) - 1u;
            uint32_t survivors = mask;

            if (side.fragment_program_enabled) {
                for (uint32_t lane = 0; lane < batch_count; lane++) {
                    const Fragment &fragment = batch[lane];

                    for (uint32_t i = 0; i < call.varyings.total_floats; i++)
                        interpolated[i] = varyings0[i] * fragment.perspective[0] + varyings1[i] * fragment.perspective[1]
                            + varyings2[i] * fragment.perspective[2];

                    for (size_t i = 0; i < call.varyings.slots.size(); i++) {
                        if (!input_storage[i])
                            continue;
                        std::memcpy(input_storage[i].lane(lane), &interpolated[call.varyings.slots[i].offset],
                            static_cast<size_t>(call.varyings.slots[i].float_count) * sizeof(float));
                    }

                    if (frag_coord) {
                        const float coordinates[4] = { static_cast<float>(fragment.x) + 0.5f,
                            static_cast<float>(fragment.y) + 0.5f, fragment.depth, fragment.inverse_w };
                        std::memcpy(frag_coord.lane(lane), coordinates, sizeof(coordinates));
                    }
                    if (front_facing) {
                        const uint32_t value = front_face ? 1u : 0u;
                        std::memcpy(front_facing.lane(lane), &value, sizeof(value));
                    }
                }

                survivors = interpreter.execute(mask);
            }

            for (uint32_t lane = 0; lane < batch_count; lane++) {
                // A discarded fragment leaves depth, stencil and color alone.
                if (!(survivors & (1u << lane)))
                    continue;

                const Fragment &fragment = batch[lane];
                const Vec4f &destination_color = destination_colors[lane];

                // Stencil comes before depth, and its ops fire even when the
                // depth test is what rejected the fragment.
                bool stencil_passed = true;
                if (fragment.test_stencil) {
                    const uint8_t current = call.depth_stencil->stencil[fragment.pixel_index];
                    stencil_passed = stencil_test_passes(side.stencil_op.func, side.stencil_values.ref,
                        current, side.stencil_values.compare_mask);

                    const SceGxmStencilOp op = !stencil_passed ? side.stencil_op.stencil_fail
                                                               : (fragment.depth_passed ? side.stencil_op.depth_pass : side.stencil_op.depth_fail);

                    const uint8_t updated = apply_stencil_op(op, current, side.stencil_values.ref);
                    call.depth_stencil->stencil[fragment.pixel_index] = static_cast<uint8_t>(
                        (current & ~side.stencil_values.write_mask)
                        | (updated & side.stencil_values.write_mask));
                }

                if (!stencil_passed || !fragment.depth_passed)
                    continue;

                if (has_depth && side.depth_write && fragment.pixel_index < call.depth_stencil->depth.size())
                    call.depth_stencil->depth[fragment.pixel_index] = fragment.depth;

                visible_pixels++;

                // With the fragment program disabled the draw only exists to
                // update depth and stencil.
                if (call.is_maskupdate || !side.fragment_program_enabled)
                    continue;

                Vec4f source_color = { 0.0f, 0.0f, 0.0f, 1.0f };
                if (color_output)
                    std::memcpy(source_color.data(), color_output.lane(lane), sizeof(float) * 4);

                Vec4f final_color = call.blend.enabled
                    ? apply_blend(call.blend, source_color, destination_color)
                    : source_color;

                for (uint32_t channel = 0; channel < 4; channel++) {
                    if (!call.blend.color_mask[channel])
                        final_color[channel] = destination_color[channel];
                }

                if (call.color.gamma_corrected) {
                    for (uint32_t channel = 0; channel < 3; channel++)
                        final_color[channel] = linear_to_srgb(clamp01(final_color[channel]));
                }

                write_color_pixel(call.color.format, fragment.destination, final_color);
            }

            batch_count = 0;
        };

        // Align the first row this worker owns with its share of the grid.
        int32_t start_y = min_y;
        const int32_t remainder = static_cast<int32_t>((static_cast<uint32_t>(start_y) % row_step));
        if (remainder != static_cast<int32_t>(row_offset))
            start_y += static_cast<int32_t>((row_offset + row_step - remainder) % row_step);

        for (int32_t y = start_y; y <= max_y; y += static_cast<int32_t>(row_step)) {
            const float py = static_cast<float>(y) + 0.5f;
            const float rows[3] = { edges[0].row_term(py), edges[1].row_term(py), edges[2].row_term(py) };

            for (int32_t x = min_x; x <= max_x; x++) {
                const float px = static_cast<float>(x) + 0.5f;

                float weights[3] = { 1.0f, 0.0f, 0.0f };

                if (!is_wide_primitive) {
                    const float w0 = edges[0].at(px, rows[0]);
                    const float w1 = edges[1].at(px, rows[1]);
                    const float w2 = edges[2].at(px, rows[2]);

                    // Inside every edge in the triangle's own winding, and on an
                    // edge only where that edge is a top or left one.
                    const float s0 = w0 * inside_sign;
                    const float s1 = w1 * inside_sign;
                    const float s2 = w2 * inside_sign;
                    if (!(s0 > 0.0f || (s0 == 0.0f && edges[0].top_left)) || !(s1 > 0.0f || (s1 == 0.0f && edges[1].top_left))
                        || !(s2 > 0.0f || (s2 == 0.0f && edges[2].top_left)))
                        continue;

                    weights[0] = w0 * inverse_area;
                    weights[1] = w1 * inverse_area;
                    weights[2] = w2 * inverse_area;
                } else {
                    const float dx = v1.position[0] - v0.position[0];
                    const float dy = v1.position[1] - v0.position[1];
                    const float length_squared = dx * dx + dy * dy;

                    float t = 0.0f;
                    if (length_squared > 0.0f)
                        t = std::clamp(((px - v0.position[0]) * dx + (py - v0.position[1]) * dy) / length_squared,
                            0.0f, 1.0f);

                    const float nearest_x = v0.position[0] + dx * t;
                    const float nearest_y = v0.position[1] + dy * t;
                    const float distance_x = px - nearest_x;
                    const float distance_y = py - nearest_y;
                    const float half_width = std::max(call.line_width, v0.point_size) * 0.5f;

                    if (distance_x * distance_x + distance_y * distance_y > half_width * half_width + 0.25f)
                        continue;

                    weights[0] = 1.0f - t;
                    weights[1] = t;
                    weights[2] = 0.0f;
                }

                const float inverse_w = weights[0] * v0.position[3] + weights[1] * v1.position[3]
                    + weights[2] * v2.position[3];
                float depth = weights[0] * v0.position[2] + weights[1] * v1.position[2]
                    + weights[2] * v2.position[2];
                depth += call.depth_bias_units;
                depth = std::clamp(depth, 0.0f, 1.0f);

                const size_t pixel_index = static_cast<size_t>(y) * call.color.width + x;
                const bool test_depth = has_depth && pixel_index < call.depth_stencil->depth.size();
                const bool test_stencil = has_stencil && pixel_index < call.depth_stencil->stencil.size();

                bool depth_passed = true;
                if (test_depth)
                    depth_passed = depth_test_passes(side.depth_func, depth, call.depth_stencil->depth[pixel_index]);

                // Depth can be settled before the shader runs whenever the
                // shader has no way to throw the fragment away.
                if (!call.fragment_module->may_discard && test_depth && !depth_passed && !test_stencil)
                    continue;

                Fragment &fragment = batch[batch_count];
                fragment.x = x;
                fragment.y = y;
                fragment.depth = depth;
                fragment.inverse_w = inverse_w;
                fragment.pixel_index = pixel_index;
                fragment.depth_passed = depth_passed;
                fragment.test_stencil = test_stencil;

                // Perspective correct weights, needed for both the varyings and
                // anything the shader derives from them.
                fragment.perspective[0] = weights[0];
                fragment.perspective[1] = weights[1];
                fragment.perspective[2] = weights[2];
                if (inverse_w != 0.0f) {
                    const float w = 1.0f / inverse_w;
                    fragment.perspective[0] = weights[0] * v0.position[3] * w;
                    fragment.perspective[1] = weights[1] * v1.position[3] * w;
                    fragment.perspective[2] = weights[2] * v2.position[3] * w;
                }

                fragment.destination = call.color.pixel(static_cast<uint32_t>(x), static_cast<uint32_t>(y));
                if (read_destination)
                    destination_colors[batch_count] = read_destination_pixel(call.color.format, fragment.destination);

                if (++batch_count == SPIRV_MAX_LANES)
                    flush();
            }
        }

        flush();
    }

    if (call.visible_counter && visible_pixels)
        call.visible_counter->fetch_add(visible_pixels, std::memory_order_relaxed);
}

} // namespace renderer::software
