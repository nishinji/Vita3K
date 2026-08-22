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
#include <renderer/types.h>
#include <shader/uniform_block.h>
#include <util/hash.h>

#include <map>
#include <memory>
#include <mutex>
#include <vector>

namespace renderer::software {

// A shader that has been recompiled to SPIR-V and parsed for interpretation.
// Programs are keyed by the hash of the GXM program plus the parts of the render
// state the recompiler bakes in, exactly like the pipeline caches of the GPU
// backends do.
struct SWShader {
    SpirvModule module;
    bool valid = false;
};

using SWShaderPtr = std::shared_ptr<SWShader>;

struct SWBlendState {
    bool enabled = false;
    SceGxmBlendFunc color_func = SCE_GXM_BLEND_FUNC_ADD;
    SceGxmBlendFunc alpha_func = SCE_GXM_BLEND_FUNC_ADD;
    SceGxmBlendFactor color_src = SCE_GXM_BLEND_FACTOR_ONE;
    SceGxmBlendFactor color_dst = SCE_GXM_BLEND_FACTOR_ZERO;
    SceGxmBlendFactor alpha_src = SCE_GXM_BLEND_FACTOR_ONE;
    SceGxmBlendFactor alpha_dst = SCE_GXM_BLEND_FACTOR_ZERO;
    // One flag per RGBA channel, in that order.
    std::array<bool, 4> color_mask = { true, true, true, true };
};

struct SWFragmentProgram : public renderer::FragmentProgram {
    const SceGxmProgram *program = nullptr;
    SWBlendState blend;
};

struct SWVertexProgram : public renderer::VertexProgram {
    const SceGxmProgram *program = nullptr;
};

struct SWRenderTarget : public renderer::RenderTarget {
    uint32_t width = 0;
    uint32_t height = 0;

    ~SWRenderTarget() override = default;
};

struct SWContext : public renderer::Context {
    const SWRenderTarget *render_target = nullptr;

    SWColorTarget color_target;
    SWDepthStencilTarget *depth_stencil_target = nullptr;

    // Uniform storage the draw path fills before every draw, mirroring the
    // layout the recompiler expects at descriptor set 0.
    std::vector<uint8_t> vertex_uniform_storage;
    std::vector<uint8_t> fragment_uniform_storage;

    shader::RenderVertUniformBlock current_vert_render_info{};
    shader::RenderFragUniformBlock current_frag_render_info{};

    // The vulkan-target shaders leave the viewport transform, and with it the Y flip, to the rasterizer.
    std::array<float, 2> viewport_offset{};
    std::array<float, 2> viewport_scale{};

    // Textures bound to the context. GXM numbers vertex texture units straight
    // after the fragment ones, so a single array covers both stages.
    std::array<SceGxmTexture, SCE_GXM_MAX_TEXTURE_UNITS * 2> textures{};

    // Visibility (occlusion) query state, applied when a draw writes pixels.
    Ptr<uint32_t> visibility_buffer;
    uint32_t visibility_stride = 0;
    uint32_t visibility_index = 0;
    bool visibility_enabled = false;
    bool visibility_increment = false;

    ~SWContext() override = default;
};

// Key of the shader cache: the GXM program hash plus everything the recompiler
// specialises on. Two draws sharing a key can share the parsed module.
struct SWShaderKey {
    Sha256Hash hash{};
    uint32_t color_format = 0;
    uint32_t hint_word = 0;
    bool is_maskupdate = false;

    bool operator<(const SWShaderKey &other) const {
        if (hash != other.hash)
            return hash < other.hash;
        if (color_format != other.color_format)
            return color_format < other.color_format;
        if (hint_word != other.hint_word)
            return hint_word < other.hint_word;
        return is_maskupdate < other.is_maskupdate;
    }
};

class SWShaderCache {
public:
    // Returns the parsed module for this program, recompiling it the first time
    // it is asked for. Safe to call from any worker thread.
    SWShaderPtr get(const SWShaderKey &key, const SceGxmProgram &program,
        const FeatureState &features, const shader::Hints &hints, bool maskupdate,
        const std::string &shader_hash);

    size_t size() const;
    void clear();

private:
    mutable std::mutex m_mutex;
    std::map<SWShaderKey, SWShaderPtr> m_cache;
};

} // namespace renderer::software
