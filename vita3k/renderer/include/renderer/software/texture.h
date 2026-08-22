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

#include <renderer/software/common.h>
#include <renderer/texture_cache.h>

#include <array>
#include <vector>

namespace renderer::software {

struct SWState;

// One mip level of a cached texture, always decoded to linear RGBA float so that
// the sampler never has to know about the original GXM format.
struct SWTextureLevel {
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<float> pixels;
};

struct SWTexture {
    // Index 0 is the base level; cube faces are stored back to back, six mip
    // chains in a row, which is how the base cache uploads them.
    std::vector<SWTextureLevel> levels;
    uint32_t face_count = 1;
    bool is_srgb = false;
    bool valid = false;
};

// Sampler state resolved out of a SceGxmTexture, paired with the decoded pixels.
struct SWSampler {
    const SWTexture *texture = nullptr;
    SceGxmTextureAddrMode uaddr = SCE_GXM_TEXTURE_ADDR_REPEAT;
    SceGxmTextureAddrMode vaddr = SCE_GXM_TEXTURE_ADDR_REPEAT;
    SceGxmTextureFilter min_filter = SCE_GXM_TEXTURE_FILTER_LINEAR;
    SceGxmTextureFilter mag_filter = SCE_GXM_TEXTURE_FILTER_LINEAR;
    SceGxmTextureMipFilter mip_filter = SCE_GXM_TEXTURE_MIP_FILTER_DISABLED;
    float lod_bias = 0.0f;
    uint32_t lod_min = 0;
    bool is_cube = false;

    // Samples the texture, returning linear RGBA. `lod` is ignored unless the
    // texture actually has mips.
    Vec4f sample(float u, float v, float lod) const;
};

class SWTextureCache : public renderer::TextureCache {
public:
    explicit SWTextureCache(SWState &state);

    void select(size_t index, const SceGxmTexture &texture) override;
    void configure_texture(const SceGxmTexture &texture) override;
    void upload_texture_impl(SceGxmTextureBaseFormat base_format, uint32_t width, uint32_t height,
        uint32_t mip_index, const void *pixels, int face, uint32_t pixels_per_stride) override;
    void import_configure_impl(SceGxmTextureBaseFormat base_format, uint32_t width, uint32_t height,
        bool is_srgb, uint16_t nb_components, uint16_t mipcount, bool swap_rb) override;

    // Binds the texture that was just cached to the given shader slot. The draw
    // path calls this once per texture unit before running any invocation.
    void bind(bool is_vertex, uint32_t slot, const SceGxmTexture &gxm_texture, MemState &mem);

    const SWSampler *bound_sampler(bool is_vertex, uint32_t slot) const;

    void reset_bindings();
    void cleanup();

private:
    SWTexture &current_texture();

    SWState &m_state;

    std::array<SWTexture, TextureCacheSize> m_textures;
    size_t m_current_index = 0;
    // Full format of the texture being cached, captured in select() because the
    // upload callback is only handed the base format and the swizzle lives in
    // the upper bits.
    SceGxmTextureFormat m_current_format = SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ABGR;

    std::array<SWSampler, SCE_GXM_MAX_TEXTURE_UNITS> m_vertex_samplers;
    std::array<SWSampler, SCE_GXM_MAX_TEXTURE_UNITS> m_fragment_samplers;

    // Set while a replacement texture is being imported, so the upload path
    // knows the component count it is being handed.
    uint16_t m_import_components = 4;
    bool m_import_swap_rb = false;
};

} // namespace renderer::software
