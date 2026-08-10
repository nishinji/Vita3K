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

#include <glutil/object.h>
#include <util/fs.h>
#include <util/types.h>

#include <string_view>

namespace renderer::gl {

class ScreenRenderer;

class ScreenFilter {
protected:
    ScreenRenderer &screen;

public:
    ScreenFilter(ScreenRenderer &screen_renderer)
        : screen(screen_renderer) {}
    virtual ~ScreenFilter() = default;

    // compiles the programs and creates the resources the filter needs. Returns false if
    // it could not be set up, the filter must then not be selected
    virtual bool init(const fs::path &static_assets) = 0;
    // the GL context is still current here, unlike in the destructor
    virtual void destroy() = 0;
    virtual std::string_view get_name() const = 0;

    // runs the offscreen passes the filter needs, then draws the frame into default_fbo.
    // uvs is the region of the source texture to display and is never null
    virtual void render(GLuint texture, const SceFVector2 &texture_size, const float *uvs,
        const SceFVector2 &viewport_pos, const SceFVector2 &viewport_size, GLuint default_fbo)
        = 0;
};

// draws the source straight to the screen with a single fragment shader
class SinglePassScreenFilter : public ScreenFilter {
protected:
    SharedGLObject program;
    GLuint sampler{ 0 };

    // file name inside shaders-builtin/opengl
    virtual std::string_view get_fragment_name() const;
    // the source is read through this sampler, so the filtering does not depend on what
    // the game happened to leave on the texture
    virtual GLuint create_sampler() = 0;
    // set once the program is bound, for the filters that need to know the frame
    virtual void set_uniforms(const SceFVector2 &texture_size) {}

public:
    using ScreenFilter::ScreenFilter;

    bool init(const fs::path &static_assets) override;
    void destroy() override;
    void render(GLuint texture, const SceFVector2 &texture_size, const float *uvs,
        const SceFVector2 &viewport_pos, const SceFVector2 &viewport_size, GLuint default_fbo) override;
};

class NearestScreenFilter : public SinglePassScreenFilter {
protected:
    GLuint create_sampler() override;

public:
    using SinglePassScreenFilter::SinglePassScreenFilter;

    std::string_view get_name() const override {
        return "Nearest";
    }
};

class BilinearScreenFilter : public SinglePassScreenFilter {
protected:
    GLuint create_sampler() override;

public:
    using SinglePassScreenFilter::SinglePassScreenFilter;

    std::string_view get_name() const override {
        return "Bilinear";
    }
};

class BicubicScreenFilter : public SinglePassScreenFilter {
protected:
    // the Bicubic filter uses a custom shader. It builds its result out of four bilinear
    // taps placed between texels, so it needs the linear sampler
    std::string_view get_fragment_name() const override;
    GLuint create_sampler() override;

public:
    using SinglePassScreenFilter::SinglePassScreenFilter;

    std::string_view get_name() const override {
        return "Bicubic";
    }
};

class FXAAScreenFilter : public SinglePassScreenFilter {
protected:
    // the FXAA filter uses a custom shader
    std::string_view get_fragment_name() const override;
    GLuint create_sampler() override;
    void set_uniforms(const SceFVector2 &texture_size) override;

public:
    using SinglePassScreenFilter::SinglePassScreenFilter;

    std::string_view get_name() const override {
        return "FXAA";
    }
};

// two offscreen passes at the source resolution followed by a screen pass
class SMAAScreenFilter : public ScreenFilter {
private:
    SharedGLObject edge_program;
    SharedGLObject blend_program;
    SharedGLObject neighborhood_program;

    // precomputed lookup tables
    GLuint area_texture{ 0 };
    GLuint search_texture{ 0 };

    // offscreen targets, sized to the source texture (lazily recreated)
    GLuint edges_texture{ 0 };
    GLuint blend_texture{ 0 };
    GLuint edges_fbo{ 0 };
    GLuint blend_fbo{ 0 };
    GLsizei target_width{ 0 };
    GLsizei target_height{ 0 };

    // (re)creates the offscreen edges/blend targets when the source resolution changes
    void resize_targets(GLsizei width, GLsizei height);
    // runs the edge detection and blending weight passes into the offscreen targets
    void render_offscreen(GLuint texture, const SceFVector2 &texture_size);

public:
    using ScreenFilter::ScreenFilter;

    bool init(const fs::path &static_assets) override;
    void destroy() override;
    void render(GLuint texture, const SceFVector2 &texture_size, const float *uvs,
        const SceFVector2 &viewport_pos, const SceFVector2 &viewport_size, GLuint default_fbo) override;

    std::string_view get_name() const override {
        return "SMAA";
    }
};

// an upscaling pass into an offscreen target at the displayed size followed by a
// sharpening screen pass
class FSRScreenFilter : public ScreenFilter {
private:
    SharedGLObject easu_program;
    SharedGLObject rcas_program;

    // dst of the easu pass, src of the rcas pass
    GLuint upscaled_texture{ 0 };
    GLuint fbo{ 0 };
    // EASU expects the source to be sampled with clamping, whatever the game left behind
    GLuint sampler{ 0 };
    GLsizei target_width{ 0 };
    GLsizei target_height{ 0 };

    // (re)creates the upscaled target when the displayed size changes
    void resize_target(GLsizei width, GLsizei height);
    // runs the EASU (upscaling) pass into the offscreen target
    void render_easu(GLuint texture, const SceFVector2 &texture_size, const float *uvs, const SceFVector2 &output_size);

public:
    using ScreenFilter::ScreenFilter;

    bool init(const fs::path &static_assets) override;
    void destroy() override;
    void render(GLuint texture, const SceFVector2 &texture_size, const float *uvs,
        const SceFVector2 &viewport_pos, const SceFVector2 &viewport_size, GLuint default_fbo) override;

    std::string_view get_name() const override {
        return "FSR";
    }
};

} // namespace renderer::gl
