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

#include <renderer/software/state.h>
#include <renderer/software/types.h>

struct Config;
struct FeatureState;
struct MemState;
struct SceGxmBlendInfo;
struct SceGxmProgram;
struct SceGxmRenderTargetParams;

namespace renderer::software {

bool create(std::unique_ptr<renderer::State> &state, const Config &config);
bool create(SWState &state, std::unique_ptr<Context> &context, MemState &mem);
bool create(SWState &state, std::unique_ptr<RenderTarget> &rt, const SceGxmRenderTargetParams &params,
    const FeatureState &features);
void create(std::unique_ptr<FragmentProgram> &fp, SWState &state, const SceGxmProgram &program,
    const SceGxmBlendInfo *blend);
void create(std::unique_ptr<VertexProgram> &vp, SWState &state, const SceGxmProgram &program);

void destroy(SWState &state, std::unique_ptr<RenderTarget> &rt);

// Scene
// The surfaces are deliberately not passed in: handle_set_context has already
// copied them into the record and freed the command payload by the time a
// backend is called, so the record is the only place they can be read from.
void set_context(SWState &state, SWContext &context, MemState &mem, SWRenderTarget *rt);
void mid_scene_flush(SWContext &context, const SceGxmNotification notification);

void draw(SWState &state, SWContext &context, SceGxmPrimitiveType type, SceGxmIndexFormat format,
    const void *indices, size_t count, uint32_t instance_count, MemState &mem, const Config &config);

// Copies one uniform block into the flat storage the recompiler addresses as a
// single buffer container.
bool set_uniform_buffer(SWContext &context, const ShaderProgram *program, bool is_vertex, int block_num,
    uint32_t size, const uint8_t *data);
void set_texture(SWContext &context, uint32_t index, const SceGxmTexture &texture);
void sync_viewport_real(SWContext &context, float xOffset, float yOffset, float xScale, float yScale);

// The transfer commands need no backend of their own: renderer/src/transfer.cpp
// already moves the pixels around in guest memory, which is exactly where this
// renderer keeps its surfaces.

} // namespace renderer::software
