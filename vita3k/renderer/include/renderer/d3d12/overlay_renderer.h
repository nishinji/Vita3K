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

#include <overlay/compiled_resource.h>
#include <overlay/controls.h>
#include <overlay/display_manager.h>

#include <memory>
#include <unordered_map>
#include <vector>

namespace renderer::d3d12 {

struct DXState;

// Mirrors the push constant block the vulkan overlay shader takes. In D3D12
// this is a root constant block: 32 DWORDs, which fits comfortably inside the
// 64-DWORD root signature budget.
struct OverlayRootConstants {
    float ui_scale[4];
    float albedo[4];
    float viewport[4];
    float clip_bounds[4];
    uint32_t vertex_config;
    uint32_t fragment_config;
    float timestamp;
    float blur_intensity;
    float sdf_params[4];
    float sdf_origin[4];
    float sdf_border_color[4];
};
static_assert(sizeof(OverlayRootConstants) == 128);

// Draws the retained-mode overlay elements (the shader precompile/loading
// screen, notifications, the pause menu and the common dialogs) on top of the
// presented frame.
//
// The vulkan backend runs the same overlay through a hand-written GLSL shader
// pair; this is the HLSL port of it, kept line-for-line equivalent so both
// backends produce the same picture.
class OverlayRenderer {
public:
    OverlayRenderer() = default;
    ~OverlayRenderer();

    bool init(DXState &state);
    void destroy();

    // Compile every visible view and upload the fonts and images they reference.
    // Recorded into `cmd_list` ahead of the frame's draws.
    void prepare(ID3D12GraphicsCommandList *cmd_list, BarrierBatcher &barriers,
        const overlay::display_manager &manager, float viewport_w, float viewport_h);

    // Issue the prepared draw commands. `viewport_*` is the letterboxed area the
    // Vita image occupies, which is the space the overlay lays itself out in.
    void render(ID3D12GraphicsCommandList *cmd_list,
        float viewport_x, float viewport_y, float viewport_w, float viewport_h);

    // Tag everything staged this frame with the fence the frame will signal.
    void retire(uint64_t fence_value);

    bool ready() const {
        return pipelines[0] != nullptr;
    }

private:
    // A texture owned by the overlay: either a font atlas or a decoded image.
    struct OverlayTexture {
        Resource image;
        DescriptorHandle srv;
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t layers = 1;
        uint32_t mip_levels = 1;
        DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    };

    DXState *state = nullptr;

    ComPtr<ID3D12RootSignature> root_sig;
    // Index 0 draws triangles (quad lists, strips and expanded fans), index 1
    // draws lines. D3D12 bakes only the topology *class* into the pipeline, so
    // strip versus list is picked on the command list.
    ComPtr<ID3D12PipelineState> pipelines[2];

    // Vertex data and texture staging for the frames still in flight. Retired
    // against the frame fence, so a region is never overwritten while read.
    UploadRingBuffer upload;

    // Keyed on the overlay-side object, which is what the compiled commands
    // reference. Fonts and images are uploaded once and reused.
    std::unordered_map<const overlay::font *, OverlayTexture> font_atlases;
    std::unordered_map<const void *, OverlayTexture> raw_images;
    std::vector<OverlayTexture> icons;

    // The shader always samples both t0 and t1, so the slot a command does not
    // use gets a view onto the shared 1x1 default texture rather than a null
    // descriptor, which would be undefined behaviour inside a table.
    DescriptorHandle dummy_array_srv;

    overlay::resource_config resources;
    bool resources_loaded = false;

    struct PreparedView {
        std::shared_ptr<overlay::overlay> view;
        overlay::compiled_resource compiled;
    };
    std::vector<PreparedView> prepared_views;

    // Images bigger than this get a mip chain; below it the extra levels buy
    // nothing and cost an upload.
    static constexpr uint32_t MIPMAP_SIZE_THRESHOLD = 256;

    bool create_root_signature();
    bool create_pipelines();

    // Allocate `layers * mip_levels` subresources worth of texture and its SRV.
    bool create_texture(OverlayTexture &texture, DXGI_FORMAT format,
        uint32_t width, uint32_t height, uint32_t layers, uint32_t mip_levels, bool array_view);
    void release_texture(OverlayTexture &texture);

    // Copy one subresource through the upload ring, repacking rows to the
    // 256-byte pitch D3D12 requires of a placed footprint.
    bool upload_subresource(ID3D12GraphicsCommandList *cmd_list, OverlayTexture &texture,
        uint32_t subresource, const uint8_t *data, uint32_t width, uint32_t height, uint32_t bytes_per_pixel);

    void upload_font(ID3D12GraphicsCommandList *cmd_list, BarrierBatcher &barriers, const overlay::font *font);
    void upload_image(ID3D12GraphicsCommandList *cmd_list, BarrierBatcher &barriers,
        const overlay::image_info_base *info, OverlayTexture &destination);

    void draw_command(ID3D12GraphicsCommandList *cmd_list,
        const overlay::compiled_resource::command &draw_cmd,
        float viewport_x, float viewport_y, float viewport_w, float viewport_h);
};

} // namespace renderer::d3d12
