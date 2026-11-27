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

#include <renderer/functions.h>
#include <renderer/profile.h>
#include <renderer/types.h>

#include <renderer/gl/functions.h>
#include <renderer/gl/state.h>
#include <renderer/gl/types.h>

#include <gxm/functions.h>
#include <gxm/types.h>
#include <util/align.h>
#include <util/log.h>
#include <util/vector_utils.h>

#include <shader/spirv_recompiler.h>

#include <bit>
#include <cmath>

namespace renderer::gl {

static GLenum translate_depth_func(SceGxmDepthFunc depth_func) {
    R_PROFILE(__func__);

    switch (depth_func) {
    case SCE_GXM_DEPTH_FUNC_NEVER:
        return GL_NEVER;
    case SCE_GXM_DEPTH_FUNC_LESS:
        return GL_LESS;
    case SCE_GXM_DEPTH_FUNC_EQUAL:
        return GL_EQUAL;
    case SCE_GXM_DEPTH_FUNC_LESS_EQUAL:
        return GL_LEQUAL;
    case SCE_GXM_DEPTH_FUNC_GREATER:
        return GL_GREATER;
    case SCE_GXM_DEPTH_FUNC_NOT_EQUAL:
        return GL_NOTEQUAL;
    case SCE_GXM_DEPTH_FUNC_GREATER_EQUAL:
        return GL_GEQUAL;
    case SCE_GXM_DEPTH_FUNC_ALWAYS:
        return GL_ALWAYS;
    }

    return GL_ALWAYS;
}

static GLenum translate_stencil_op(SceGxmStencilOp stencil_op) {
    R_PROFILE(__func__);

    switch (stencil_op) {
    case SCE_GXM_STENCIL_OP_KEEP:
        return GL_KEEP;
    case SCE_GXM_STENCIL_OP_ZERO:
        return GL_ZERO;
    case SCE_GXM_STENCIL_OP_REPLACE:
        return GL_REPLACE;
    case SCE_GXM_STENCIL_OP_INCR:
        return GL_INCR;
    case SCE_GXM_STENCIL_OP_DECR:
        return GL_DECR;
    case SCE_GXM_STENCIL_OP_INVERT:
        return GL_INVERT;
    case SCE_GXM_STENCIL_OP_INCR_WRAP:
        return GL_INCR_WRAP;
    case SCE_GXM_STENCIL_OP_DECR_WRAP:
        return GL_DECR_WRAP;
    }

    return GL_KEEP;
}

static GLenum translate_stencil_func(SceGxmStencilFunc stencil_func) {
    R_PROFILE(__func__);

    switch (stencil_func) {
    case SCE_GXM_STENCIL_FUNC_NEVER:
        return GL_NEVER;
    case SCE_GXM_STENCIL_FUNC_LESS:
        return GL_LESS;
    case SCE_GXM_STENCIL_FUNC_EQUAL:
        return GL_EQUAL;
    case SCE_GXM_STENCIL_FUNC_LESS_EQUAL:
        return GL_LEQUAL;
    case SCE_GXM_STENCIL_FUNC_GREATER:
        return GL_GREATER;
    case SCE_GXM_STENCIL_FUNC_NOT_EQUAL:
        return GL_NOTEQUAL;
    case SCE_GXM_STENCIL_FUNC_GREATER_EQUAL:
        return GL_GEQUAL;
    case SCE_GXM_STENCIL_FUNC_ALWAYS:
        return GL_ALWAYS;
    }

    return GL_ALWAYS;
}

void sync_mask(const GLState &state, GLContext &context, const MemState &mem) {
    GLubyte initial_byte = context.record.depth_stencil_surface.mask ? 0xFF : 0;
    if (context.render_target->mask_clear_value == initial_byte)
        return;
    context.render_target->mask_clear_value = initial_byte;

#ifndef __ANDROID__
    if (glClearTexImage) {
        GLubyte clear_bytes[4] = { initial_byte, initial_byte, initial_byte, initial_byte };
        glClearTexImage(context.render_target->masktexture[0], 0, GL_RGBA, GL_UNSIGNED_BYTE, clear_bytes);
    } else
#endif
    {
        auto width = context.render_target->width;
        auto height = context.render_target->height;
        std::vector<GLubyte> emptyData(width * height * 4, initial_byte);
        GLint texId;
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &texId);
        glBindTexture(GL_TEXTURE_2D, context.render_target->masktexture[0]);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, &emptyData[0]);
        glBindTexture(GL_TEXTURE_2D, texId);
    }
}

void sync_viewport_flat(const GLState &state, GLContext &context) {
    const GLsizei display_w = context.record.color_surface.width;
    const GLsizei display_h = context.record.color_surface.height;

    const GLint vx = 0;
    const GLint vy = static_cast<GLint>((context.current_framebuffer_height - display_h) * state.res_multiplier);
    const GLsizei vw = static_cast<GLsizei>(display_w * state.res_multiplier);
    const GLsizei vh = static_cast<GLsizei>(display_h * state.res_multiplier);

    auto &cache = context.state_cache;
    if (!cache.viewport_valid || cache.viewport[0] != vx || cache.viewport[1] != vy || cache.viewport[2] != vw || cache.viewport[3] != vh) {
        glViewport(vx, vy, vw, vh);
        cache.viewport[0] = vx;
        cache.viewport[1] = vy;
        cache.viewport[2] = vw;
        cache.viewport[3] = vh;
        cache.viewport_valid = true;
    }
    if (!cache.depth_range_valid) {
        glDepthRangef(0.0f, 1.0f);
        cache.depth_range_valid = true;
    }
}

void sync_viewport_real(const GLState &state, GLContext &context, const float xOffset, const float yOffset, const float zOffset,
    const float xScale, const float yScale, const float zScale) {
    const GLfloat ymin = yOffset + yScale;
    const GLfloat ymax = yOffset - yScale - 1;

    const GLfloat w = std::abs(2 * xScale);
    const GLfloat h = std::abs(2 * yScale);
    const GLfloat x = xOffset - std::abs(xScale);
    const GLfloat y = std::min<GLfloat>(ymin, ymax);

    const GLint vx = static_cast<GLint>(x * state.res_multiplier);
    const GLint vy = static_cast<GLint>(y * state.res_multiplier);
    const GLsizei vw = static_cast<GLsizei>(w * state.res_multiplier);
    const GLsizei vh = static_cast<GLsizei>(h * state.res_multiplier);

    auto &cache = context.state_cache;
    if (!cache.viewport_valid || cache.viewport[0] != vx || cache.viewport[1] != vy || cache.viewport[2] != vw || cache.viewport[3] != vh) {
        glViewport(vx, vy, vw, vh);
        cache.viewport[0] = vx;
        cache.viewport[1] = vy;
        cache.viewport[2] = vw;
        cache.viewport[3] = vh;
        cache.viewport_valid = true;
    }
    if (!cache.depth_range_valid) {
        glDepthRangef(0.0f, 1.0f);
        cache.depth_range_valid = true;
    }
}

void sync_clipping(const GLState &state, GLContext &context) {
    const GLsizei display_h = context.current_framebuffer_height;
    const GLsizei scissor_x = context.record.region_clip_min.x;
    GLsizei scissor_y = 0;

    if (context.record.viewport_flip[1] == -1.0f)
        scissor_y = context.record.region_clip_min.y;
    else
        scissor_y = display_h - context.record.region_clip_max.y - 1;

    const GLsizei scissor_w = context.record.region_clip_max.x - context.record.region_clip_min.x + 1;
    const GLsizei scissor_h = context.record.region_clip_max.y - context.record.region_clip_min.y + 1;

    auto &cache = context.state_cache;
    bool enable = false;
    GLint sx = 0, sy = 0;
    GLsizei sw = 0, sh = 0;

    switch (context.record.region_clip_mode) {
    case SCE_GXM_REGION_CLIP_NONE:
        enable = false;
        break;
    case SCE_GXM_REGION_CLIP_ALL:
        enable = true;
        break;
    case SCE_GXM_REGION_CLIP_OUTSIDE:
        enable = true;
        sx = static_cast<GLint>(scissor_x * state.res_multiplier);
        sy = static_cast<GLint>(scissor_y * state.res_multiplier);
        sw = static_cast<GLsizei>(scissor_w * state.res_multiplier);
        sh = static_cast<GLsizei>(scissor_h * state.res_multiplier);
        break;
    case SCE_GXM_REGION_CLIP_INSIDE:
        enable = false;
        LOG_WARN("Unimplemented region clip mode used: SCE_GXM_REGION_CLIP_INSIDE");
        break;
    }

    if (!cache.scissor_valid || cache.scissor_enabled != enable) {
        if (enable)
            glEnable(GL_SCISSOR_TEST);
        else
            glDisable(GL_SCISSOR_TEST);
        cache.scissor_enabled = enable;
    }
    if (enable && (!cache.scissor_valid || cache.scissor[0] != sx || cache.scissor[1] != sy || cache.scissor[2] != sw || cache.scissor[3] != sh)) {
        glScissor(sx, sy, sw, sh);
        cache.scissor[0] = sx;
        cache.scissor[1] = sy;
        cache.scissor[2] = sw;
        cache.scissor[3] = sh;
    }
    cache.scissor_valid = true;
}

void sync_cull(GLContext &context, const GxmRecordState &state) {
    auto &cache = context.state_cache;
    if (cache.cull_valid && cache.cull_mode == state.cull_mode)
        return;

    switch (state.cull_mode) {
    case SCE_GXM_CULL_CCW:
        if (!cache.cull_valid || cache.cull_mode == SCE_GXM_CULL_NONE)
            glEnable(GL_CULL_FACE);
        glCullFace(GL_BACK);
        break;
    case SCE_GXM_CULL_CW:
        if (!cache.cull_valid || cache.cull_mode == SCE_GXM_CULL_NONE)
            glEnable(GL_CULL_FACE);
        glCullFace(GL_FRONT);
        break;
    case SCE_GXM_CULL_NONE:
        glDisable(GL_CULL_FACE);
        break;
    }
    cache.cull_mode = state.cull_mode;
    cache.cull_valid = true;
}

void sync_depth_func(GLContext &context, const SceGxmDepthFunc func, const bool is_front) {
    if (!is_front)
        return;
    const GLenum gl_func = translate_depth_func(func);
    auto &cache = context.state_cache;
    if (cache.depth_func_valid && cache.depth_func == gl_func)
        return;
    glDepthFunc(gl_func);
    cache.depth_func = gl_func;
    cache.depth_func_valid = true;
}

void sync_depth_write_enable(GLContext &context, const SceGxmDepthWriteMode mode, const bool is_front) {
    if (!is_front)
        return;
    const GLboolean mask = mode == SCE_GXM_DEPTH_WRITE_ENABLED ? GL_TRUE : GL_FALSE;
    auto &cache = context.state_cache;
    if (cache.depth_mask_valid && cache.depth_mask == mask)
        return;
    glDepthMask(mask);
    cache.depth_mask = mask;
    cache.depth_mask_valid = true;
}

void sync_depth_data(const renderer::GxmRecordState &state) {
    // Depth test.
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);

    if (!state.depth_stencil_surface.force_load) {
        glClearDepthf(state.depth_stencil_surface.background_depth);
        glClear(GL_DEPTH_BUFFER_BIT);
    }
}

void sync_stencil_func(GLContext &context, const GxmRecordState &state, const bool is_back_stencil) {
    GLenum face;
    if (state.two_sided == SCE_GXM_TWO_SIDED_DISABLED) {
        // the back state is not used when two sided is disabled
        if (is_back_stencil)
            return;
        face = GL_FRONT_AND_BACK;
    } else {
        // the y flip makes the GXM front faces back faces for OpenGL, as for culling
        face = is_back_stencil ? GL_FRONT : GL_BACK;
    }
    const GxmStencilStateOp &state_op = is_back_stencil ? state.back_stencil_state_op : state.front_stencil_state_op;
    const GxmStencilStateValues &state_vals = is_back_stencil ? state.back_stencil_state_values : state.front_stencil_state_values;

    const GLenum sfail = translate_stencil_op(state_op.stencil_fail);
    const GLenum dpfail = translate_stencil_op(state_op.depth_fail);
    const GLenum dppass = translate_stencil_op(state_op.depth_pass);
    const GLenum func = translate_stencil_func(state_op.func);
    const GLint ref = state_vals.ref;
    const GLuint cmp_mask = state_vals.compare_mask;
    const GLuint write_mask = state_vals.write_mask;

    auto &cache = context.state_cache;
    auto matches = [&](const GLStateCache::StencilFace &f) {
        return f.valid && f.sfail == sfail && f.dpfail == dpfail && f.dppass == dppass
            && f.func == func && f.ref == ref && f.compare_mask == cmp_mask && f.write_mask == write_mask;
    };
    auto store = [&](GLStateCache::StencilFace &f) {
        f.valid = true;
        f.sfail = sfail;
        f.dpfail = dpfail;
        f.dppass = dppass;
        f.func = func;
        f.ref = ref;
        f.compare_mask = cmp_mask;
        f.write_mask = write_mask;
    };

    if (face == GL_FRONT_AND_BACK) {
        if (matches(cache.stencil_front) && matches(cache.stencil_back))
            return;
        glStencilOpSeparate(face, sfail, dpfail, dppass);
        glStencilFuncSeparate(face, func, ref, cmp_mask);
        glStencilMaskSeparate(face, write_mask);
        store(cache.stencil_front);
        store(cache.stencil_back);
    } else {
        auto &slot = (face == GL_FRONT) ? cache.stencil_front : cache.stencil_back;
        if (matches(slot))
            return;
        glStencilOpSeparate(face, sfail, dpfail, dppass);
        glStencilFuncSeparate(face, func, ref, cmp_mask);
        glStencilMaskSeparate(face, write_mask);
        store(slot);
    }
}

void sync_stencil_data(const GxmRecordState &state, const MemState &mem) {
    // Stencil test.
    glEnable(GL_STENCIL_TEST);
    glStencilMask(0xFF);
    if (!state.depth_stencil_surface.force_load) {
        glClearStencil(state.depth_stencil_surface.stencil);
        glClear(GL_STENCIL_BUFFER_BIT);
    }
}

void sync_depth_stencil_data(const GxmRecordState &state) {
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_STENCIL_TEST);
    glDepthMask(GL_TRUE);
    glStencilMask(0xFF);

    if (state.depth_stencil_surface.force_load)
        return;

    // clear both aspects with a single call, clearing them separately keeps the
    // driver from fast clearing the packed depth stencil buffer
    glClearDepthf(state.depth_stencil_surface.background_depth);
    glClearStencil(state.depth_stencil_surface.stencil);
    glClear(GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
}

void sync_polygon_mode(GLContext &context, const SceGxmPolygonMode mode, const bool front) {
    const GLint face = GL_FRONT_AND_BACK;
    GLenum gl_mode = GL_FILL;

    switch (mode) {
    case SCE_GXM_POLYGON_MODE_POINT_10UV:
    case SCE_GXM_POLYGON_MODE_POINT:
    case SCE_GXM_POLYGON_MODE_POINT_01UV:
    case SCE_GXM_POLYGON_MODE_TRIANGLE_POINT:
        gl_mode = GL_POINT;
        break;
    case SCE_GXM_POLYGON_MODE_LINE:
    case SCE_GXM_POLYGON_MODE_TRIANGLE_LINE:
        gl_mode = GL_LINE;
        break;
    case SCE_GXM_POLYGON_MODE_TRIANGLE_FILL:
        gl_mode = GL_FILL;
        break;
    }

    auto &cache = context.state_cache;
    if (cache.polygon_mode_valid && cache.polygon_mode == gl_mode)
        return;
    glPolygonMode(face, gl_mode);
    cache.polygon_mode = gl_mode;
    cache.polygon_mode_valid = true;
}

void sync_point_line_width(const GLState &state, GLContext &context, const std::uint32_t width, const bool is_front) {
    if (!is_front)
        return;
    const GLfloat w = width * state.res_multiplier;
    auto &cache = context.state_cache;
    if (cache.point_line_width_valid && cache.line_width == w)
        return;
    glLineWidth(w);
    glPointSize(w);
    cache.line_width = w;
    cache.point_line_width_valid = true;
}

void sync_depth_bias(GLContext &context, const int factor, const int unit, const bool is_front) {
    if (!is_front)
        return;
    auto &cache = context.state_cache;
    if (cache.depth_bias_valid && cache.depth_bias_factor == factor && cache.depth_bias_unit == unit)
        return;
    if (!cache.depth_bias_valid) {
        glEnable(GL_POLYGON_OFFSET_FILL);
#ifndef __ANDROID__
        glEnable(GL_POLYGON_OFFSET_LINE);
        glEnable(GL_POLYGON_OFFSET_POINT);
#endif
    }
    glPolygonOffset(static_cast<GLfloat>(factor), static_cast<GLfloat>(unit));
    cache.depth_bias_factor = factor;
    cache.depth_bias_unit = unit;
    cache.depth_bias_valid = true;
}

void sync_texture(GLState &state, GLContext &context, MemState &mem, std::size_t index, SceGxmTexture texture,
    const Config &config) {
    Address data_addr = texture.data_addr << 2;

    if (!is_valid_addr(mem, data_addr)) {
        LOG_WARN("Texture has freed data.");
        return;
    }

    const SceGxmTextureFormat format = gxm::get_format(texture);
    const SceGxmTextureBaseFormat base_format = gxm::get_base_format(format);
    if (gxm::is_paletted_format(base_format) && texture.palette_addr == 0) {
        LOG_WARN("Ignoring null palette texture");
        return;
    }

    if (index >= SCE_GXM_MAX_TEXTURE_UNITS) {
        // Vertex textures
        context.shader_hints.vertex_textures[index - SCE_GXM_MAX_TEXTURE_UNITS] = format;
    } else {
        context.shader_hints.fragment_textures[index] = format;
    }

    glActiveTexture(static_cast<GLenum>(static_cast<std::size_t>(GL_TEXTURE0) + index));
    state.surface_cache.set_gamma_sampling(index, 0);

    // linear strided textures use the mag filter as the min filter too
    const bool point_sampled = (texture.mag_filter == SCE_GXM_TEXTURE_FILTER_POINT)
        && ((texture.texture_type() == SCE_GXM_TEXTURE_LINEAR_STRIDED) || (texture.min_filter == SCE_GXM_TEXTURE_FILTER_POINT));

    std::uint64_t texture_as_surface = 0;
    const GLint *swizzle_surface = nullptr;
    bool only_nearest = false;
    bool filterable_surface = false;

    // A texture covering only the top left part of the surface being rendered (the first tile of an atlas, for example)
    // must be cropped out of it like the other parts: binding the whole attachment would squeeze the surface into it
    const SceGxmColorSurface &target = context.record.color_surface;
    const std::uint32_t texture_width = gxm::get_width(texture);
    const std::uint32_t texture_height = gxm::get_height(texture);
    const bool covers_part_of_target = texture_width <= target.width && texture_height <= target.height
        && (texture_width < target.width || texture_height < target.height);

    const bool can_be_surface = !gxm::is_bcn_format(base_format)
        && !renderer::texture::is_astc_format(base_format)
        && !gxm::is_yuv_format(base_format)
        && !gxm::is_paletted_format(base_format);

    if (can_be_surface && target.data.address() == data_addr && !covers_part_of_target) {
        texture_as_surface = context.current_color_attachment;
        swizzle_surface = color::translate_swizzle(context.record.color_surface.colorFormat);

        const SceGxmColorBaseFormat surface_format = gxm::get_base_format(context.record.color_surface.colorFormat);
        const bool stored_rawly = state.features.preserve_f16_nan_as_u16 && color::is_write_surface_stored_rawly(surface_format);
        filterable_surface = !stored_rawly && !color::is_write_surface_non_linearity_filtering(surface_format);
        if (index < SCE_GXM_MAX_TEXTURE_UNITS) {
            const uint32_t bit = 1U << index;
            if (filterable_surface && !point_sampled && (state.res_multiplier > 1.0f))
                context.self_sampling_mipmap_units |= bit;
            else
                context.self_sampling_mipmap_units &= ~bit;
        }

        vector_utils::push_if_not_exists(context.self_sampling_indices, index);
    } else {
        if (!context.self_sampling_indices.empty())
            vector_utils::erase_first(context.self_sampling_indices, index);
        if (index < SCE_GXM_MAX_TEXTURE_UNITS)
            context.self_sampling_mipmap_units &= ~(1U << index);

        if (can_be_surface) {
            SceGxmColorBaseFormat format_target_of_texture;

            const std::uint16_t width = static_cast<std::uint16_t>(texture_width);
            const std::uint16_t height = static_cast<std::uint16_t>(texture_height);

            if (renderer::texture::convert_base_texture_format_to_base_color_format(base_format, format_target_of_texture)) {
                std::uint32_t stride_in_pixels = width;

                switch (texture.texture_type()) {
                case SCE_GXM_TEXTURE_LINEAR_STRIDED:
                    stride_in_pixels = gxm::get_stride_in_bytes(texture) / ((gxm::bits_per_pixel(base_format) + 7) >> 3);
                    break;
                case SCE_GXM_TEXTURE_LINEAR:
                    // when the texture is linear, the stride should be aligned to 8 pixels
                    stride_in_pixels = align(stride_in_pixels, 8);
                    break;
                case SCE_GXM_TEXTURE_TILED:
                    // tiles are 32x32
                    stride_in_pixels = align(stride_in_pixels, 32);
                    break;
                default:
                    break;
                }

                std::uint32_t swizz_raw = 0;

                texture_as_surface = state.surface_cache.retrieve_color_surface_texture_handle(
                    state, width, height, stride_in_pixels, format_target_of_texture, Ptr<void>(data_addr),
                    SurfaceTextureRetrievePurpose::SAMPLING, swizz_raw, nullptr, nullptr, texture.gamma_mode != 0);

                swizzle_surface = color::translate_swizzle(static_cast<SceGxmColorFormat>(format_target_of_texture | swizz_raw));
                only_nearest = color::is_write_surface_non_linearity_filtering(format_target_of_texture);
                filterable_surface = (texture_as_surface != 0) && !only_nearest;

                if (texture_as_surface && texture.gamma_mode) {
                    const GLuint surface = static_cast<GLuint>(texture_as_surface);
                    texture_as_surface = state.surface_cache.retrieve_srgb_copy(surface);
                    if (texture_as_surface == surface)
                        // stored as sRGB: if a later draw stores it as RGBA8, the unit must still decode
                        state.surface_cache.set_gamma_sampling(index, surface);
                    else if (filterable_surface && (state.res_multiplier > 1.0f))
                        generate_surface_mipmaps(static_cast<GLuint>(texture_as_surface), state.res_multiplier);
                }
            }

            // Try to retrieve S24D8 texture
            if (!texture_as_surface) {
                SceGxmDepthStencilSurface lookup_temp;
                lookup_temp.depth_data = data_addr;
                lookup_temp.stencil_data.reset();

                texture_as_surface = state.surface_cache.retrieve_depth_stencil_texture_handle(state, mem, lookup_temp, width, height, true);
                if (texture_as_surface) {
                    only_nearest = true;
                }
            }
        }
    }

    if (index < SCE_GXM_MAX_TEXTURE_UNITS) {
        const uint32_t bit = 1U << index;
        if (filterable_surface && !point_sampled && (state.res_multiplier > 1.0f))
            context.current_frag_render_info.surface_mask |= bit;
        else
            context.current_frag_render_info.surface_mask &= ~bit;
    }

    // point-sampled reads of a mipmapped surface stay on the base level, as before mipmaps were added
    GLuint sampler = 0;
    if (filterable_surface && point_sampled && (state.res_multiplier > 1.0f)) {
        if (!context.point_surface_samplers[0]) {
            context.point_surface_samplers.init(glGenSamplers, glDeleteSamplers);
            for (int decode = 0; decode < 2; decode++) {
                const GLuint point_sampler = context.point_surface_samplers[decode];
                glSamplerParameteri(point_sampler, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                glSamplerParameteri(point_sampler, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                if (state.surface_cache.support_srgb_decode)
                    glSamplerParameteri(point_sampler, GL_TEXTURE_SRGB_DECODE_EXT, decode ? GL_DECODE_EXT : GL_SKIP_DECODE_EXT);
            }
        }
        sampler = context.point_surface_samplers[texture.gamma_mode ? 1 : 0];
    }
    if (context.bound_samplers[index] != sampler) {
        glBindSampler(static_cast<GLuint>(index), sampler);
        context.bound_samplers[index] = sampler;
    }

    if (texture_as_surface != 0) {
        const GLuint surf_tex = static_cast<GLuint>(texture_as_surface);
        glBindTexture(GL_TEXTURE_2D, surf_tex);
        SurfaceTexParamCache &param_cache = context.surface_tex_params[surf_tex];

        if (state.surface_cache.support_srgb_decode) {
            const GLint decode = texture.gamma_mode ? GL_DECODE_EXT : GL_SKIP_DECODE_EXT;
            if (!param_cache.srgb_valid || param_cache.srgb_decode != decode) {
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SRGB_DECODE_EXT, decode);
                param_cache.srgb_decode = decode;
                param_cache.srgb_valid = true;
            }
        }

        if (only_nearest && !param_cache.only_nearest) {
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            param_cache.only_nearest = true;
        }

        if (base_format != SCE_GXM_TEXTURE_BASE_FORMAT_X8U24) {
            const GLint *swizzle = texture::translate_swizzle(format);

            if (swizzle) {
                GLint desired_swizzle[4] = { GL_RED, GL_GREEN, GL_BLUE, GL_ALPHA };
                if (swizzle_surface) {
                    if (std::memcmp(swizzle_surface, swizzle, 16) != 0) {
                        for (int i = 0; i < 4; i++) {
                            if ((swizzle[i] < GL_RED) || (swizzle[i] > GL_ALPHA)) {
                                desired_swizzle[i] = swizzle[i];
                            } else {
                                desired_swizzle[i] = GL_RED + i;
                                for (int j = 0; j < 4; j++) {
                                    if (swizzle[i] == swizzle_surface[j]) {
                                        desired_swizzle[i] = GL_RED + j;
                                        break;
                                    }
                                }
                            }
                        }
                    }
                } else {
                    static bool has_happened = false;
                    LOG_TRACE_IF(!has_happened, "No surface swizzle found, use default texture swizzle");
                    has_happened = true;
                    std::memcpy(desired_swizzle, swizzle, sizeof(desired_swizzle));
                }

                if (!param_cache.swizzle_valid || std::memcmp(param_cache.swizzle, desired_swizzle, sizeof(desired_swizzle)) != 0) {
#ifdef __ANDROID__
                    for (int i = 0; i < 4; i++) {
                        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_R + i, desired_swizzle[i]);
                    }
#else
                    glTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_RGBA, desired_swizzle);
#endif
                    std::memcpy(param_cache.swizzle, desired_swizzle, sizeof(desired_swizzle));
                    param_cache.swizzle_valid = true;
                }
            }
        }
    } else {
        state.complete_readbacks_overlapping(mem, data_addr, data_addr + renderer::texture::texture_source_size(texture));
        if (config.texture_cache) {
            state.texture_cache.cache_and_bind_texture(texture, mem);
        } else {
            texture::bind_texture_without_cache(state.texture_cache, texture, mem);
        }
    }

    glActiveTexture(GL_TEXTURE0);
}

void sync_blending(GLContext &context, const GxmRecordState &state, const FeatureState &features, const MemState &mem) {
    // Blending.
    const SceGxmFragmentProgram &gxm_fragment_program = *state.fragment_program.get(mem);
    const GLFragmentProgram &fragment_program = *reinterpret_cast<GLFragmentProgram *>(
        gxm_fragment_program.renderer_data.get());

    auto &cache = context.state_cache;

    if (features.should_use_shader_interlock() && gxm_fragment_program.program.get(mem)->is_frag_color_used()) {
        if (!cache.color_mask_valid || cache.color_mask[0] != GL_FALSE || cache.color_mask[1] != GL_FALSE
            || cache.color_mask[2] != GL_FALSE || cache.color_mask[3] != GL_FALSE) {
            glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
            cache.color_mask[0] = GL_FALSE;
            cache.color_mask[1] = GL_FALSE;
            cache.color_mask[2] = GL_FALSE;
            cache.color_mask[3] = GL_FALSE;
            cache.color_mask_valid = true;
        }
        return;
    }

    if (!cache.color_mask_valid || cache.color_mask[0] != fragment_program.color_mask_red
        || cache.color_mask[1] != fragment_program.color_mask_green
        || cache.color_mask[2] != fragment_program.color_mask_blue
        || cache.color_mask[3] != fragment_program.color_mask_alpha) {
        glColorMask(fragment_program.color_mask_red, fragment_program.color_mask_green, fragment_program.color_mask_blue, fragment_program.color_mask_alpha);
        cache.color_mask[0] = fragment_program.color_mask_red;
        cache.color_mask[1] = fragment_program.color_mask_green;
        cache.color_mask[2] = fragment_program.color_mask_blue;
        cache.color_mask[3] = fragment_program.color_mask_alpha;
        cache.color_mask_valid = true;
    }

    if (fragment_program.blend_enabled) {
        if (!cache.blend_state_valid || !cache.blend_enabled)
            glEnable(GL_BLEND);
        if (!cache.blend_state_valid || !cache.blend_enabled
            || cache.blend_color_func != fragment_program.color_func || cache.blend_alpha_func != fragment_program.alpha_func) {
            glBlendEquationSeparate(fragment_program.color_func, fragment_program.alpha_func);
            cache.blend_color_func = fragment_program.color_func;
            cache.blend_alpha_func = fragment_program.alpha_func;
        }
        if (!cache.blend_state_valid || !cache.blend_enabled
            || cache.blend_color_src != fragment_program.color_src || cache.blend_color_dst != fragment_program.color_dst
            || cache.blend_alpha_src != fragment_program.alpha_src || cache.blend_alpha_dst != fragment_program.alpha_dst) {
            glBlendFuncSeparate(fragment_program.color_src, fragment_program.color_dst, fragment_program.alpha_src, fragment_program.alpha_dst);
            cache.blend_color_src = fragment_program.color_src;
            cache.blend_color_dst = fragment_program.color_dst;
            cache.blend_alpha_src = fragment_program.alpha_src;
            cache.blend_alpha_dst = fragment_program.alpha_dst;
        }
        cache.blend_enabled = true;
    } else {
        if (!cache.blend_state_valid || cache.blend_enabled)
            glDisable(GL_BLEND);
        cache.blend_enabled = false;
    }
    cache.blend_state_valid = true;
}

void clear_previous_uniform_storage(GLContext &context) {
    context.vertex_uniform_buffer_storage_ptr.first = nullptr;
    context.vertex_uniform_buffer_storage_ptr.second = 0;
    context.fragment_uniform_buffer_storage_ptr.first = nullptr;
    context.fragment_uniform_buffer_storage_ptr.second = 0;
}

void sync_vertex_streams_and_attributes(const GLState &renderer, GLContext &context, GxmRecordState &state, const MemState &mem) {
    const SceGxmVertexProgram &vertex_program = *state.vertex_program.get(mem);
    GLVertexProgram *glvert = reinterpret_cast<GLVertexProgram *>(vertex_program.renderer_data.get());

    if (!glvert->attribs_cached) {
        glvert->cached_attribs.clear();
        glvert->used_streams_mask = 0;
        glvert->stream_strides.fill(0);
        glvert->stream_divisors.fill(0);
        glvert->can_use_attrib_binding = true;

        for (const SceGxmVertexAttribute &attribute : vertex_program.attributes) {
            auto info_it = glvert->attribute_infos.find(attribute.regIndex);
            if (info_it == glvert->attribute_infos.end())
                continue;

            const SceGxmVertexStream &stream = vertex_program.streams[attribute.streamIndex];
            const shader::usse::AttributeInformation &info = info_it->second;

            GLenum type = attribute_format_to_gl_type(attribute.format);
            const GLboolean normalized = attribute_format_normalized(attribute.format);
            bool upload_integral = false;

            uint32_t array_size = 1;
            uint32_t array_element_size = 0;
            uint8_t component_count = attribute.componentCount;

            if (info.regformat) {
                component_count = info.component_count;
                switch (info.gxm_type) {
                case SCE_GXM_PARAMETER_TYPE_U8:
                case SCE_GXM_PARAMETER_TYPE_S8:
                case SCE_GXM_PARAMETER_TYPE_C10:
                    type = GL_UNSIGNED_BYTE;
                    break;
                case SCE_GXM_PARAMETER_TYPE_U16:
                case SCE_GXM_PARAMETER_TYPE_S16:
                case SCE_GXM_PARAMETER_TYPE_F16:
                    type = GL_UNSIGNED_SHORT;
                    break;
                default:
                    type = GL_UNSIGNED_INT;
                    break;
                }
                upload_integral = true;

                if (info.gxm_type == SCE_GXM_PARAMETER_TYPE_C10)
                    component_count = (component_count * 10 + 7) / 8;

                if (component_count > 4) {
                    array_size = (component_count + 3) / 4;
                    array_element_size = 4 * gxm::attribute_format_size(attribute.format);
                    component_count = 4;
                }
            } else {
                switch (info.gxm_type) {
                case SCE_GXM_PARAMETER_TYPE_U8:
                case SCE_GXM_PARAMETER_TYPE_S8:
                case SCE_GXM_PARAMETER_TYPE_U16:
                case SCE_GXM_PARAMETER_TYPE_S16:
                case SCE_GXM_PARAMETER_TYPE_U32:
                case SCE_GXM_PARAMETER_TYPE_S32:
                    upload_integral = true;
                    break;
                default:
                    break;
                }
            }

            const bool integral = upload_integral || (attribute.format == SCE_GXM_ATTRIBUTE_FORMAT_UNTYPED);
            const GLuint divisor = gxm::is_stream_instancing(static_cast<SceGxmIndexSource>(stream.indexSource)) ? 1u : 0u;
            const uint16_t stream_index = attribute.streamIndex;
            glvert->used_streams_mask |= static_cast<uint16_t>(1u << stream_index);
            glvert->stream_strides[stream_index] = stream.stride;
            glvert->stream_divisors[stream_index] = divisor;

            for (uint32_t i = 0; i < array_size; i++) {
                const uint32_t loc = static_cast<uint32_t>(info.location) + i;
                const uint32_t offset = i * array_element_size + attribute.offset;
                if (loc >= 32 || offset > 2047 || stream_index >= SCE_GXM_MAX_VERTEX_STREAMS)
                    glvert->can_use_attrib_binding = false;
                glvert->cached_attribs.push_back({ loc, component_count, type, normalized, integral, stream_index, stream.stride, offset, divisor });
            }
        }
        glvert->attribs_cached = true;
    }

    std::array<std::size_t, SCE_GXM_MAX_VERTEX_STREAMS> offset_in_buffer{};
    for (std::size_t i = 0; i < SCE_GXM_MAX_VERTEX_STREAMS; i++) {
        if (state.vertex_streams[i].data) {
            std::pair<std::uint8_t *, std::size_t> result = context.vertex_stream_ring_buffer.allocate(state.vertex_streams[i].size);
            if (!result.first) {
                LOG_ERROR("Failed to allocate vertex stream data from GPU!");
            } else {
                std::memcpy(result.first, state.vertex_streams[i].data.get(mem), state.vertex_streams[i].size);
                offset_in_buffer[i] = result.second;
            }

            state.vertex_streams[i].data = nullptr;
            state.vertex_streams[i].size = 0;
        }
    }

    const GLuint vbo = context.vertex_stream_ring_buffer.handle();

#ifndef __ANDROID__
    if (renderer.support_vertex_attrib_binding && glvert->can_use_attrib_binding) {
        if (context.bound_vertex_program_id != glvert->id) {
            uint32_t new_enabled_mask = 0;
            for (const GLVertexAttribDesc &desc : glvert->cached_attribs) {
                if (desc.upload_integral)
                    glVertexAttribIFormat(desc.location, desc.component_count, desc.type, desc.offset);
                else
                    glVertexAttribFormat(desc.location, desc.component_count, desc.type, desc.normalized, desc.offset);

                glVertexAttribBinding(desc.location, desc.stream_index);
                new_enabled_mask |= (1u << desc.location);
            }

            uint32_t to_enable = new_enabled_mask & ~context.enabled_attrib_mask;
            while (to_enable) {
                const GLuint loc = static_cast<GLuint>(std::countr_zero(to_enable));
                glEnableVertexAttribArray(loc);
                to_enable &= to_enable - 1;
            }
            uint32_t to_disable = context.enabled_attrib_mask & ~new_enabled_mask;
            while (to_disable) {
                const GLuint loc = static_cast<GLuint>(std::countr_zero(to_disable));
                glDisableVertexAttribArray(loc);
                to_disable &= to_disable - 1;
            }
            context.enabled_attrib_mask = new_enabled_mask;

            uint16_t stream_mask = glvert->used_streams_mask;
            while (stream_mask) {
                const GLuint s = static_cast<GLuint>(std::countr_zero(stream_mask));
                glVertexBindingDivisor(s, glvert->stream_divisors[s]);
                stream_mask &= static_cast<uint16_t>(stream_mask - 1);
            }

            context.bound_vertex_program_id = glvert->id;
        }

        uint16_t stream_mask = glvert->used_streams_mask;
        while (stream_mask) {
            const GLuint s = static_cast<GLuint>(std::countr_zero(stream_mask));
            glBindVertexBuffer(s, vbo, static_cast<GLintptr>(offset_in_buffer[s]), glvert->stream_strides[s]);
            stream_mask &= static_cast<uint16_t>(stream_mask - 1);
        }
        return;
    }
#endif

    context.bound_vertex_program_id = 0;
    glBindBuffer(GL_ARRAY_BUFFER, vbo);

    for (const GLVertexAttribDesc &desc : glvert->cached_attribs) {
        const auto ptr = reinterpret_cast<const GLvoid *>(static_cast<std::uintptr_t>(desc.offset + offset_in_buffer[desc.stream_index]));
        if (desc.upload_integral)
            glVertexAttribIPointer(desc.location, desc.component_count, desc.type, desc.stride, ptr);
        else
            glVertexAttribPointer(desc.location, desc.component_count, desc.type, desc.normalized, desc.stride, ptr);

        if (desc.location < 32) {
            const uint32_t bit = 1u << desc.location;
            if (!(context.enabled_attrib_mask & bit)) {
                glEnableVertexAttribArray(desc.location);
                context.enabled_attrib_mask |= bit;
            }
            const bool cur_div = (context.attrib_divisor_mask & bit) != 0;
            const bool want_div = desc.divisor != 0;
            if (cur_div != want_div) {
                glVertexAttribDivisor(desc.location, desc.divisor);
                if (want_div)
                    context.attrib_divisor_mask |= bit;
                else
                    context.attrib_divisor_mask &= ~bit;
            }
        } else {
            glEnableVertexAttribArray(desc.location);
            glVertexAttribDivisor(desc.location, desc.divisor);
        }
    }
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}
} // namespace renderer::gl
