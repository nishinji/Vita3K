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

#include <renderer/gl/screen_filters.h>

#include <glutil/object.h>
#include <util/fs.h>
#include <util/types.h>

#include <memory>
#include <string_view>

namespace renderer::gl {

class ScreenRenderer {
public:
    ScreenRenderer() = default;
    ~ScreenRenderer();

    bool init(const fs::path &static_assets);
    void render(const SceFVector2 &viewport_pos, const SceFVector2 &viewport_size, const float *uvs, const GLuint texture, const SceFVector2 texture_size, GLuint default_fbo = 0);

    void destroy();

    // Fallback in case surface cache does not contain what we want
    // For homebrew that does not use GXM
    GLuint get_resident_texture() const {
        return m_screen_texture;
    }

    // selects the filter by name, falling back to no filter when it is not one we handle
    // or when it could not be set up
    void set_filter(const std::string_view &filter);

    // FSR needs shader features that are not available everywhere, so it is only
    // offered once its shaders have actually been compiled
    bool is_fsr_available() const {
        return static_cast<bool>(m_fsr_filter);
    }

    static constexpr uint32_t screen_vertex_count = 4;

    //
    // drawing helpers used by the filters, they all rely on the state render() saves
    //

    // points the attributes of the currently bound quad at the given program
    void setup_vertex_attributes(GLuint program);
    // binds the quad textured with the region of the source given by uvs
    void bind_screen_quad(const float *uvs);
    // binds the quad covering a whole target (uv 0..1), for the offscreen passes and for
    // the filters whose screen pass reads an offscreen target instead of the source
    void bind_offscreen_quad();
    // binds the target the frame is presented to and clears it
    void begin_screen_pass(const SceFVector2 &viewport_pos, const SceFVector2 &viewport_size, GLuint default_fbo);

private:
    struct screen_vertex {
        GLfloat pos[3];
        GLfloat uv[2];
    };

    static constexpr size_t screen_vertex_size = sizeof(screen_vertex);

    using screen_vertices_t = screen_vertex[screen_vertex_count];

    GLuint m_vao{ 0 };
    GLuint m_vbo{ 0 };
    GLuint m_offscreen_vao{ 0 };
    GLuint m_offscreen_vbo{ 0 };
    GLuint m_screen_texture{ 0 };

    float last_uvs[4] = { 0.0f, 0.0f, 1.0f, 1.0f };

    std::unique_ptr<ScreenFilter> m_nearest_filter;
    std::unique_ptr<ScreenFilter> m_bilinear_filter;
    std::unique_ptr<ScreenFilter> m_bicubic_filter;
    std::unique_ptr<ScreenFilter> m_fxaa_filter;
    // the optional filters are left empty when they could not be set up
    std::unique_ptr<ScreenFilter> m_smaa_filter;
    std::unique_ptr<ScreenFilter> m_fsr_filter;
    // points at one of the above, never owns it
    ScreenFilter *m_filter = nullptr;
};

} // namespace renderer::gl
