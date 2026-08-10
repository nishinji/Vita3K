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

#include <glutil/shader.h>
#include <renderer/AreaTex.h>
#include <renderer/SearchTex.h>
#include <renderer/gl/screen_filters.h>
#include <renderer/gl/screen_render.h>
#include <util/log.h>

#include <string>
#include <vector>

namespace renderer::gl {

// the same default the vulkan backend uses for its FSR filter
static constexpr float fsr_sharpening = 0.2f;

static fs::path builtin_shaders_path(const fs::path &static_assets) {
    return static_assets / "shaders-builtin/opengl";
}

static GLuint create_clamped_sampler(GLint filter) {
    GLuint sampler = 0;
    glGenSamplers(1, &sampler);
    glSamplerParameteri(sampler, GL_TEXTURE_MIN_FILTER, filter);
    glSamplerParameteri(sampler, GL_TEXTURE_MAG_FILTER, filter);
    glSamplerParameteri(sampler, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glSamplerParameteri(sampler, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    return sampler;
}

//
// single pass filters
//

std::string_view SinglePassScreenFilter::get_fragment_name() const {
    return "render_main.frag";
}

bool SinglePassScreenFilter::init(const fs::path &static_assets) {
    const auto shaders_path = builtin_shaders_path(static_assets);
    program = ::gl::load_shaders(shaders_path / "render_main.vert",
        shaders_path / std::string(get_fragment_name()));
    if (!program)
        return false;

    sampler = create_sampler();
    return true;
}

void SinglePassScreenFilter::destroy() {
    program.reset();
    glDeleteSamplers(1, &sampler);
    sampler = 0;
}

void SinglePassScreenFilter::render(GLuint texture, const SceFVector2 &texture_size, const float *uvs,
    const SceFVector2 &viewport_pos, const SceFVector2 &viewport_size, GLuint default_fbo) {
    screen.begin_screen_pass(viewport_pos, viewport_size, default_fbo);

    glUseProgram(*program);
    screen.bind_screen_quad(uvs);
    screen.setup_vertex_attributes(*program);
    set_uniforms(texture_size);

    glBindTexture(GL_TEXTURE_2D, texture);
    glBindSampler(0, sampler);
    glDrawArrays(GL_TRIANGLE_FAN, 0, ScreenRenderer::screen_vertex_count);
}

GLuint NearestScreenFilter::create_sampler() {
    return create_clamped_sampler(GL_NEAREST);
}

GLuint BilinearScreenFilter::create_sampler() {
    return create_clamped_sampler(GL_LINEAR);
}

std::string_view BicubicScreenFilter::get_fragment_name() const {
    return "render_main_bicubic.frag";
}

GLuint BicubicScreenFilter::create_sampler() {
    return create_clamped_sampler(GL_LINEAR);
}

std::string_view FXAAScreenFilter::get_fragment_name() const {
    return "render_main_fxaa.frag";
}

GLuint FXAAScreenFilter::create_sampler() {
    return create_clamped_sampler(GL_LINEAR);
}

void FXAAScreenFilter::set_uniforms(const SceFVector2 &texture_size) {
    const GLint inv_screen_location = glGetUniformLocation(*program, "inv_frame_size");
    glUniform2f(inv_screen_location, 1 / texture_size.x, 1 / texture_size.y);
}

//
// SMAA
//

bool SMAAScreenFilter::init(const fs::path &static_assets) {
    const auto shaders_path = builtin_shaders_path(static_assets);

    // the SMAA library is a single header shared with the vulkan backend, it is pasted
    // in front of each shader (along with its configuration) instead of being #included
    std::vector<char> smaa_library;
    if (!fs_utils::read_data(static_assets / "shaders-builtin/SMAA.hlsl", smaa_library)) {
        LOG_ERROR("Couldn't open SMAA.hlsl, SMAA will not be available");
        return false;
    }

    // SMAA_GLSL_3 only needs GLSL 1.30 features, so the same code works on GLES 3.0
    const std::string config = "#define SMAA_GLSL_3 1\n"
                               "#define SMAA_PRESET_HIGH 1\n"
                               "uniform vec4 rt_metrics;\n"
                               "#define SMAA_RT_METRICS rt_metrics\n";
    // the pixel shaders use discard, which is not allowed in a vertex shader
    const std::string vertex_prelude = config + "#define SMAA_INCLUDE_PS 0\n" + std::string(smaa_library.data(), smaa_library.size());
    const std::string fragment_prelude = config + "#define SMAA_INCLUDE_VS 0\n" + std::string(smaa_library.data(), smaa_library.size());

    edge_program = ::gl::load_shaders(shaders_path / "smaa_edge.vert",
        shaders_path / "smaa_edge.frag", vertex_prelude, fragment_prelude);
    blend_program = ::gl::load_shaders(shaders_path / "smaa_blend.vert",
        shaders_path / "smaa_blend.frag", vertex_prelude, fragment_prelude);
    neighborhood_program = ::gl::load_shaders(shaders_path / "smaa_neighborhood.vert",
        shaders_path / "smaa_neighborhood.frag", vertex_prelude, fragment_prelude);

    if (!edge_program || !blend_program || !neighborhood_program) {
        LOG_ERROR("Couldn't compile the SMAA shaders, SMAA will not be available");
        edge_program.reset();
        blend_program.reset();
        neighborhood_program.reset();
        return false;
    }

    // bind each sampler to the texture unit it is given in render_offscreen / render
    glUseProgram(*blend_program);
    glUniform1i(glGetUniformLocation(*blend_program, "edges_tex"), 0);
    glUniform1i(glGetUniformLocation(*blend_program, "area_tex"), 1);
    glUniform1i(glGetUniformLocation(*blend_program, "search_tex"), 2);
    glUseProgram(*neighborhood_program);
    glUniform1i(glGetUniformLocation(*neighborhood_program, "fb"), 0);
    glUniform1i(glGetUniformLocation(*neighborhood_program, "blend_tex"), 1);
    glUseProgram(0);

    // upload the two precomputed lookup tables
    // their rows are not always a multiple of 4 bytes (the search texture is 66 wide)
    GLint last_unpack_alignment;
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &last_unpack_alignment);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

    const auto upload_lut = [](GLuint &texture, GLenum internal_format, GLenum format,
                                GLsizei width, GLsizei height, const void *data, GLint filter) {
        glGenTextures(1, &texture);
        glBindTexture(GL_TEXTURE_2D, texture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
        glTexImage2D(GL_TEXTURE_2D, 0, internal_format, width, height, 0, format, GL_UNSIGNED_BYTE, data);
    };

    upload_lut(area_texture, GL_RG8, GL_RG, AREATEX_WIDTH, AREATEX_HEIGHT, areaTexBytes, GL_LINEAR);
    upload_lut(search_texture, GL_R8, GL_RED, SEARCHTEX_WIDTH, SEARCHTEX_HEIGHT, searchTexBytes, GL_NEAREST);

    glPixelStorei(GL_UNPACK_ALIGNMENT, last_unpack_alignment);
    glBindTexture(GL_TEXTURE_2D, 0);

    glGenFramebuffers(1, &edges_fbo);
    glGenFramebuffers(1, &blend_fbo);
    glGenTextures(1, &edges_texture);
    glGenTextures(1, &blend_texture);

    return true;
}

void SMAAScreenFilter::destroy() {
    glDeleteFramebuffers(1, &edges_fbo);
    glDeleteFramebuffers(1, &blend_fbo);
    edges_fbo = 0;
    blend_fbo = 0;

    glDeleteTextures(1, &edges_texture);
    glDeleteTextures(1, &blend_texture);
    glDeleteTextures(1, &area_texture);
    glDeleteTextures(1, &search_texture);
    edges_texture = 0;
    blend_texture = 0;
    area_texture = 0;
    search_texture = 0;

    target_width = 0;
    target_height = 0;

    edge_program.reset();
    blend_program.reset();
    neighborhood_program.reset();
}

void SMAAScreenFilter::resize_targets(GLsizei width, GLsizei height) {
    if (width == target_width && height == target_height)
        return;

    const auto attach = [&](GLuint fbo, GLuint texture) {
        glBindTexture(GL_TEXTURE_2D, texture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);

        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
            LOG_ERROR("Incomplete SMAA framebuffer ({}x{})", width, height);
    };

    attach(edges_fbo, edges_texture);
    attach(blend_fbo, blend_texture);
    glBindTexture(GL_TEXTURE_2D, 0);

    target_width = width;
    target_height = height;
}

void SMAAScreenFilter::render_offscreen(GLuint texture, const SceFVector2 &texture_size) {
    const auto width = static_cast<GLsizei>(texture_size.x);
    const auto height = static_cast<GLsizei>(texture_size.y);
    resize_targets(width, height);

    glViewport(0, 0, width, height);
    screen.bind_offscreen_quad();

    const auto run_pass = [&](GLuint fbo, GLuint program) {
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        glUseProgram(program);
        glUniform4f(glGetUniformLocation(program, "rt_metrics"),
            1.0f / texture_size.x, 1.0f / texture_size.y, texture_size.x, texture_size.y);
        screen.setup_vertex_attributes(program);
        glDrawArrays(GL_TRIANGLE_FAN, 0, ScreenRenderer::screen_vertex_count);
    };

    // the lookup tables carry their own filtering (linear for the area one, point for
    // the search one), a leftover sampler object would override it
    glBindSampler(0, 0);
    glBindSampler(1, 0);
    glBindSampler(2, 0);

    // pass 1: edge detection, reads the source color
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, texture);
    run_pass(edges_fbo, *edge_program);

    // pass 2: blending weight calculation, reads the edges and both lookup tables
    glBindTexture(GL_TEXTURE_2D, edges_texture);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, area_texture);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, search_texture);
    run_pass(blend_fbo, *blend_program);

    // leave the lookup tables behind, pass 3 needs unit 1 for the blend texture
    glBindTexture(GL_TEXTURE_2D, 0);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, 0);
    glActiveTexture(GL_TEXTURE0);
}

void SMAAScreenFilter::render(GLuint texture, const SceFVector2 &texture_size, const float *uvs,
    const SceFVector2 &viewport_pos, const SceFVector2 &viewport_size, GLuint default_fbo) {
    // the first two passes render the whole source into offscreen targets, the last one
    // is the screen pass below
    render_offscreen(texture, texture_size);

    screen.begin_screen_pass(viewport_pos, viewport_size, default_fbo);

    glUseProgram(*neighborhood_program);
    screen.bind_screen_quad(uvs);
    screen.setup_vertex_attributes(*neighborhood_program);

    const GLint rt_metrics_location = glGetUniformLocation(*neighborhood_program, "rt_metrics");
    glUniform4f(rt_metrics_location, 1 / texture_size.x, 1 / texture_size.y, texture_size.x, texture_size.y);
    // the blending weights computed by the offscreen passes cover the whole source,
    // so they are addressed with the same uvs as the color texture
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, blend_texture);
    glActiveTexture(GL_TEXTURE0);

    glBindTexture(GL_TEXTURE_2D, texture);
    glDrawArrays(GL_TRIANGLE_FAN, 0, ScreenRenderer::screen_vertex_count);

    // only unit 0 is saved and restored around the frame
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, 0);
    glActiveTexture(GL_TEXTURE0);
}

//
// FSR
//

bool FSRScreenFilter::init(const fs::path &static_assets) {
    const auto shaders_path = builtin_shaders_path(static_assets);

    // GLSL has no #include, so the FSR library is pasted in front of each shader (along
    // with its configuration and the callbacks it expects), like SMAA.hlsl above
    std::vector<char> ffx_a;
    std::vector<char> ffx_fsr1;
    if (!fs_utils::read_data(static_assets / "shaders-builtin/GPUOpen/ffx_a.h", ffx_a)
        || !fs_utils::read_data(static_assets / "shaders-builtin/GPUOpen/ffx_fsr1.h", ffx_fsr1)) {
        LOG_ERROR("Couldn't open the FSR library, FSR will not be available");
        return false;
    }

    // the 32 bit path is used because the packed one needs 16 bit arithmetic, which core
    // GLSL does not have. It still needs a newer language version than the other screen
    // shaders: packHalf2x16 is only core since GLSL 4.20, and textureGather since GLSL ES
    // 3.10. FSR also does its bit twiddling on ints, which default to mediump on GLES
#ifdef __ANDROID__
    constexpr std::string_view version = "#version 310 es\nprecision highp float;\nprecision highp int;\n";
#else
    constexpr std::string_view version = "#version 420 core\n";
#endif

    const std::string library = "#define A_GPU 1\n"
                                "#define A_GLSL 1\n"
        + std::string(ffx_a.data(), ffx_a.size());
    const std::string library_end(ffx_fsr1.data(), ffx_fsr1.size());

    const std::string easu_prelude = library
        + "#define FSR_EASU_F 1\n"
          "uniform sampler2D fb;\n"
          "AF4 FsrEasuRF(AF2 p) { return textureGather(fb, p, 0); }\n"
          "AF4 FsrEasuGF(AF2 p) { return textureGather(fb, p, 1); }\n"
          "AF4 FsrEasuBF(AF2 p) { return textureGather(fb, p, 2); }\n"
        + library_end;
    const std::string rcas_prelude = library
        + "#define FSR_RCAS_F 1\n"
          "uniform sampler2D fb;\n"
          // the sharpening kernel reads one texel around its own, which is out of bounds
          // (and undefined) on the borders, so it is clamped back in
          "AF4 FsrRcasLoadF(ASU2 p) { return texelFetch(fb, clamp(p, ASU2(0), textureSize(fb, 0) - ASU2(1)), 0); }\n"
          "void FsrRcasInputF(inout AF1 r, inout AF1 g, inout AF1 b) {}\n"
        + library_end;

    const auto vertex_path = shaders_path / "render_main.vert";
    easu_program = ::gl::load_shaders(vertex_path, shaders_path / "fsr_easu.frag", "", easu_prelude, version);
    rcas_program = ::gl::load_shaders(vertex_path, shaders_path / "fsr_rcas.frag", "", rcas_prelude, version);

    if (!easu_program || !rcas_program) {
        LOG_ERROR("Couldn't compile the FSR shaders, FSR will not be available");
        easu_program.reset();
        rcas_program.reset();
        return false;
    }

    // both passes read their input from texture unit 0
    glUseProgram(*easu_program);
    glUniform1i(glGetUniformLocation(*easu_program, "fb"), 0);
    glUseProgram(*rcas_program);
    glUniform1i(glGetUniformLocation(*rcas_program, "fb"), 0);
    glUseProgram(0);

    // the gathers of the upscaling pass reach outside of the displayed region on the
    // edges, so the source has to be clamped whatever the game left on the texture
    glGenSamplers(1, &sampler);
    glSamplerParameteri(sampler, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glSamplerParameteri(sampler, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glSamplerParameteri(sampler, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glSamplerParameteri(sampler, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glGenFramebuffers(1, &fbo);
    glGenTextures(1, &upscaled_texture);

    return true;
}

void FSRScreenFilter::destroy() {
    glDeleteFramebuffers(1, &fbo);
    fbo = 0;

    glDeleteTextures(1, &upscaled_texture);
    upscaled_texture = 0;

    glDeleteSamplers(1, &sampler);
    sampler = 0;

    target_width = 0;
    target_height = 0;

    easu_program.reset();
    rcas_program.reset();
}

void FSRScreenFilter::resize_target(GLsizei width, GLsizei height) {
    if (width == target_width && height == target_height)
        return;

    glBindTexture(GL_TEXTURE_2D, upscaled_texture);
    // the sharpening pass reads the target with texelFetch, it never gets filtered
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, width, height, 0, GL_RGBA, GL_HALF_FLOAT, nullptr);

    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, upscaled_texture, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        // half float targets are not renderable on every GLES implementation, the banding
        // an 8 bit intermediate adds is better than not rendering at all
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, upscaled_texture, 0);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
            LOG_ERROR("Incomplete FSR framebuffer ({}x{})", width, height);
    }
    glBindTexture(GL_TEXTURE_2D, 0);

    target_width = width;
    target_height = height;
}

void FSRScreenFilter::render_easu(GLuint texture, const SceFVector2 &texture_size, const float *uvs, const SceFVector2 &output_size) {
    const auto width = static_cast<GLsizei>(output_size.x);
    const auto height = static_cast<GLsizei>(output_size.y);
    resize_target(width, height);

    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glViewport(0, 0, width, height);
    screen.bind_offscreen_quad();

    const GLuint program = *easu_program;
    glUseProgram(program);
    // uvs[1] is the top of the displayed region and uvs[3] its bottom, matching the
    // top-down space FSR works in
    glUniform2f(glGetUniformLocation(program, "src_offset"), uvs[0] * texture_size.x, uvs[1] * texture_size.y);
    glUniform2f(glGetUniformLocation(program, "src_size"), (uvs[2] - uvs[0]) * texture_size.x, (uvs[3] - uvs[1]) * texture_size.y);
    glUniform2f(glGetUniformLocation(program, "texture_size"), texture_size.x, texture_size.y);
    glUniform2f(glGetUniformLocation(program, "output_size"), output_size.x, output_size.y);
    screen.setup_vertex_attributes(program);

    glBindSampler(0, sampler);
    glBindTexture(GL_TEXTURE_2D, texture);
    glDrawArrays(GL_TRIANGLE_FAN, 0, ScreenRenderer::screen_vertex_count);

    // the sharpening pass uses texelFetch, it must not inherit this sampler
    glBindSampler(0, 0);
    glBindTexture(GL_TEXTURE_2D, 0);
}

void FSRScreenFilter::render(GLuint texture, const SceFVector2 &texture_size, const float *uvs,
    const SceFVector2 &viewport_pos, const SceFVector2 &viewport_size, GLuint default_fbo) {
    // the upscaling pass brings the displayed region to the size it is shown at, the
    // screen pass below is then only the sharpening one
    render_easu(texture, texture_size, uvs, viewport_size);

    screen.begin_screen_pass(viewport_pos, viewport_size, default_fbo);

    glUseProgram(*rcas_program);
    // the upscaled target already holds exactly the region to display, so it is drawn
    // with the plain quad instead of the source uvs
    screen.bind_offscreen_quad();
    screen.setup_vertex_attributes(*rcas_program);

    glUniform2f(glGetUniformLocation(*rcas_program, "output_size"), viewport_size.x, viewport_size.y);
    glUniform1f(glGetUniformLocation(*rcas_program, "sharpening"), fsr_sharpening);

    glBindTexture(GL_TEXTURE_2D, upscaled_texture);
    glDrawArrays(GL_TRIANGLE_FAN, 0, ScreenRenderer::screen_vertex_count);
}

} // namespace renderer::gl
