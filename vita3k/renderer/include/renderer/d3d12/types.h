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
#include <renderer/texture_cache.h>
#include <renderer/types.h>
#include <shader/uniform_block.h>

#include <array>
#include <map>
#include <unordered_map>
#include <vector>

struct MemState;

namespace renderer::d3d12 {

struct DXState;
struct ColorSurfaceCacheInfo;

// Per-frame-in-flight resources. The CPU records into frame N while the GPU is
// still consuming N-1 and N-2, so anything written during recording needs one
// copy per frame slot.
struct FrameObject {
    ComPtr<ID3D12CommandAllocator> render_allocator;
    // Separate allocator for texture/buffer uploads recorded outside the render
    // pass, matching the prerender pool split the vulkan backend uses.
    ComPtr<ID3D12CommandAllocator> prerender_allocator;

    // Command list recorded into this slot. D3D12 does not keep a submitted
    // command list alive, so it is owned per frame and reset rather than
    // recreated, which would risk releasing one still executing.
    ComPtr<ID3D12GraphicsCommandList> render_cmd_list;

    // Fence value signalled on the direct queue once this frame is done. The
    // slot may not be reused until the fence has reached it.
    uint64_t fence_value = 0;

    ShaderVisibleDescriptorRing view_descriptors;
    ShaderVisibleDescriptorRing sampler_descriptors;

    UploadRingBuffer upload_buffer;

    // Descriptor tables already built this frame, keyed on the set of descriptors
    // they hold. Games bind the same handful of materials over and over, so
    // reusing the table keeps the shader-visible heaps from filling up -- the
    // sampler heap is capped at 2048 descriptors by D3D12 and cannot be grown.
    std::unordered_map<uint64_t, DescriptorHandle> view_table_cache;
    std::unordered_map<uint64_t, DescriptorHandle> sampler_table_cache;

    // Resources whose last use was in this frame; released when the slot is
    // recycled, which is the point the GPU is known to be done with them.
    std::vector<ComPtr<ID3D12Resource>> destroy_queue;
};

struct DXRenderTarget : public renderer::RenderTarget {
    uint16_t width = 0;
    uint16_t height = 0;

    // Scaled by res_multiplier; this is the size actually allocated on the GPU.
    uint16_t scaled_width = 0;
    uint16_t scaled_height = 0;

    // GXM depth/stencil lives on chip, so scenes without a backed surface still test against this.
    static constexpr DXGI_FORMAT depth_stencil_format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
    Resource depth_stencil;
    DescriptorHandle dsv;
};

struct TextureCacheEntry {
    Resource image;
    // Long-lived CPU descriptor; copied into the frame ring when bound.
    DescriptorHandle srv;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    uint16_t mip_count = 1;
    bool is_cube = false;
    uint32_t memory_needed = 0;
};

// Texture cache backed by D3D12 committed resources.
//
// The cache-wide bookkeeping (lookup, LRU, replacement textures) stays in
// renderer::TextureCache; what lives here is resource creation, the staging
// upload and the sampler descriptors.
struct DXTextureCache : public renderer::TextureCache {
    DXState &state;

    std::array<TextureCacheEntry, TextureCacheSize> textures;

    // Samplers are created in a non-shader-visible heap and copied into the
    // per-frame ring at bind time, mirroring how the SRVs are handled.
    StagingDescriptorAllocator sampler_descriptors;
    std::vector<DescriptorHandle> samplers;

    // Staging memory for uploads. Separate from the per-frame upload ring
    // because textures are also uploaded outside of a frame.
    UploadRingBuffer staging_buffer;

    // Bumped by set_context; lets the cache tell whether an entry was touched
    // by the scene currently being recorded.
    uint64_t current_scene_timestamp = 0;

    TextureCacheEntry *current_texture = nullptr;
    // Set by the draw path before textures are configured; uploads are recorded
    // here so they complete before the draw that samples them.
    ID3D12GraphicsCommandList *cmd_list = nullptr;

    explicit DXTextureCache(DXState &state);

    bool init(bool hashless_texture_cache, const fs::path &texture_folder, const std::string_view game_id);

    void select(size_t index, const SceGxmTexture &texture) override;
    void configure_texture(const SceGxmTexture &texture) override;
    void upload_texture_impl(SceGxmTextureBaseFormat base_format, uint32_t width, uint32_t height,
        uint32_t mip_index, const void *pixels, int face, uint32_t pixels_per_stride) override;
    void upload_done() override;
    void configure_sampler(size_t index, const SceGxmTexture &texture, bool no_linear) override;
    void import_configure_impl(SceGxmTextureBaseFormat base_format, uint32_t width, uint32_t height,
        bool is_srgb, uint16_t nb_components, uint16_t mipcount, bool swap_rb) override;

    DescriptorHandle get_retrieved_sampler() const {
        return samplers.empty() ? DescriptorHandle{} : samplers[last_bound_sampler_index];
    }

    void cleanup();
};

struct DXVertexProgram : public renderer::VertexProgram {
};

struct DXFragmentProgram : public renderer::FragmentProgram {
    D3D12_RENDER_TARGET_BLEND_DESC blending{};
    // Folded into the pipeline key, exactly as the vulkan backend does.
    uint64_t blending_hash = 0;
};

// Occlusion query state for one guest visibility buffer.
struct VisibilityBuffer {
    Address address = 0;
    ComPtr<ID3D12QueryHeap> query_heap;
    // Readback destination the resolved results land in.
    ComPtr<ID3D12Resource> result_buffer;
    uint32_t size = 0;
    std::vector<bool> queries_used;
};

struct DXContext : public renderer::Context {
    DXState &state;
    MemState &mem;

    // A command allocator may not be reset, and a command list may not be reset
    // onto it, while anything recorded from it is still executing. Several
    // scenes can be submitted within one frame, so the context cycles through a
    // pool rather than reusing the frame allocator directly.
    struct CommandSlot {
        ComPtr<ID3D12CommandAllocator> render_allocator;
        ComPtr<ID3D12CommandAllocator> prerender_allocator;
        ComPtr<ID3D12GraphicsCommandList> render_cmd_list;
        ComPtr<ID3D12GraphicsCommandList> prerender_cmd_list;
        // Value the submission using this slot signals; 0 means never used.
        uint64_t fence_value = 0;
    };

    static constexpr uint32_t COMMAND_SLOT_COUNT = 8;
    std::array<CommandSlot, COMMAND_SLOT_COUNT> command_slots;
    uint32_t current_slot_idx = 0;

    // Aliases of the current slot, so the recording code reads naturally.
    ID3D12GraphicsCommandList *render_cmd_list = nullptr;
    // Uploads that must land before the render list runs. D3D12 has no
    // equivalent of recording a copy inside a render pass, so these are kept
    // apart and submitted first.
    ID3D12GraphicsCommandList *prerender_cmd_list = nullptr;

    BarrierBatcher barriers;

    uint64_t scene_timestamp = 0;
    uint64_t frame_timestamp = 1;

    bool is_recording = false;
    bool in_render_pass = false;
    bool refresh_pipeline = false;
    bool is_first_scene_draw = false;

    // Per-scene streaming memory. Sized to hold a whole scene, reclaimed once
    // the frame fence has passed.
    UploadRingBuffer vertex_stream_buffer;
    UploadRingBuffer index_stream_buffer;
    UploadRingBuffer vertex_uniform_buffer;
    UploadRingBuffer fragment_uniform_buffer;
    UploadRingBuffer vertex_info_buffer;
    UploadRingBuffer fragment_info_buffer;

    // The region each ring buffer last handed out. The GPU address is what the
    // root descriptors point at; the CPU pointer is where uniforms are written.
    UploadRingBuffer::Allocation vertex_uniform_alloc;
    UploadRingBuffer::Allocation fragment_uniform_alloc;
    UploadRingBuffer::Allocation vertex_info_alloc;
    UploadRingBuffer::Allocation fragment_info_alloc;

    bool vertex_uniform_storage_allocated = false;
    bool fragment_uniform_storage_allocated = false;

    // Bound textures, as CPU descriptors copied into the frame ring at draw time.
    DescriptorHandle vertex_textures[SCE_GXM_MAX_TEXTURE_UNITS] = {};
    DescriptorHandle vertex_samplers[SCE_GXM_MAX_TEXTURE_UNITS] = {};
    DescriptorHandle fragment_textures[SCE_GXM_MAX_TEXTURE_UNITS] = {};
    DescriptorHandle fragment_samplers[SCE_GXM_MAX_TEXTURE_UNITS] = {};

    // Set whenever a bound texture or sampler changes, so the descriptor tables
    // are only rebuilt when they actually differ. The shader-visible sampler
    // heap holds at most 2048 descriptors, so a table per draw would run out
    // after a few dozen draws.
    bool textures_dirty = true;
    DescriptorHandle cached_vertex_texture_table;
    DescriptorHandle cached_vertex_sampler_table;
    DescriptorHandle cached_fragment_texture_table;
    DescriptorHandle cached_fragment_sampler_table;
    DescriptorHandle cached_attachment_table;
    // Framebuffer fetch snapshot the attachment table points at.
    DescriptorHandle fetch_srv;

    D3D12_VERTEX_BUFFER_VIEW vertex_stream_views[SCE_GXM_MAX_VERTEX_STREAMS] = {};

    shader::RenderVertUniformBlock prev_vert_ublock;
    shader::RenderFragUniformBlock prev_frag_ublock;
    shader::RenderVertUniformBlockExtended curr_vert_ublock;
    shader::RenderFragUniformBlockExtended curr_frag_ublock;

    // Scratch used to build the uniform block before it is copied out.
    uint8_t shader_info_temp[std::max(shader::RenderVertUniformBlockExtended::get_max_size(),
        shader::RenderFragUniformBlockExtended::get_max_size())];

    std::map<Address, VisibilityBuffer> visibility_buffers;
    VisibilityBuffer *current_visibility_buffer = nullptr;
    int current_query_idx = -1;
    bool is_in_query = false;
    bool is_query_op_increment = false;

    DXRenderTarget *render_target = nullptr;

    D3D12_VIEWPORT viewport{};
    D3D12_RECT scissor{};
    SceGxmPrimitiveType last_primitive = SCE_GXM_PRIMITIVE_TRIANGLES;
    ID3D12PipelineState *current_pipeline = nullptr;

    // Render targets currently bound. A D3D12 PSO bakes the formats in, so these
    // feed the pipeline key too.
    Resource *current_color_image = nullptr;
    ColorSurfaceCacheInfo *current_color_surface = nullptr;
    Resource *current_depth_image = nullptr;
    D3D12_CPU_DESCRIPTOR_HANDLE current_rtv{};
    D3D12_CPU_DESCRIPTOR_HANDLE current_dsv{};
    DXGI_FORMAT current_color_format = DXGI_FORMAT_UNKNOWN;
    DXGI_FORMAT current_depth_format = DXGI_FORMAT_UNKNOWN;
    uint32_t current_sample_count = 1;

    // Fence value the scene last submitted will signal.
    uint64_t last_submit_fence = 0;

    DXContext(DXState &state, MemState &mem);
    ~DXContext() override;

    bool create_resources();

    void start_recording();
    // Submit the recorded lists and signal the notifications once the GPU is done.
    void stop_recording(const SceGxmNotification &notification1, const SceGxmNotification &notification2, bool submit);

    void start_render_pass();
    void stop_render_pass();
};

} // namespace renderer::d3d12
