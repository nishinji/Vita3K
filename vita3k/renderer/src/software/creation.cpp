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
#include <renderer/software/state.h>
#include <renderer/software/types.h>

#include <config/state.h>
#include <glutil/gl.h>
#include <gxm/functions.h>
#include <renderer/frame_host.h>
#include <shader/spirv_recompiler.h>
#include <util/log.h>

namespace renderer::software {

bool create(std::unique_ptr<renderer::State> &state, const Config &config) {
    auto &sw_state = dynamic_cast<SWState &>(*state);

    // Presentation goes through the OpenGL screen renderer, so the GL entry
    // points have to be resolved before anything touches them. Nothing else in
    // this backend uses OpenGL, and no extension is probed: the feature flags
    // below describe what the rasterizer does, not what the driver offers.
    static renderer::FrameHost *s_frame = nullptr;
    s_frame = sw_state.frame;
#ifdef __ANDROID__
    gladLoadGLES2Loader([](const char *name) -> void * {
#else
    gladLoadGLLoader([](const char *name) -> void * {
#endif
        return s_frame->get_proc_address(name);
    });

    const char *gl_version = reinterpret_cast<const char *>(glGetString(GL_VERSION));
    LOG_INFO("Software renderer presenting through GL_VERSION = {}", gl_version ? gl_version : "Unknown");

    // Every one of these is a statement about what the rasterizer itself can do,
    // not about a driver. The CPU path has the destination pixel in hand while a
    // fragment runs, so it can serve gl_LastFragData directly, which is the
    // fastest of the programmable blending paths the recompiler knows about.
    sw_state.features.direct_fragcolor = true;
    sw_state.features.support_shader_interlock = false;
    sw_state.features.support_texture_barrier = false;
    sw_state.features.support_unknown_format = true;
    sw_state.features.support_rgb_attributes = true;
    sw_state.features.support_scaled_attribute_formats = true;
    sw_state.features.support_get_texture_sub_image = false;
    sw_state.features.use_mask_bit = false;
    sw_state.features.enable_memory_mapping = false;
    sw_state.features.use_texture_viewport = false;

    // Upscaling a CPU rasterizer costs the square of the multiplier for no
    // benefit a user of this backend is after, so it is pinned to native.
    sw_state.res_multiplier = 1.0f;

    return sw_state.init();
}

bool create(SWState &state, std::unique_ptr<Context> &context, MemState &mem) {
    context = std::make_unique<SWContext>();
    return true;
}

bool create(SWState &state, std::unique_ptr<RenderTarget> &rt, const SceGxmRenderTargetParams &params,
    const FeatureState &features) {
    rt = std::make_unique<SWRenderTarget>();

    auto *render_target = static_cast<SWRenderTarget *>(rt.get());
    render_target->width = params.width;
    render_target->height = params.height;

    return true;
}

void destroy(SWState &state, std::unique_ptr<RenderTarget> &rt) {
    rt.reset();
}

namespace {

SWBlendState translate_blend(const SceGxmBlendInfo *blend) {
    SWBlendState result;
    if (!blend)
        return result;

    result.color_mask[0] = (blend->colorMask & SCE_GXM_COLOR_MASK_R) != 0;
    result.color_mask[1] = (blend->colorMask & SCE_GXM_COLOR_MASK_G) != 0;
    result.color_mask[2] = (blend->colorMask & SCE_GXM_COLOR_MASK_B) != 0;
    result.color_mask[3] = (blend->colorMask & SCE_GXM_COLOR_MASK_A) != 0;

    result.enabled = (blend->colorFunc != SCE_GXM_BLEND_FUNC_NONE)
        || (blend->alphaFunc != SCE_GXM_BLEND_FUNC_NONE);

    result.color_func = blend->colorFunc;
    result.alpha_func = blend->alphaFunc;
    result.color_src = blend->colorSrc;
    result.color_dst = blend->colorDst;
    result.alpha_src = blend->alphaSrc;
    result.alpha_dst = blend->alphaDst;

    return result;
}

} // namespace

void create(std::unique_ptr<FragmentProgram> &fp, SWState &state, const SceGxmProgram &program,
    const SceGxmBlendInfo *blend) {
    auto *fragment_program = new SWFragmentProgram;
    fp = std::unique_ptr<FragmentProgram>(fragment_program);

    fragment_program->program = &program;
    fragment_program->blend = translate_blend(blend);
}

void create(std::unique_ptr<VertexProgram> &vp, SWState &state, const SceGxmProgram &program) {
    auto *vertex_program = new SWVertexProgram;
    vp = std::unique_ptr<VertexProgram>(vertex_program);

    vertex_program->program = &program;
}

SWShaderPtr SWShaderCache::get(const SWShaderKey &key, const SceGxmProgram &program,
    const FeatureState &features, const shader::Hints &hints, bool maskupdate,
    const std::string &shader_hash) {
    {
        std::lock_guard<std::mutex> guard(m_mutex);
        const auto it = m_cache.find(key);
        if (it != m_cache.end())
            return it->second;
    }

    // Recompiling outside the lock keeps a slow translation from stalling the
    // other worker threads; the worst case is that two of them translate the
    // same program once and one result is thrown away.
    auto shader = std::make_shared<SWShader>();

    const shader::GeneratedShader generated = shader::convert_gxp(program, shader_hash, features,
        shader::Target::SpirVVulkan, hints, maskupdate);

    if (generated.spirv.empty()) {
        LOG_ERROR("Software renderer: the recompiler produced no SPIR-V for shader {}", shader_hash);
    } else {
        shader->valid = shader->module.parse(generated.spirv);
        if (!shader->valid)
            LOG_ERROR("Software renderer: failed to parse the SPIR-V of shader {}", shader_hash);
    }

    std::lock_guard<std::mutex> guard(m_mutex);
    const auto [it, inserted] = m_cache.try_emplace(key, std::move(shader));
    return it->second;
}

size_t SWShaderCache::size() const {
    std::lock_guard<std::mutex> guard(m_mutex);
    return m_cache.size();
}

void SWShaderCache::clear() {
    std::lock_guard<std::mutex> guard(m_mutex);
    m_cache.clear();
}

} // namespace renderer::software
