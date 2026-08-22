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

#include <renderer/software/common.h>

#include <gxm/functions.h>
#include <renderer/functions.h>
#include <util/log.h>

#include <algorithm>

namespace renderer::software {

namespace {

float convert_half(uint16_t half) {
    const uint32_t sign = static_cast<uint32_t>(half & 0x8000) << 16;
    uint32_t exponent = (half >> 10) & 0x1F;
    uint32_t mantissa = half & 0x3FF;

    if (exponent == 0) {
        if (mantissa == 0) {
            const uint32_t bits = sign;
            float result;
            std::memcpy(&result, &bits, sizeof(result));
            return result;
        }

        // Subnormal: renormalize into a regular single precision number.
        exponent = 1;
        while ((mantissa & 0x400) == 0) {
            mantissa <<= 1;
            exponent--;
        }
        mantissa &= 0x3FF;
        exponent = exponent + (127 - 15);
    } else if (exponent == 0x1F) {
        // Infinity or NaN.
        const uint32_t bits = sign | 0x7F800000u | (mantissa << 13);
        float result;
        std::memcpy(&result, &bits, sizeof(result));
        return result;
    } else {
        exponent = exponent + (127 - 15);
    }

    const uint32_t bits = sign | (exponent << 23) | (mantissa << 13);
    float result;
    std::memcpy(&result, &bits, sizeof(result));
    return result;
}

} // namespace

const std::array<float, 65536> half_to_float_table = [] {
    std::array<float, 65536> table{};
    for (uint32_t half = 0; half < table.size(); half++)
        table[half] = convert_half(static_cast<uint16_t>(half));
    return table;
}();

uint16_t float_to_half(float value) {
    uint32_t bits;
    std::memcpy(&bits, &value, sizeof(bits));

    const uint16_t sign = static_cast<uint16_t>((bits >> 16) & 0x8000);
    const int32_t exponent = static_cast<int32_t>((bits >> 23) & 0xFF) - 127 + 15;
    const uint32_t mantissa = bits & 0x7FFFFF;

    if (exponent >= 0x1F) {
        // Overflow, infinity and NaN all saturate to the half infinity/NaN.
        return static_cast<uint16_t>(sign | 0x7C00 | (mantissa ? 0x200 : 0));
    }

    if (exponent <= 0) {
        if (exponent < -10)
            return sign;

        // Subnormal half: shift the implicit one back in and round to nearest.
        const uint32_t shifted = (mantissa | 0x800000) >> (1 - exponent + 13);
        return static_cast<uint16_t>(sign | shifted);
    }

    return static_cast<uint16_t>(sign | (static_cast<uint32_t>(exponent) << 10) | (mantissa >> 13));
}

namespace {

// Maps the component stored at position i of a pixel to the RGBA channel it
// carries. The naming of the GXM swizzles runs from the last component to the
// first, so ABGR is the one whose memory layout is plain R, G, B, A.
constexpr std::array<uint8_t, 4> SWIZZLE4_ABGR = { 0, 1, 2, 3 };
constexpr std::array<uint8_t, 4> SWIZZLE4_ARGB = { 2, 1, 0, 3 };
constexpr std::array<uint8_t, 4> SWIZZLE4_RGBA = { 3, 2, 1, 0 };
constexpr std::array<uint8_t, 4> SWIZZLE4_BGRA = { 1, 2, 3, 0 };

constexpr std::array<uint8_t, 4> SWIZZLE3_BGR = { 0, 1, 2, 3 };
constexpr std::array<uint8_t, 4> SWIZZLE3_RGB = { 2, 1, 0, 3 };

constexpr std::array<uint8_t, 4> SWIZZLE2_GR = { 0, 1, 2, 3 };
constexpr std::array<uint8_t, 4> SWIZZLE2_RG = { 1, 0, 2, 3 };
constexpr std::array<uint8_t, 4> SWIZZLE2_RA = { 0, 3, 2, 1 };
constexpr std::array<uint8_t, 4> SWIZZLE2_AR = { 3, 0, 2, 1 };

constexpr std::array<uint8_t, 4> SWIZZLE1_R = { 0, 1, 2, 3 };
constexpr std::array<uint8_t, 4> SWIZZLE1_G = { 1, 0, 2, 3 };
constexpr std::array<uint8_t, 4> SWIZZLE1_A = { 3, 1, 2, 0 };

std::array<uint8_t, 4> swizzle_for(SceGxmColorFormat format, uint32_t component_count) {
    const uint32_t swizzle = static_cast<uint32_t>(format) & 0x00300000u;

    switch (component_count) {
    case 4:
        switch (swizzle) {
        case SCE_GXM_COLOR_SWIZZLE4_ARGB:
            return SWIZZLE4_ARGB;
        case SCE_GXM_COLOR_SWIZZLE4_RGBA:
            return SWIZZLE4_RGBA;
        case SCE_GXM_COLOR_SWIZZLE4_BGRA:
            return SWIZZLE4_BGRA;
        default:
            return SWIZZLE4_ABGR;
        }
    case 3:
        return (swizzle == SCE_GXM_COLOR_SWIZZLE3_RGB) ? SWIZZLE3_RGB : SWIZZLE3_BGR;
    case 2:
        switch (swizzle) {
        case SCE_GXM_COLOR_SWIZZLE2_RG:
            return SWIZZLE2_RG;
        case SCE_GXM_COLOR_SWIZZLE2_RA:
            return SWIZZLE2_RA;
        case SCE_GXM_COLOR_SWIZZLE2_AR:
            return SWIZZLE2_AR;
        default:
            return SWIZZLE2_GR;
        }
    default:
        // The two one-component swizzles G and A share the same encoding, so
        // there is nothing to tell them apart here; treat both as G.
        return (swizzle == SCE_GXM_COLOR_SWIZZLE1_G) ? SWIZZLE1_G : SWIZZLE1_R;
    }
}

uint32_t component_count_of(SceGxmColorBaseFormat base_format) {
    switch (base_format) {
    case SCE_GXM_COLOR_BASE_FORMAT_U8:
    case SCE_GXM_COLOR_BASE_FORMAT_S8:
    case SCE_GXM_COLOR_BASE_FORMAT_U16:
    case SCE_GXM_COLOR_BASE_FORMAT_S16:
    case SCE_GXM_COLOR_BASE_FORMAT_F16:
    case SCE_GXM_COLOR_BASE_FORMAT_F32:
        return 1;
    case SCE_GXM_COLOR_BASE_FORMAT_U8U8:
    case SCE_GXM_COLOR_BASE_FORMAT_S8S8:
    case SCE_GXM_COLOR_BASE_FORMAT_U16U16:
    case SCE_GXM_COLOR_BASE_FORMAT_S16S16:
    case SCE_GXM_COLOR_BASE_FORMAT_F16F16:
    case SCE_GXM_COLOR_BASE_FORMAT_F32F32:
        return 2;
    case SCE_GXM_COLOR_BASE_FORMAT_U8U8U8:
    case SCE_GXM_COLOR_BASE_FORMAT_U5U6U5:
    case SCE_GXM_COLOR_BASE_FORMAT_S5S5U6:
    case SCE_GXM_COLOR_BASE_FORMAT_F11F11F10:
    case SCE_GXM_COLOR_BASE_FORMAT_SE5M9M9M9:
        return 3;
    default:
        return 4;
    }
}

} // namespace

ColorFormatInfo get_color_format_info(SceGxmColorFormat format) {
    ColorFormatInfo info{};
    info.base_format = gxm::get_base_format(format);
    info.component_count = component_count_of(info.base_format);
    info.bytes_per_pixel = static_cast<uint32_t>((gxm::bits_per_pixel(info.base_format) + 7) >> 3);
    info.swizzle = swizzle_for(format, info.component_count);

    switch (info.base_format) {
    case SCE_GXM_COLOR_BASE_FORMAT_F16:
    case SCE_GXM_COLOR_BASE_FORMAT_F16F16:
    case SCE_GXM_COLOR_BASE_FORMAT_F16F16F16F16:
    case SCE_GXM_COLOR_BASE_FORMAT_F32:
    case SCE_GXM_COLOR_BASE_FORMAT_F32F32:
    case SCE_GXM_COLOR_BASE_FORMAT_F11F11F10:
    case SCE_GXM_COLOR_BASE_FORMAT_SE5M9M9M9:
    case SCE_GXM_COLOR_BASE_FORMAT_U2F10F10F10:
        info.is_float = true;
        break;
    default:
        info.is_float = false;
        break;
    }

    return info;
}

namespace {

float unorm_to_float(uint32_t value, uint32_t bits) {
    const uint32_t maximum = (bits >= 32) ? 0xFFFFFFFFu : ((1u << bits) - 1u);
    return static_cast<float>(value) / static_cast<float>(maximum);
}

uint32_t float_to_unorm(float value, uint32_t bits) {
    const uint32_t maximum = (bits >= 32) ? 0xFFFFFFFFu : ((1u << bits) - 1u);
    const float scaled = clamp01(value) * static_cast<float>(maximum) + 0.5f;
    return static_cast<uint32_t>(scaled);
}

float snorm_to_float(int32_t value, uint32_t bits) {
    const float maximum = static_cast<float>((1u << (bits - 1)) - 1u);
    return std::max(static_cast<float>(value) / maximum, -1.0f);
}

int32_t float_to_snorm(float value, uint32_t bits) {
    const float maximum = static_cast<float>((1u << (bits - 1)) - 1u);
    const float clamped = std::clamp(value, -1.0f, 1.0f);
    return static_cast<int32_t>(clamped * maximum + (clamped >= 0.0f ? 0.5f : -0.5f));
}

// Reads a packed pixel of at most 32 bits as one integer.
uint32_t load_packed(const uint8_t *pixel, uint32_t bytes) {
    uint32_t value = 0;
    std::memcpy(&value, pixel, bytes);
    return value;
}

void store_packed(uint8_t *pixel, uint32_t bytes, uint32_t value) {
    std::memcpy(pixel, &value, bytes);
}

// Extracts the bit range of one component of a packed format. Components are
// numbered from the least significant bits upwards.
uint32_t extract_bits(uint32_t value, uint32_t offset, uint32_t bits) {
    return (value >> offset) & ((1u << bits) - 1u);
}

} // namespace

Vec4f read_color_pixel(const ColorFormatInfo &info, const uint8_t *pixel) {
    Vec4f components = { 0.0f, 0.0f, 0.0f, 1.0f };
    std::array<float, 4> raw = { 0.0f, 0.0f, 0.0f, 1.0f };

    switch (info.base_format) {
    case SCE_GXM_COLOR_BASE_FORMAT_U8U8U8U8:
        for (uint32_t i = 0; i < 4; i++)
            raw[i] = unorm_to_float(pixel[i], 8);
        break;
    case SCE_GXM_COLOR_BASE_FORMAT_U8U8U8:
        for (uint32_t i = 0; i < 3; i++)
            raw[i] = unorm_to_float(pixel[i], 8);
        break;
    case SCE_GXM_COLOR_BASE_FORMAT_U8U8:
        for (uint32_t i = 0; i < 2; i++)
            raw[i] = unorm_to_float(pixel[i], 8);
        break;
    case SCE_GXM_COLOR_BASE_FORMAT_U8:
        raw[0] = unorm_to_float(pixel[0], 8);
        break;
    case SCE_GXM_COLOR_BASE_FORMAT_S8S8S8S8:
        for (uint32_t i = 0; i < 4; i++)
            raw[i] = snorm_to_float(static_cast<int8_t>(pixel[i]), 8);
        break;
    case SCE_GXM_COLOR_BASE_FORMAT_S8S8:
        for (uint32_t i = 0; i < 2; i++)
            raw[i] = snorm_to_float(static_cast<int8_t>(pixel[i]), 8);
        break;
    case SCE_GXM_COLOR_BASE_FORMAT_S8:
        raw[0] = snorm_to_float(static_cast<int8_t>(pixel[0]), 8);
        break;
    case SCE_GXM_COLOR_BASE_FORMAT_U5U6U5: {
        const uint32_t value = load_packed(pixel, 2);
        raw[0] = unorm_to_float(extract_bits(value, 0, 5), 5);
        raw[1] = unorm_to_float(extract_bits(value, 5, 6), 6);
        raw[2] = unorm_to_float(extract_bits(value, 11, 5), 5);
        break;
    }
    case SCE_GXM_COLOR_BASE_FORMAT_U1U5U5U5: {
        const uint32_t value = load_packed(pixel, 2);
        raw[0] = unorm_to_float(extract_bits(value, 0, 5), 5);
        raw[1] = unorm_to_float(extract_bits(value, 5, 5), 5);
        raw[2] = unorm_to_float(extract_bits(value, 10, 5), 5);
        raw[3] = unorm_to_float(extract_bits(value, 15, 1), 1);
        break;
    }
    case SCE_GXM_COLOR_BASE_FORMAT_U4U4U4U4: {
        const uint32_t value = load_packed(pixel, 2);
        for (uint32_t i = 0; i < 4; i++)
            raw[i] = unorm_to_float(extract_bits(value, i * 4, 4), 4);
        break;
    }
    case SCE_GXM_COLOR_BASE_FORMAT_U8U3U3U2: {
        const uint32_t value = load_packed(pixel, 2);
        raw[0] = unorm_to_float(extract_bits(value, 0, 2), 2);
        raw[1] = unorm_to_float(extract_bits(value, 2, 3), 3);
        raw[2] = unorm_to_float(extract_bits(value, 5, 3), 3);
        raw[3] = unorm_to_float(extract_bits(value, 8, 8), 8);
        break;
    }
    case SCE_GXM_COLOR_BASE_FORMAT_U16:
    case SCE_GXM_COLOR_BASE_FORMAT_U16U16: {
        const uint16_t *values = reinterpret_cast<const uint16_t *>(pixel);
        for (uint32_t i = 0; i < info.component_count; i++)
            raw[i] = unorm_to_float(values[i], 16);
        break;
    }
    case SCE_GXM_COLOR_BASE_FORMAT_S16:
    case SCE_GXM_COLOR_BASE_FORMAT_S16S16: {
        const int16_t *values = reinterpret_cast<const int16_t *>(pixel);
        for (uint32_t i = 0; i < info.component_count; i++)
            raw[i] = snorm_to_float(values[i], 16);
        break;
    }
    case SCE_GXM_COLOR_BASE_FORMAT_F16:
    case SCE_GXM_COLOR_BASE_FORMAT_F16F16:
    case SCE_GXM_COLOR_BASE_FORMAT_F16F16F16F16: {
        const uint16_t *values = reinterpret_cast<const uint16_t *>(pixel);
        for (uint32_t i = 0; i < info.component_count; i++)
            raw[i] = half_to_float(values[i]);
        break;
    }
    case SCE_GXM_COLOR_BASE_FORMAT_F32:
    case SCE_GXM_COLOR_BASE_FORMAT_F32F32: {
        const float *values = reinterpret_cast<const float *>(pixel);
        for (uint32_t i = 0; i < info.component_count; i++)
            raw[i] = values[i];
        break;
    }
    case SCE_GXM_COLOR_BASE_FORMAT_U2U10U10U10: {
        const uint32_t value = load_packed(pixel, 4);
        raw[0] = unorm_to_float(extract_bits(value, 0, 10), 10);
        raw[1] = unorm_to_float(extract_bits(value, 10, 10), 10);
        raw[2] = unorm_to_float(extract_bits(value, 20, 10), 10);
        raw[3] = unorm_to_float(extract_bits(value, 30, 2), 2);
        break;
    }
    default:
        LOG_ERROR_ONCE("Software renderer: unhandled color surface format 0x{:08X} on read",
            fmt::underlying(info.base_format));
        break;
    }

    for (uint32_t i = 0; i < info.component_count; i++)
        components[info.swizzle[i]] = raw[i];

    // Components the format does not carry read back as the usual defaults.
    if (info.component_count < 4) {
        bool has_alpha = false;
        for (uint32_t i = 0; i < info.component_count; i++)
            has_alpha |= (info.swizzle[i] == 3);
        if (!has_alpha)
            components[3] = 1.0f;
    }

    return components;
}

void write_color_pixel(const ColorFormatInfo &info, uint8_t *pixel, const Vec4f &color) {
    std::array<float, 4> raw = { 0.0f, 0.0f, 0.0f, 0.0f };
    for (uint32_t i = 0; i < info.component_count; i++)
        raw[i] = color[info.swizzle[i]];

    switch (info.base_format) {
    case SCE_GXM_COLOR_BASE_FORMAT_U8U8U8U8:
        for (uint32_t i = 0; i < 4; i++)
            pixel[i] = static_cast<uint8_t>(float_to_unorm(raw[i], 8));
        break;
    case SCE_GXM_COLOR_BASE_FORMAT_U8U8U8:
        for (uint32_t i = 0; i < 3; i++)
            pixel[i] = static_cast<uint8_t>(float_to_unorm(raw[i], 8));
        break;
    case SCE_GXM_COLOR_BASE_FORMAT_U8U8:
        for (uint32_t i = 0; i < 2; i++)
            pixel[i] = static_cast<uint8_t>(float_to_unorm(raw[i], 8));
        break;
    case SCE_GXM_COLOR_BASE_FORMAT_U8:
        pixel[0] = static_cast<uint8_t>(float_to_unorm(raw[0], 8));
        break;
    case SCE_GXM_COLOR_BASE_FORMAT_S8S8S8S8:
        for (uint32_t i = 0; i < 4; i++)
            pixel[i] = static_cast<uint8_t>(static_cast<int8_t>(float_to_snorm(raw[i], 8)));
        break;
    case SCE_GXM_COLOR_BASE_FORMAT_S8S8:
        for (uint32_t i = 0; i < 2; i++)
            pixel[i] = static_cast<uint8_t>(static_cast<int8_t>(float_to_snorm(raw[i], 8)));
        break;
    case SCE_GXM_COLOR_BASE_FORMAT_S8:
        pixel[0] = static_cast<uint8_t>(static_cast<int8_t>(float_to_snorm(raw[0], 8)));
        break;
    case SCE_GXM_COLOR_BASE_FORMAT_U5U6U5: {
        const uint32_t value = float_to_unorm(raw[0], 5)
            | (float_to_unorm(raw[1], 6) << 5)
            | (float_to_unorm(raw[2], 5) << 11);
        store_packed(pixel, 2, value);
        break;
    }
    case SCE_GXM_COLOR_BASE_FORMAT_U1U5U5U5: {
        const uint32_t value = float_to_unorm(raw[0], 5)
            | (float_to_unorm(raw[1], 5) << 5)
            | (float_to_unorm(raw[2], 5) << 10)
            | (float_to_unorm(raw[3], 1) << 15);
        store_packed(pixel, 2, value);
        break;
    }
    case SCE_GXM_COLOR_BASE_FORMAT_U4U4U4U4: {
        uint32_t value = 0;
        for (uint32_t i = 0; i < 4; i++)
            value |= float_to_unorm(raw[i], 4) << (i * 4);
        store_packed(pixel, 2, value);
        break;
    }
    case SCE_GXM_COLOR_BASE_FORMAT_U8U3U3U2: {
        const uint32_t value = float_to_unorm(raw[0], 2)
            | (float_to_unorm(raw[1], 3) << 2)
            | (float_to_unorm(raw[2], 3) << 5)
            | (float_to_unorm(raw[3], 8) << 8);
        store_packed(pixel, 2, value);
        break;
    }
    case SCE_GXM_COLOR_BASE_FORMAT_U16:
    case SCE_GXM_COLOR_BASE_FORMAT_U16U16: {
        uint16_t *values = reinterpret_cast<uint16_t *>(pixel);
        for (uint32_t i = 0; i < info.component_count; i++)
            values[i] = static_cast<uint16_t>(float_to_unorm(raw[i], 16));
        break;
    }
    case SCE_GXM_COLOR_BASE_FORMAT_S16:
    case SCE_GXM_COLOR_BASE_FORMAT_S16S16: {
        int16_t *values = reinterpret_cast<int16_t *>(pixel);
        for (uint32_t i = 0; i < info.component_count; i++)
            values[i] = static_cast<int16_t>(float_to_snorm(raw[i], 16));
        break;
    }
    case SCE_GXM_COLOR_BASE_FORMAT_F16:
    case SCE_GXM_COLOR_BASE_FORMAT_F16F16:
    case SCE_GXM_COLOR_BASE_FORMAT_F16F16F16F16: {
        uint16_t *values = reinterpret_cast<uint16_t *>(pixel);
        for (uint32_t i = 0; i < info.component_count; i++)
            values[i] = float_to_half(raw[i]);
        break;
    }
    case SCE_GXM_COLOR_BASE_FORMAT_F32:
    case SCE_GXM_COLOR_BASE_FORMAT_F32F32: {
        float *values = reinterpret_cast<float *>(pixel);
        for (uint32_t i = 0; i < info.component_count; i++)
            values[i] = raw[i];
        break;
    }
    case SCE_GXM_COLOR_BASE_FORMAT_U2U10U10U10: {
        const uint32_t value = float_to_unorm(raw[0], 10)
            | (float_to_unorm(raw[1], 10) << 10)
            | (float_to_unorm(raw[2], 10) << 20)
            | (float_to_unorm(raw[3], 2) << 30);
        store_packed(pixel, 4, value);
        break;
    }
    default:
        LOG_ERROR_ONCE("Software renderer: unhandled color surface format 0x{:08X} on write",
            fmt::underlying(info.base_format));
        break;
    }
}

uint32_t color_pixel_offset(SceGxmColorSurfaceType surface_type, uint32_t x, uint32_t y,
    uint32_t stride_in_pixels, uint32_t height, uint32_t bytes_per_pixel) {
    switch (surface_type) {
    case SCE_GXM_COLOR_SURFACE_TILED: {
        // Tiles are 32x32 pixels, stored one after the other in raster order.
        constexpr uint32_t tile_size = 32;
        const uint32_t tiles_per_row = std::max(stride_in_pixels / tile_size, 1u);
        const uint32_t tile_index = (y / tile_size) * tiles_per_row + (x / tile_size);
        const uint32_t inside = (y % tile_size) * tile_size + (x % tile_size);
        return (tile_index * tile_size * tile_size + inside) * bytes_per_pixel;
    }
    case SCE_GXM_COLOR_SURFACE_SWIZZLED:
        return texture::encode_morton(static_cast<uint16_t>(x), static_cast<uint16_t>(y),
                   static_cast<uint16_t>(stride_in_pixels), static_cast<uint16_t>(height))
            * bytes_per_pixel;
    default:
        return (y * stride_in_pixels + x) * bytes_per_pixel;
    }
}

} // namespace renderer::software
