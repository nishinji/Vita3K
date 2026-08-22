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

#include <gxm/types.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace renderer::software {

using Vec4f = std::array<float, 4>;

// Half float conversion. The GXM surface and texture formats use IEEE binary16
// in a few places and the interpreter needs it for OpFConvert as well.
// Every half maps through a table: uploads decode whole F16 render targets each frame.
extern const std::array<float, 65536> half_to_float_table;

inline float half_to_float(uint16_t half) {
    return half_to_float_table[half];
}

uint16_t float_to_half(float value);

inline float clamp01(float value) {
    return value < 0.0f ? 0.0f : (value > 1.0f ? 1.0f : value);
}

inline float srgb_to_linear(float value) {
    return (value <= 0.04045f) ? (value / 12.92f) : std::pow((value + 0.055f) / 1.055f, 2.4f);
}

inline float linear_to_srgb(float value) {
    return (value <= 0.0031308f) ? (value * 12.92f) : (1.055f * std::pow(value, 1.0f / 2.4f) - 0.055f);
}

// Describes how one pixel of a color surface is laid out in guest memory. The
// software renderer writes straight into the guest surface, so every draw needs
// this to encode and decode the pixels it touches.
struct ColorFormatInfo {
    SceGxmColorBaseFormat base_format;
    // Bytes taken by one pixel. Formats whose size is not a whole number of
    // bytes do not exist among the color formats, unlike the texture ones.
    uint32_t bytes_per_pixel;
    uint32_t component_count;
    // Maps a component of the surface to the channel of the RGBA value it
    // holds, i.e. swizzle[i] is the RGBA channel stored at position i.
    std::array<uint8_t, 4> swizzle;
    bool is_float;
};

ColorFormatInfo get_color_format_info(SceGxmColorFormat format);

// Read/write a single pixel of a color surface, always in RGBA order with the
// values already swizzled back into their canonical channel.
Vec4f read_color_pixel(const ColorFormatInfo &info, const uint8_t *pixel);
void write_color_pixel(const ColorFormatInfo &info, uint8_t *pixel, const Vec4f &color);

// Byte offset of a pixel inside a color surface. Tiled and swizzled surfaces do
// not store their pixels in scanline order, so this is not simply y * stride.
uint32_t color_pixel_offset(SceGxmColorSurfaceType surface_type, uint32_t x, uint32_t y,
    uint32_t stride_in_pixels, uint32_t height, uint32_t bytes_per_pixel);

} // namespace renderer::software
