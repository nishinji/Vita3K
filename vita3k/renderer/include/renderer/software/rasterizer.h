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

#include <renderer/software/spirv_interp.h>
#include <renderer/software/surface_cache.h>
#include <renderer/software/types.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

namespace renderer::software {

// Where one varying of the fragment shader lives inside the interpolated block.
struct VaryingSlot {
    uint32_t location = 0;
    uint32_t offset = 0;
    uint32_t float_count = 0;
};

// The varyings a draw carries from the vertex stage to the fragment stage. It is
// derived from the fragment inputs, since a vertex output nothing reads never
// needs to be interpolated.
struct VaryingLayout {
    std::vector<VaryingSlot> slots;
    uint32_t total_floats = 0;
};

// A vertex the draw path has already taken through the vertex shader, the
// perspective divide and the viewport transform: x and y are surface pixels, z
// is the depth to test against and w holds 1/w of the clip position, which is
// what the perspective correct interpolation needs.
struct ShadedVertex {
    Vec4f position = { 0.0f, 0.0f, 0.0f, 1.0f };
    // The position the vertex shader wrote, which clipping works on.
    Vec4f clip = { 0.0f, 0.0f, 0.0f, 1.0f };
    float point_size = 1.0f;
    // Index of this vertex inside the draw varying storage.
    uint32_t varying_index = 0;
};

// Per-side state, since GXM can drive front and back faces independently.
struct SideState {
    SceGxmDepthFunc depth_func = SCE_GXM_DEPTH_FUNC_LESS_EQUAL;
    bool depth_write = true;
    GxmStencilStateOp stencil_op;
    GxmStencilStateValues stencil_values;
    bool fragment_program_enabled = true;
    SceGxmPolygonMode polygon_mode = SCE_GXM_POLYGON_MODE_TRIANGLE_FILL;
};

// Everything one draw needs, gathered up front so the worker threads never have
// to reach back into the command stream or the renderer state.
class SWTextureCache;

struct DrawCall {
    const SpirvModule *fragment_module = nullptr;
    // Template the workers copy: each one installs its own sampler callback, so
    // that reading the destination pixel stays private to the invocation.
    const SpirvBindings *fragment_bindings = nullptr;
    const SWTextureCache *textures = nullptr;

    SWColorTarget color;
    SWDepthStencilTarget *depth_stencil = nullptr;

    VaryingLayout varyings;
    const float *varying_storage = nullptr;

    const ShadedVertex *vertices = nullptr;
    // Vertex indices making up the primitives, three per triangle, two per line
    // and one per point.
    const uint32_t *primitive_indices = nullptr;
    uint32_t primitive_count = 0;
    SceGxmPrimitiveType primitive_type = SCE_GXM_PRIMITIVE_TRIANGLES;

    SideState front;
    SideState back;
    SceGxmCullMode cull_mode = SCE_GXM_CULL_NONE;
    bool two_sided = false;

    SWBlendState blend;

    // Scissor rectangle in surface pixels, inclusive on both ends.
    int32_t clip_min_x = 0;
    int32_t clip_min_y = 0;
    int32_t clip_max_x = 0;
    int32_t clip_max_y = 0;
    // GXM can also clip everything outside or inside the region.
    SceGxmRegionClipMode region_clip_mode = SCE_GXM_REGION_CLIP_ALL;

    float depth_bias_units = 0.0f;
    float depth_bias_slope = 0.0f;
    float line_width = 1.0f;

    bool depth_enabled = false;
    bool stencil_enabled = false;
    // Set for the mask-update pass, which writes no color at all.
    bool is_maskupdate = false;

    // Number of pixels the draw wrote, used to answer visibility queries.
    std::atomic<uint32_t> *visible_counter = nullptr;
};

// Runs fragment work across a pool of worker threads. Each worker owns its own
// interpreter, so a shader invocation never shares mutable state with another.
class Rasterizer {
public:
    Rasterizer();
    ~Rasterizer();

    void run(const DrawCall &call);

private:
    void worker_loop(uint32_t index);
    // Each worker owns the rows congruent to its index, so two of them never
    // touch the same pixel and no locking is needed around the surfaces.
    void rasterize_rows(const DrawCall &call, SpirvInterpreter &interpreter, uint32_t row_offset,
        uint32_t row_step);

    std::vector<std::thread> m_threads;
    std::vector<std::unique_ptr<SpirvInterpreter>> m_interpreters;

    std::mutex m_mutex;
    std::condition_variable m_work_ready;
    std::condition_variable m_work_done;
    const DrawCall *m_current = nullptr;
    uint32_t m_generation = 0;
    uint32_t m_pending = 0;
    bool m_shutdown = false;
};

// Builds the varying layout a fragment module expects.
VaryingLayout build_varying_layout(const SpirvModule &fragment_module);

} // namespace renderer::software
