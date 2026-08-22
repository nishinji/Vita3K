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

#include <renderer/d3d12/common.h>
#include <renderer/d3d12/resource.h>
#include <renderer/d3d12/surface_cache.h>

#include <array>

struct MemState;
struct DisplayFrameInfo;

namespace renderer::d3d12 {

struct DXState;

// Composites the Vita image onto the swapchain.
//
// The source is currently read straight out of guest memory and uploaded each
// frame. Once the surface cache lands it will hand over the colour surface that
// was already rendered on the GPU, and this upload becomes the fallback for the
// case where the frame was never drawn through a render target.
class ScreenRenderer {
public:
    DXState &state;

    explicit ScreenRenderer(DXState &state);

    bool create();
    void destroy();

    // Blit a colour surface the GPU already rendered to. `source` selects the
    // sub-rectangle of that surface holding the frame, in its own (upscaled)
    // pixels. This is the normal path: the guest-memory copy of a surface is
    // never written back, so reading it would present an empty frame.
    bool render_surface(ID3D12GraphicsCommandList *cmd_list, BarrierBatcher &barriers,
        Resource &image, DescriptorHandle image_srv, const Viewport &source, const D3D12_VIEWPORT &viewport);

    // Fallback for frames that were never rendered through a tracked render
    // target: upload the guest framebuffer and blit that.
    bool render(ID3D12GraphicsCommandList *cmd_list, BarrierBatcher &barriers,
        const DisplayFrameInfo &frame, MemState &mem, const D3D12_VIEWPORT &viewport);

    // Whether sampling uses linear filtering; driven by the screen filter setting.
    bool use_linear_filter = true;

private:
    // Per-frame-in-flight source texture, so an upload never overwrites the one
    // the GPU is still reading.
    struct SourceTexture {
        Resource image;
        DescriptorHandle srv;
        uint32_t width = 0;
        uint32_t height = 0;
    };

    // Recreate the texture when the display resolution changes.
    bool ensure_source_texture(SourceTexture &texture, uint32_t width, uint32_t height);

    // Bind the pipeline and issue the fullscreen triangle. `uv_scale` and
    // `uv_offset` select which part of the source texture is sampled.
    bool draw_fullscreen(ID3D12GraphicsCommandList *cmd_list, DescriptorHandle source_srv,
        const float uv_scale[2], const float uv_offset[2], const D3D12_VIEWPORT &viewport);

    ComPtr<ID3D12RootSignature> root_sig;
    ComPtr<ID3D12PipelineState> pipeline;

    // Two entries: index 0 point, index 1 linear.
    DescriptorHeap sampler_heap;

    std::array<SourceTexture, MAX_FRAMES_RENDERING> source_textures;
};

} // namespace renderer::d3d12
