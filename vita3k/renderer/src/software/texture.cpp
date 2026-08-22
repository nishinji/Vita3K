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

#include <renderer/software/texture.h>

#include <renderer/software/state.h>

#include <gxm/functions.h>
#include <util/log.h>

#include <algorithm>
#include <cmath>

namespace renderer::software {

namespace {

// How many mip levels one face can hold. A Vita texture tops out well below
// this, so the slot per (face, mip) never has to grow.
constexpr uint32_t MAX_MIPS = 16;

// Destination channel mapping: values 0-3 pick a source component, -1 is a
// constant zero and -2 a constant one. This mirrors the GL_TEXTURE_SWIZZLE_RGBA
// tables the OpenGL backend uses, so both agree on what a format means.
using Swizzle = std::array<int8_t, 4>;

constexpr int8_t ZERO = -1;
constexpr int8_t ONE = -2;

Swizzle swizzle_of(SceGxmTextureFormat format, uint32_t component_count) {
    const uint32_t swizzle = static_cast<uint32_t>(format) & 0x00007000u;

    switch (component_count) {
    case 4:
        switch (swizzle) {
        case SCE_GXM_TEXTURE_SWIZZLE4_ARGB:
            return { 2, 1, 0, 3 };
        case SCE_GXM_TEXTURE_SWIZZLE4_RGBA:
            return { 3, 2, 1, 0 };
        case SCE_GXM_TEXTURE_SWIZZLE4_BGRA:
            return { 1, 2, 3, 0 };
        case SCE_GXM_TEXTURE_SWIZZLE4_1BGR:
            return { 0, 1, 2, ONE };
        case SCE_GXM_TEXTURE_SWIZZLE4_1RGB:
            return { 2, 1, 0, ONE };
        case SCE_GXM_TEXTURE_SWIZZLE4_RGB1:
            return { 3, 2, 1, ONE };
        case SCE_GXM_TEXTURE_SWIZZLE4_BGR1:
            return { 1, 2, 3, ONE };
        default:
            return { 0, 1, 2, 3 };
        }
    case 3:
        return (swizzle == SCE_GXM_TEXTURE_SWIZZLE3_RGB)
            ? Swizzle{ 2, 1, 0, ONE }
            : Swizzle{ 0, 1, 2, ONE };
    case 2:
        switch (swizzle) {
        case SCE_GXM_TEXTURE_SWIZZLE2_00GR:
            return { 0, 1, ZERO, ZERO };
        case SCE_GXM_TEXTURE_SWIZZLE2_GRRR:
            return { 0, 0, 0, 1 };
        case SCE_GXM_TEXTURE_SWIZZLE2_RGGG:
            return { 1, 1, 1, 0 };
        case SCE_GXM_TEXTURE_SWIZZLE2_GRGR:
            return { 0, 1, 0, 1 };
        case SCE_GXM_TEXTURE_SWIZZLE2_00RG:
            return { 1, 0, ZERO, ZERO };
        default:
            return { 0, 1, ZERO, ONE };
        }
    default:
        switch (swizzle) {
        case SCE_GXM_TEXTURE_SWIZZLE1_000R:
            return { 0, ZERO, ZERO, ZERO };
        case SCE_GXM_TEXTURE_SWIZZLE1_111R:
            return { 0, ONE, ONE, ONE };
        case SCE_GXM_TEXTURE_SWIZZLE1_RRRR:
            return { 0, 0, 0, 0 };
        case SCE_GXM_TEXTURE_SWIZZLE1_0RRR:
            return { 0, 0, 0, ZERO };
        case SCE_GXM_TEXTURE_SWIZZLE1_1RRR:
            return { 0, 0, 0, ONE };
        case SCE_GXM_TEXTURE_SWIZZLE1_R000:
            return { ZERO, ZERO, ZERO, 0 };
        case SCE_GXM_TEXTURE_SWIZZLE1_R111:
            return { ONE, ONE, ONE, 0 };
        default:
            return { 0, ZERO, ZERO, ONE };
        }
    }
}

uint32_t component_count_of(SceGxmTextureBaseFormat base_format) {
    return static_cast<uint32_t>(gxm::get_num_components(base_format));
}

// Decodes one texel of a linear texture into its raw components, before the
// format swizzle is applied.
void decode_texel(SceGxmTextureBaseFormat base_format, uint32_t components, const uint8_t *source, float *raw) {
    raw[0] = raw[1] = raw[2] = 0.0f;
    raw[3] = 1.0f;

    switch (base_format) {
    case SCE_GXM_TEXTURE_BASE_FORMAT_U8U8U8U8:
        for (uint32_t i = 0; i < 4; i++)
            raw[i] = source[i] / 255.0f;
        break;
    case SCE_GXM_TEXTURE_BASE_FORMAT_U8U8U8:
        for (uint32_t i = 0; i < 3; i++)
            raw[i] = source[i] / 255.0f;
        break;
    case SCE_GXM_TEXTURE_BASE_FORMAT_U8U8:
        for (uint32_t i = 0; i < 2; i++)
            raw[i] = source[i] / 255.0f;
        break;
    case SCE_GXM_TEXTURE_BASE_FORMAT_U8:
        raw[0] = source[0] / 255.0f;
        break;
    case SCE_GXM_TEXTURE_BASE_FORMAT_S8S8S8S8:
        for (uint32_t i = 0; i < 4; i++)
            raw[i] = std::max(static_cast<int8_t>(source[i]) / 127.0f, -1.0f);
        break;
    case SCE_GXM_TEXTURE_BASE_FORMAT_S8S8:
        for (uint32_t i = 0; i < 2; i++)
            raw[i] = std::max(static_cast<int8_t>(source[i]) / 127.0f, -1.0f);
        break;
    case SCE_GXM_TEXTURE_BASE_FORMAT_S8:
        raw[0] = std::max(static_cast<int8_t>(source[0]) / 127.0f, -1.0f);
        break;
    case SCE_GXM_TEXTURE_BASE_FORMAT_U5U6U5: {
        uint16_t value;
        std::memcpy(&value, source, sizeof(value));
        raw[0] = (value & 0x1F) / 31.0f;
        raw[1] = ((value >> 5) & 0x3F) / 63.0f;
        raw[2] = ((value >> 11) & 0x1F) / 31.0f;
        break;
    }
    case SCE_GXM_TEXTURE_BASE_FORMAT_U1U5U5U5: {
        uint16_t value;
        std::memcpy(&value, source, sizeof(value));
        raw[0] = (value & 0x1F) / 31.0f;
        raw[1] = ((value >> 5) & 0x1F) / 31.0f;
        raw[2] = ((value >> 10) & 0x1F) / 31.0f;
        raw[3] = ((value >> 15) & 0x1) ? 1.0f : 0.0f;
        break;
    }
    case SCE_GXM_TEXTURE_BASE_FORMAT_U4U4U4U4: {
        uint16_t value;
        std::memcpy(&value, source, sizeof(value));
        for (uint32_t i = 0; i < 4; i++)
            raw[i] = ((value >> (i * 4)) & 0xF) / 15.0f;
        break;
    }
    case SCE_GXM_TEXTURE_BASE_FORMAT_U16U16U16U16:
    case SCE_GXM_TEXTURE_BASE_FORMAT_U16U16:
    case SCE_GXM_TEXTURE_BASE_FORMAT_U16: {
        const uint16_t *values = reinterpret_cast<const uint16_t *>(source);
        for (uint32_t i = 0; i < components; i++)
            raw[i] = values[i] / 65535.0f;
        break;
    }
    case SCE_GXM_TEXTURE_BASE_FORMAT_F16F16F16F16:
    case SCE_GXM_TEXTURE_BASE_FORMAT_F16F16:
    case SCE_GXM_TEXTURE_BASE_FORMAT_F16: {
        const uint16_t *values = reinterpret_cast<const uint16_t *>(source);
        for (uint32_t i = 0; i < components; i++)
            raw[i] = half_to_float(values[i]);
        break;
    }
    case SCE_GXM_TEXTURE_BASE_FORMAT_F32F32:
    case SCE_GXM_TEXTURE_BASE_FORMAT_F32: {
        const float *values = reinterpret_cast<const float *>(source);
        for (uint32_t i = 0; i < components; i++)
            raw[i] = values[i];
        break;
    }
    case SCE_GXM_TEXTURE_BASE_FORMAT_X8U24: {
        // The base cache hands this over as U24X8 for anything but Vulkan.
        uint32_t value;
        std::memcpy(&value, source, sizeof(value));
        raw[0] = static_cast<float>(value >> 8) / 16777215.0f;
        break;
    }
    case SCE_GXM_TEXTURE_BASE_FORMAT_U2U10U10U10: {
        uint32_t value;
        std::memcpy(&value, source, sizeof(value));
        raw[0] = (value & 0x3FF) / 1023.0f;
        raw[1] = ((value >> 10) & 0x3FF) / 1023.0f;
        raw[2] = ((value >> 20) & 0x3FF) / 1023.0f;
        raw[3] = ((value >> 30) & 0x3) / 3.0f;
        break;
    }
    default:
        LOG_ERROR_ONCE("Software renderer: unhandled texture format 0x{:08X}", fmt::underlying(base_format));
        break;
    }
}

inline float wrap_coordinate(float value, SceGxmTextureAddrMode mode) {
    switch (mode) {
    case SCE_GXM_TEXTURE_ADDR_MIRROR:
        // Mirrors on every other repeat of the texture.
        value = std::fabs(std::fmod(value, 2.0f));
        if (value > 1.0f)
            value = 2.0f - value;
        return value;
    case SCE_GXM_TEXTURE_ADDR_CLAMP:
    case SCE_GXM_TEXTURE_ADDR_CLAMP_FULL_BORDER:
    case SCE_GXM_TEXTURE_ADDR_CLAMP_IGNORE_BORDER:
    case SCE_GXM_TEXTURE_ADDR_CLAMP_HALF_BORDER:
        return std::clamp(value, 0.0f, 1.0f);
    case SCE_GXM_TEXTURE_ADDR_MIRROR_CLAMP:
        return std::clamp(std::fabs(value), 0.0f, 1.0f);
    default: {
        const float wrapped = value - std::floor(value);
        return wrapped;
    }
    }
}

inline Vec4f fetch_texel(const SWTextureLevel &level, int32_t x, int32_t y) {
    x = std::clamp<int32_t>(x, 0, static_cast<int32_t>(level.width) - 1);
    y = std::clamp<int32_t>(y, 0, static_cast<int32_t>(level.height) - 1);

    const size_t offset = (static_cast<size_t>(y) * level.width + x) * 4;
    return { level.pixels[offset], level.pixels[offset + 1], level.pixels[offset + 2], level.pixels[offset + 3] };
}

inline Vec4f sample_level(const SWTextureLevel &level, float u, float v, bool linear) {
    if (level.width == 0 || level.height == 0)
        return { 0.0f, 0.0f, 0.0f, 1.0f };

    if (!linear) {
        const int32_t x = static_cast<int32_t>(u * level.width);
        const int32_t y = static_cast<int32_t>(v * level.height);
        return fetch_texel(level, x, y);
    }

    const float x = u * level.width - 0.5f;
    const float y = v * level.height - 0.5f;
    const int32_t x0 = static_cast<int32_t>(std::floor(x));
    const int32_t y0 = static_cast<int32_t>(std::floor(y));
    const float fx = x - x0;
    const float fy = y - y0;

    const Vec4f c00 = fetch_texel(level, x0, y0);
    const Vec4f c10 = fetch_texel(level, x0 + 1, y0);
    const Vec4f c01 = fetch_texel(level, x0, y0 + 1);
    const Vec4f c11 = fetch_texel(level, x0 + 1, y0 + 1);

    Vec4f result{};
    for (uint32_t i = 0; i < 4; i++) {
        const float top = c00[i] * (1.0f - fx) + c10[i] * fx;
        const float bottom = c01[i] * (1.0f - fx) + c11[i] * fx;
        result[i] = top * (1.0f - fy) + bottom * fy;
    }
    return result;
}

} // namespace

Vec4f SWSampler::sample(float u, float v, float lod) const {
    if (!texture || !texture->valid || texture->levels.empty())
        return { 0.0f, 0.0f, 0.0f, 1.0f };

    u = wrap_coordinate(u, uaddr);
    v = wrap_coordinate(v, vaddr);

    const bool magnifying = lod <= 0.0f;
    const SceGxmTextureFilter filter = magnifying ? mag_filter : min_filter;
    const bool linear = (filter != SCE_GXM_TEXTURE_FILTER_POINT);

    if (mip_filter == SCE_GXM_TEXTURE_MIP_FILTER_DISABLED || texture->levels.size() <= 1)
        return sample_level(texture->levels[0], u, v, linear);

    const float biased = std::max(0.0f, lod + lod_bias);
    const uint32_t base = std::min<uint32_t>(static_cast<uint32_t>(biased),
        static_cast<uint32_t>(texture->levels.size()) - 1);
    const uint32_t next = std::min<uint32_t>(base + 1, static_cast<uint32_t>(texture->levels.size()) - 1);

    const Vec4f coarse = sample_level(texture->levels[base], u, v, linear);
    if (base == next)
        return coarse;

    const Vec4f finer = sample_level(texture->levels[next], u, v, linear);
    const float blend = biased - static_cast<float>(base);

    Vec4f result{};
    for (uint32_t i = 0; i < 4; i++)
        result[i] = coarse[i] * (1.0f - blend) + finer[i] * blend;
    return result;
}

SWTextureCache::SWTextureCache(SWState &state)
    : m_state(state) {
    backend = Backend::Software;
}

SWTexture &SWTextureCache::current_texture() {
    return m_textures[std::min(m_current_index, m_textures.size() - 1)];
}

void SWTextureCache::select(size_t index, const SceGxmTexture &texture) {
    m_current_index = index;
    m_current_format = gxm::get_format(texture);
}

void SWTextureCache::configure_texture(const SceGxmTexture &texture) {
    SWTexture &target = current_texture();

    const auto texture_type = texture.texture_type();
    const bool is_cube = (texture_type == SCE_GXM_TEXTURE_CUBE)
        || (texture_type == SCE_GXM_TEXTURE_CUBE_ARBITRARY);

    target.face_count = is_cube ? 6 : 1;
    target.is_srgb = false;
    target.valid = false;
    target.levels.assign(static_cast<size_t>(target.face_count) * MAX_MIPS, SWTextureLevel{});
}

void SWTextureCache::upload_texture_impl(SceGxmTextureBaseFormat base_format, uint32_t width, uint32_t height,
    uint32_t mip_index, const void *pixels, int face, uint32_t pixels_per_stride) {
    SWTexture &target = current_texture();

    if (target.levels.empty())
        target.levels.assign(MAX_MIPS, SWTextureLevel{});

    if (mip_index >= MAX_MIPS)
        return;

    const size_t slot = static_cast<size_t>(std::max(face - 1, 0)) * MAX_MIPS + mip_index;
    if (slot >= target.levels.size())
        return;

    SWTextureLevel &level = target.levels[slot];
    level.width = width;
    level.height = height;
    level.pixels.assign(static_cast<size_t>(width) * height * 4, 0.0f);

    const uint32_t components = component_count_of(base_format);
    // A replacement texture is already decoded into plain RGBA, so the swizzle
    // of the format it stands in for must not be applied a second time.
    // The YUV swizzle modes pick the conversion the base cache already did to RGBA, not a channel order.
    const SceGxmTextureBaseFormat original_format = gxm::get_base_format(m_current_format);
    const bool converted_yuv = original_format == SCE_GXM_TEXTURE_BASE_FORMAT_YUV420P2
        || original_format == SCE_GXM_TEXTURE_BASE_FORMAT_YUV420P3 || original_format == SCE_GXM_TEXTURE_BASE_FORMAT_YUV422;
    Swizzle swizzle = (importing_texture || converted_yuv)
        ? Swizzle{ 0, 1, 2, 3 }
        : swizzle_of(m_current_format, components);
    if (importing_texture && m_import_swap_rb)
        swizzle = Swizzle{ 2, 1, 0, 3 };

    const uint32_t bytes_per_pixel = std::max(1u, (gxm::bits_per_pixel(base_format) + 7) >> 3);
    const uint8_t *source = static_cast<const uint8_t *>(pixels);

    // The constants sit after the four raw components, so the swizzle is a plain gather.
    std::array<uint32_t, 4> gather{};
    for (uint32_t i = 0; i < 4; i++)
        gather[i] = (swizzle[i] == ZERO) ? 4 : ((swizzle[i] == ONE) ? 5 : static_cast<uint32_t>(swizzle[i]));

    for (uint32_t y = 0; y < height; y++) {
        const uint8_t *row = source + static_cast<size_t>(y) * pixels_per_stride * bytes_per_pixel;
        float *destination = &level.pixels[static_cast<size_t>(y) * width * 4];
        for (uint32_t x = 0; x < width; x++, destination += 4) {
            float raw[6];
            if (base_format == SCE_GXM_TEXTURE_BASE_FORMAT_U8U8U8U8) {
                const uint8_t *texel = row + static_cast<size_t>(x) * 4;
                for (uint32_t i = 0; i < 4; i++)
                    raw[i] = texel[i] / 255.0f;
            } else {
                decode_texel(base_format, components, row + static_cast<size_t>(x) * bytes_per_pixel, raw);
            }
            raw[4] = 0.0f;
            raw[5] = 1.0f;

            for (uint32_t i = 0; i < 4; i++)
                destination[i] = raw[gather[i]];
        }
    }

    target.valid = true;
}

void SWTextureCache::import_configure_impl(SceGxmTextureBaseFormat base_format, uint32_t width, uint32_t height,
    bool is_srgb, uint16_t nb_components, uint16_t mipcount, bool swap_rb) {
    SWTexture &target = current_texture();

    target.face_count = 1;
    target.is_srgb = is_srgb;
    target.valid = false;
    target.levels.assign(MAX_MIPS, SWTextureLevel{});

    m_import_components = nb_components;
    m_import_swap_rb = swap_rb;
}

void SWTextureCache::bind(bool is_vertex, uint32_t slot, const SceGxmTexture &gxm_texture, MemState &mem) {
    if (slot >= SCE_GXM_MAX_TEXTURE_UNITS)
        return;

    cache_and_bind_texture(gxm_texture, mem);

    SWSampler &sampler = is_vertex ? m_vertex_samplers[slot] : m_fragment_samplers[slot];
    sampler.texture = &current_texture();
    sampler.uaddr = static_cast<SceGxmTextureAddrMode>(gxm_texture.uaddr_mode);
    sampler.vaddr = static_cast<SceGxmTextureAddrMode>(gxm_texture.vaddr_mode);
    sampler.min_filter = static_cast<SceGxmTextureFilter>(gxm_texture.min_filter);
    sampler.mag_filter = static_cast<SceGxmTextureFilter>(gxm_texture.mag_filter);
    sampler.mip_filter = gxm_texture.mip_filter
        ? SCE_GXM_TEXTURE_MIP_FILTER_ENABLED
        : SCE_GXM_TEXTURE_MIP_FILTER_DISABLED;
    sampler.lod_bias = static_cast<float>(gxm_texture.lod_bias) / 6.0f - 31.0f;
    sampler.lod_min = gxm_texture.lod_min0 | (gxm_texture.lod_min1 << 2);

    const auto texture_type = gxm_texture.texture_type();
    sampler.is_cube = (texture_type == SCE_GXM_TEXTURE_CUBE)
        || (texture_type == SCE_GXM_TEXTURE_CUBE_ARBITRARY);
}

const SWSampler *SWTextureCache::bound_sampler(bool is_vertex, uint32_t slot) const {
    if (slot >= SCE_GXM_MAX_TEXTURE_UNITS)
        return nullptr;

    const SWSampler &sampler = is_vertex ? m_vertex_samplers[slot] : m_fragment_samplers[slot];
    return sampler.texture ? &sampler : nullptr;
}

void SWTextureCache::reset_bindings() {
    for (auto &sampler : m_vertex_samplers)
        sampler.texture = nullptr;
    for (auto &sampler : m_fragment_samplers)
        sampler.texture = nullptr;
}

void SWTextureCache::cleanup() {
    reset_bindings();
    for (auto &texture : m_textures) {
        texture.levels.clear();
        texture.valid = false;
    }
}

} // namespace renderer::software
