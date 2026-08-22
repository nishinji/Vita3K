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

#include <renderer/d3d12/state.h>

struct Config;
struct MemState;
struct FeatureState;

namespace renderer::d3d12 {

// Entry point used by renderer::init.
bool create(std::unique_ptr<renderer::State> &state, const Config &config);

bool create(DXState &state, std::unique_ptr<Context> &context, MemState &mem);
bool create(DXState &state, std::unique_ptr<RenderTarget> &rt, const SceGxmRenderTargetParams &params, const FeatureState &features);
void destroy(DXState &state, std::unique_ptr<RenderTarget> &rt);

bool create(std::unique_ptr<VertexProgram> &vp, DXState &state, const SceGxmProgram &program);
bool create(std::unique_ptr<FragmentProgram> &fp, DXState &state, const SceGxmProgram &program, const SceGxmBlendInfo *blend);

// Scene

void set_context(DXContext &context, MemState &mem, DXRenderTarget *rt, const FeatureState &features);
void new_frame(DXContext &context);
void mid_scene_flush(DXContext &context, const SceGxmNotification notification);
void signal_sync_object(DXState &state, SceGxmSyncObject *sync_object, uint32_t timestamp);

void set_uniform_buffer(DXContext &context, MemState &mem, const ShaderProgram *program, const bool vertex_shader,
    const int block_num, const int size, Ptr<uint8_t> data);

void draw(DXContext &context, SceGxmPrimitiveType type, SceGxmIndexFormat format,
    Ptr<void> indices, size_t count, uint32_t instance_count, MemState &mem, const Config &config);

// Dynamic state

void sync_clipping(DXContext &context);
void sync_stencil_func(DXContext &context, const bool is_back);
void sync_depth_bias(DXContext &context);
void sync_depth_data(DXContext &context);
void sync_stencil_data(DXContext &context, const MemState &mem);
void sync_point_line_width(DXContext &context, const bool is_front);
void sync_texture(DXContext &context, MemState &mem, std::size_t index, SceGxmTexture texture, const Config &config);
void sync_viewport_flat(DXContext &context);
void sync_viewport_real(DXContext &context, const float xOffset, const float yOffset, const float zOffset,
    const float xScale, const float yScale, const float zScale);
void sync_visibility_buffer(DXContext &context, Ptr<uint32_t> buffer, uint32_t stride);
void sync_visibility_index(DXContext &context, bool enable, uint32_t index, bool is_increment);

void refresh_pipeline(DXContext &context);

} // namespace renderer::d3d12
