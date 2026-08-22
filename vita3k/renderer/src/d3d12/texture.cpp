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

#include <renderer/d3d12/gxm_to_d3d12.h>
#include <renderer/d3d12/state.h>
#include <renderer/d3d12/types.h>

#include <gxm/functions.h>
#include <renderer/functions.h>
#include <renderer/texture_cache.h>
#include <util/align.h>
#include <util/log.h>

#include <algorithm>

namespace renderer::d3d12 {

// Big enough for several large textures in flight; reclaimed against the frame
// fence like every other upload allocation.
constexpr uint64_t TEXTURE_STAGING_BUFFER_SIZE = 128 * 1024 * 1024;

// Only a handful of DXGI formats have an sRGB twin, which is exactly the set
// GXM can ask for gamma correction on.
static DXGI_FORMAT linear_to_srgb(DXGI_FORMAT format) {
    switch (format) {
    case DXGI_FORMAT_R8G8B8A8_UNORM:
        return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    case DXGI_FORMAT_BC1_UNORM:
        return DXGI_FORMAT_BC1_UNORM_SRGB;
    case DXGI_FORMAT_BC2_UNORM:
        return DXGI_FORMAT_BC2_UNORM_SRGB;
    case DXGI_FORMAT_BC3_UNORM:
        return DXGI_FORMAT_BC3_UNORM_SRGB;
    case DXGI_FORMAT_BC7_UNORM:
        return DXGI_FORMAT_BC7_UNORM_SRGB;
    default:
        // DXGI has no single- or two-channel sRGB formats, unlike vulkan.
        LOG_WARN_ONCE("D3D12: gamma correction requested on a format without an sRGB variant");
        return format;
    }
}

// Bytes occupied by one row of blocks (or pixels, for uncompressed formats).
static uint32_t row_size_in_bytes(SceGxmTextureBaseFormat base_format, uint32_t width_in_pixels) {
    if (gxm::is_bcn_format(base_format) || renderer::texture::is_astc_format(base_format)) {
        const auto [block_width, block_height] = gxm::get_block_size(base_format);
        const uint32_t blocks = (width_in_pixels + block_width - 1) / block_width;
        // get_compressed_size over a single block row gives the bytes per block
        // without having to special-case each format here.
        return blocks * renderer::texture::get_compressed_size(base_format, block_width, block_height);
    }

    const size_t bits_per_pixel = gxm::bits_per_pixel(base_format);
    return static_cast<uint32_t>((static_cast<uint64_t>(width_in_pixels) * bits_per_pixel + 7) / 8);
}

// Number of rows a copy covers: block rows for compressed formats, pixel rows
// otherwise.
static uint32_t row_count(SceGxmTextureBaseFormat base_format, uint32_t height_in_pixels) {
    if (gxm::is_bcn_format(base_format) || renderer::texture::is_astc_format(base_format)) {
        const uint32_t block_height = gxm::get_block_size(base_format).second;
        return (height_in_pixels + block_height - 1) / block_height;
    }

    return height_in_pixels;
}

// Add an alpha channel to u8u8u8 textures. DXGI has no three-channel 8-bit
// format, so the data has to be widened before upload.
static void *add_alpha_channel(const void *pixels, uint32_t width, uint32_t height, std::vector<uint8_t> &data) {
    data.resize(static_cast<size_t>(width) * height * 4);

    const uint8_t *src = static_cast<const uint8_t *>(pixels);
    uint8_t *dst = data.data();
    for (uint32_t y = 0; y < height; y++) {
        for (uint32_t x = 0; x < width; x++) {
            dst[0] = src[0];
            dst[1] = src[1];
            dst[2] = src[2];
            dst[3] = 255;

            src += 3;
            dst += 4;
        }
    }

    return data.data();
}

DXTextureCache::DXTextureCache(DXState &state)
    : state(state) {
    backend = Backend::DirectX12;
    // Every feature level 11_0 device supports BC1-BC7, so GXM DXT textures go
    // up uncompressed-free.
    support_dxt = true;
    // ASTC and PVRTC have no DXGI equivalent; the shared cache decompresses them.
    support_astc = false;
    support_pvrt = false;
    support_x8d24 = false;
    support_e5rgb9 = true;
    support_a2rgb10 = true;
    use_sampler_cache = true;
}

bool DXTextureCache::init(bool hashless_texture_cache, const fs::path &texture_folder, const std::string_view game_id) {
    if (!sampler_descriptors.create(state.device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER,
            D3D12_MAX_SHADER_VISIBLE_SAMPLER_HEAP_SIZE, L"Vita3K sampler staging heap"))
        return false;

    if (!staging_buffer.create(state.device.Get(), TEXTURE_STAGING_BUFFER_SIZE,
            D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT, L"Vita3K texture staging buffer"))
        return false;

    // get_retrieved_sampler indexes this directly, so it has to be sized up
    // front to match the shared sampler cache.
    samplers.resize(D3D12_MAX_SHADER_VISIBLE_SAMPLER_HEAP_SIZE);

    return renderer::TextureCache::init(hashless_texture_cache, texture_folder, game_id,
        D3D12_MAX_SHADER_VISIBLE_SAMPLER_HEAP_SIZE);
}

void DXTextureCache::select(size_t index, const SceGxmTexture &texture) {
    current_texture = &textures[index];
}

void DXTextureCache::configure_texture(const SceGxmTexture &gxm_texture) {
    const SceGxmTextureFormat format = gxm::get_format(gxm_texture);
    const SceGxmTextureBaseFormat base_format = gxm::get_base_format(format);

    const bool is_cube = gxm_texture.texture_type() == SCE_GXM_TEXTURE_CUBE
        || gxm_texture.texture_type() == SCE_GXM_TEXTURE_CUBE_ARBITRARY;

    const uint32_t width = gxm::get_width(gxm_texture);
    const uint32_t height = gxm::get_height(gxm_texture);
    const uint16_t mip_count = renderer::texture::get_upload_mip(gxm_texture.true_mip_count(), width, height);

    DXGI_FORMAT dxgi_format = texture::translate_format(base_format);
    if (dxgi_format == DXGI_FORMAT_UNKNOWN) {
        LOG_ERROR_ONCE("D3D12: refusing to create a texture with an unsupported format");
        return;
    }

    if (gxm_texture.gamma_mode)
        dxgi_format = linear_to_srgb(dxgi_format);

    // Upper bound on the memory the entry occupies, used by the LRU accounting.
    uint32_t memory_needed = row_size_in_bytes(base_format, std::max(next_power_of_two(width), 8u))
        * row_count(base_format, std::max(next_power_of_two(height), 8u));
    if (mip_count > 1)
        // With mips the total is 4/3 of the base; round up to 3/2.
        memory_needed += memory_needed / 2;
    if (is_cube)
        memory_needed *= 6;

    current_texture->memory_needed = align(memory_needed, 16);
    current_texture->mip_count = mip_count;
    current_texture->is_cube = is_cube;
    current_texture->format = dxgi_format;

    // The previous resource may still be in flight, so it goes on the frame
    // destroy queue rather than being released outright.
    if (current_texture->image)
        state.frame().destroy_queue.push_back(current_texture->image.resource);

    current_texture->image.reset();

    const D3D12_HEAP_PROPERTIES heap_props{
        .Type = D3D12_HEAP_TYPE_DEFAULT,
        .CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN,
        .MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN,
        .CreationNodeMask = 1,
        .VisibleNodeMask = 1,
    };

    const D3D12_RESOURCE_DESC desc{
        .Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D,
        .Alignment = 0,
        .Width = width,
        .Height = height,
        // Cube maps are six array slices with a cube SRV over them.
        .DepthOrArraySize = static_cast<UINT16>(is_cube ? 6 : 1),
        .MipLevels = mip_count,
        .Format = dxgi_format,
        .SampleDesc = { .Count = 1, .Quality = 0 },
        .Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN,
        .Flags = D3D12_RESOURCE_FLAG_NONE,
    };

    if (!dx_check(state.device->CreateCommittedResource(&heap_props, D3D12_HEAP_FLAG_NONE, &desc,
                      D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&current_texture->image.resource)),
            "creating a texture"))
        return;

    current_texture->image.state = D3D12_RESOURCE_STATE_COPY_DEST;

    if (!current_texture->srv.valid()) {
        current_texture->srv = state.srv_staging_heap.allocate();
        if (!current_texture->srv.valid())
            return;
    }

    D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc{};
    srv_desc.Format = dxgi_format;
    srv_desc.Shader4ComponentMapping = texture::translate_swizzle(format);

    if (is_cube) {
        srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
        srv_desc.TextureCube = { .MostDetailedMip = 0, .MipLevels = mip_count, .ResourceMinLODClamp = 0.0f };
    } else {
        srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv_desc.Texture2D = { .MostDetailedMip = 0, .MipLevels = mip_count, .PlaneSlice = 0, .ResourceMinLODClamp = 0.0f };
    }

    state.device->CreateShaderResourceView(current_texture->image.get(), &srv_desc, current_texture->srv.cpu);
}

void DXTextureCache::upload_texture_impl(SceGxmTextureBaseFormat base_format, uint32_t width, uint32_t height,
    uint32_t mip_index, const void *pixels, int face, uint32_t pixels_per_stride) {
    if (!current_texture || !current_texture->image || !cmd_list)
        return;

    // Faces are 1-based when set; array slice 0 is the first face.
    if (face > 0)
        face--;

    if (pixels_per_stride == 0)
        pixels_per_stride = width;

    const void *texture_data = pixels;
    std::vector<uint8_t> temp_data;
    if (base_format == SCE_GXM_TEXTURE_BASE_FORMAT_U8U8U8 || base_format == SCE_GXM_TEXTURE_BASE_FORMAT_S8S8S8) {
        // No three-channel 8-bit DXGI format, so an alpha channel is inserted.
        texture_data = add_alpha_channel(pixels, pixels_per_stride, height, temp_data);
        base_format = (base_format == SCE_GXM_TEXTURE_BASE_FORMAT_U8U8U8)
            ? SCE_GXM_TEXTURE_BASE_FORMAT_U8U8U8U8
            : SCE_GXM_TEXTURE_BASE_FORMAT_S8S8S8S8;
    }

    // The source is packed at the GXM stride; D3D12 wants each row of the copy
    // to start on a 256-byte boundary, so the rows are repacked rather than
    // memcpy'd in one block.
    const uint32_t source_row_pitch = row_size_in_bytes(base_format, pixels_per_stride);
    const uint32_t copy_row_bytes = row_size_in_bytes(base_format, width);
    const uint32_t rows = row_count(base_format, height);
    const uint32_t upload_pitch = align(copy_row_bytes, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);
    const uint64_t upload_size = static_cast<uint64_t>(upload_pitch) * rows;

    // A cached texture that is being re-uploaded is still in its shader-resource
    // state from the last time it was sampled, and a copy destination has to be
    // in COPY_DEST. Only a freshly created resource is already in that state.
    BarrierBatcher barriers;
    barriers.transition(current_texture->image, D3D12_RESOURCE_STATE_COPY_DEST);
    barriers.flush(cmd_list);

    // Everything submitted so far on the frame fence is safe to overwrite.
    staging_buffer.reclaim(state.completed_fence_value());

    UploadRingBuffer::Allocation allocation = staging_buffer.allocate(upload_size);
    if (!allocation.valid()) {
        LOG_ERROR("D3D12: the texture staging buffer has no room for a {} byte upload", upload_size);
        return;
    }

    const uint8_t *source = static_cast<const uint8_t *>(texture_data);
    for (uint32_t row = 0; row < rows; row++) {
        memcpy(allocation.cpu + static_cast<uint64_t>(row) * upload_pitch,
            source + static_cast<uint64_t>(row) * source_row_pitch,
            copy_row_bytes);
    }

    // Subresources are indexed mip-major within each array slice.
    const UINT subresource = mip_index + static_cast<uint32_t>(face) * current_texture->mip_count;

    const D3D12_TEXTURE_COPY_LOCATION destination{
        .pResource = current_texture->image.get(),
        .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,
        .SubresourceIndex = subresource,
    };

    D3D12_TEXTURE_COPY_LOCATION source_location{};
    source_location.pResource = allocation.resource;
    source_location.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    source_location.PlacedFootprint.Offset = allocation.offset;
    source_location.PlacedFootprint.Footprint = D3D12_SUBRESOURCE_FOOTPRINT{
        .Format = current_texture->format,
        .Width = width,
        // A compressed footprint is still described in pixels, but both
        // dimensions have to be a whole number of blocks.
        .Height = height,
        .Depth = 1,
        .RowPitch = upload_pitch,
    };

    if (gxm::is_bcn_format(base_format) || renderer::texture::is_astc_format(base_format)) {
        const auto [block_width, block_height] = gxm::get_block_size(base_format);
        source_location.PlacedFootprint.Footprint.Width = (width + block_width - 1) / block_width * block_width;
        source_location.PlacedFootprint.Footprint.Height = (height + block_height - 1) / block_height * block_height;
    }

    cmd_list->CopyTextureRegion(&destination, 0, 0, 0, &source_location, nullptr);
}

void DXTextureCache::upload_done() {
    if (!current_texture || !current_texture->image || !cmd_list)
        return;

    // Sampling may happen from either stage, so the transition has to cover both.
    BarrierBatcher barriers;
    barriers.transition(current_texture->image,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    barriers.flush(cmd_list);
}

void DXTextureCache::configure_sampler(size_t index, const SceGxmTexture &gxm_texture, bool no_linear) {
    if (index >= samplers.size())
        return;

    // Sampler descriptors are overwritten in place, so a slot is only allocated
    // the first time it is used.
    if (!samplers[index].valid()) {
        samplers[index] = sampler_descriptors.allocate();
        if (!samplers[index].valid())
            return;
    }

    // Linear strided textures use the mag filter for minification as well.
    const bool is_linear_strided = gxm_texture.texture_type() == SCE_GXM_TEXTURE_LINEAR_STRIDED;

    const SceGxmTextureAddrMode uaddr = static_cast<SceGxmTextureAddrMode>(gxm_texture.uaddr_mode);
    const SceGxmTextureAddrMode vaddr = static_cast<SceGxmTextureAddrMode>(gxm_texture.vaddr_mode);

    SceGxmTextureFilter mag_filter = static_cast<SceGxmTextureFilter>(gxm_texture.mag_filter);
    SceGxmTextureFilter min_filter = is_linear_strided
        ? mag_filter
        : static_cast<SceGxmTextureFilter>(gxm_texture.min_filter);

    if (no_linear) {
        min_filter = SCE_GXM_TEXTURE_FILTER_POINT;
        mag_filter = SCE_GXM_TEXTURE_FILTER_POINT;
    }

    const SceGxmTextureMipFilter mip_filter = static_cast<SceGxmTextureMipFilter>(gxm_texture.mip_filter);

    // Nearest-filtered textures can hold data that is not colour, so anisotropy
    // is only applied when at least one of the filters is linear.
    const bool any_linear = mag_filter != SCE_GXM_TEXTURE_FILTER_POINT
        || min_filter != SCE_GXM_TEXTURE_FILTER_POINT;
    const bool use_anisotropy = anisotropic_filtering > 1 && any_linear;

    const D3D12_SAMPLER_DESC desc{
        .Filter = texture::translate_filter(min_filter, mag_filter, mip_filter, use_anisotropy),
        .AddressU = texture::translate_address_mode(uaddr),
        .AddressV = texture::translate_address_mode(vaddr),
        .AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP,
        .MipLODBias = (static_cast<float>(gxm_texture.lod_bias) - 31.0f) / 8.0f,
        .MaxAnisotropy = static_cast<UINT>(use_anisotropy ? anisotropic_filtering : 1),
        .ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER,
        .BorderColor = { 0.0f, 0.0f, 0.0f, 0.0f },
        .MinLOD = static_cast<float>(gxm_texture.lod_min0 | (gxm_texture.lod_min1 << 2)),
        .MaxLOD = D3D12_FLOAT32_MAX,
    };

    state.device->CreateSampler(&desc, samplers[index].cpu);
}

void DXTextureCache::import_configure_impl(SceGxmTextureBaseFormat base_format, uint32_t width, uint32_t height,
    bool is_srgb, uint16_t nb_components, uint16_t mipcount, bool swap_rb) {
    // Replacement textures arrive already decoded, so only the resource and its
    // view need to exist; the upload goes through upload_texture_impl as usual.
    if (!current_texture)
        return;

    DXGI_FORMAT dxgi_format = DXGI_FORMAT_UNKNOWN;
    switch (nb_components) {
    case 1:
        dxgi_format = DXGI_FORMAT_R8_UNORM;
        break;
    case 2:
        dxgi_format = DXGI_FORMAT_R8G8_UNORM;
        break;
    default:
        dxgi_format = swap_rb ? DXGI_FORMAT_B8G8R8A8_UNORM : DXGI_FORMAT_R8G8B8A8_UNORM;
        break;
    }

    if (gxm::is_bcn_format(base_format))
        dxgi_format = texture::translate_format(base_format);

    if (is_srgb)
        dxgi_format = linear_to_srgb(dxgi_format);

    current_texture->format = dxgi_format;
    current_texture->mip_count = mipcount;
    current_texture->is_cube = false;

    if (current_texture->image)
        state.frame().destroy_queue.push_back(current_texture->image.resource);

    current_texture->image.reset();

    const D3D12_HEAP_PROPERTIES heap_props{
        .Type = D3D12_HEAP_TYPE_DEFAULT,
        .CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN,
        .MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN,
        .CreationNodeMask = 1,
        .VisibleNodeMask = 1,
    };

    const D3D12_RESOURCE_DESC desc{
        .Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D,
        .Alignment = 0,
        .Width = width,
        .Height = height,
        .DepthOrArraySize = 1,
        .MipLevels = mipcount,
        .Format = dxgi_format,
        .SampleDesc = { .Count = 1, .Quality = 0 },
        .Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN,
        .Flags = D3D12_RESOURCE_FLAG_NONE,
    };

    if (!dx_check(state.device->CreateCommittedResource(&heap_props, D3D12_HEAP_FLAG_NONE, &desc,
                      D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&current_texture->image.resource)),
            "creating an imported texture"))
        return;

    current_texture->image.state = D3D12_RESOURCE_STATE_COPY_DEST;

    if (!current_texture->srv.valid()) {
        current_texture->srv = state.srv_staging_heap.allocate();
        if (!current_texture->srv.valid())
            return;
    }

    const D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc{
        .Format = dxgi_format,
        .ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D,
        .Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING,
        .Texture2D = { .MostDetailedMip = 0, .MipLevels = mipcount, .PlaneSlice = 0, .ResourceMinLODClamp = 0.0f },
    };
    state.device->CreateShaderResourceView(current_texture->image.get(), &srv_desc, current_texture->srv.cpu);
}

void DXTextureCache::cleanup() {
    for (TextureCacheEntry &entry : textures) {
        if (entry.srv.valid()) {
            state.srv_staging_heap.free(entry.srv);
            entry.srv = {};
        }
        entry.image.reset();
    }

    samplers.clear();
    sampler_descriptors.destroy();
    staging_buffer.destroy();
}

} // namespace renderer::d3d12
