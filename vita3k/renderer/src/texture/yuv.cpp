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

#include <renderer/functions.h>
#include <renderer/texture_cache.h>

extern "C" {
#include <libswscale/swscale.h>
}

namespace renderer::texture {

static SwsContext *get_sws_context(YUVConversionCache &cache, size_t width, size_t height) {
    bool recreate = false;
    auto *context = static_cast<SwsContext *>(cache.sws_context);
    if (cache.width != width || cache.height != height) {
        recreate = true;
        cache.width = width;
        cache.height = height;
    } else if (context == nullptr) {
        recreate = true;
    }

    if (recreate) {
        if (context != nullptr) {
            sws_freeContext(context);
            context = nullptr;
        }
        // swscale has a fast (SIMD) path converting YUV420P to RGBA of the same size, but not NV12 or RGB0
        context = sws_getContext(width, height, AV_PIX_FMT_YUV420P, width, height, AV_PIX_FMT_RGBA,
            0, nullptr, nullptr, nullptr);
        cache.sws_context = context;
    }
    return context;
}

void yuv420_texture_to_rgb(YUVConversionCache &cache, uint8_t *dst, const uint8_t *src, uint32_t width, uint32_t height, uint32_t layout_width, uint32_t layout_height, bool is_p3) {
    SwsContext *context = get_sws_context(cache, width, height);
    assert(context);

    const uint8_t *slices[] = {
        src, // Y Slice
        src + layout_width * layout_height, // U Slice
        src + layout_width * layout_height + layout_width * layout_height / 4, // V Slice
    };

    const int strides[] = {
        static_cast<int>(width),
        static_cast<int>(width / 2),
        static_cast<int>(width / 2),
    };
    if (!is_p3) {
        // the U and V samples are interleaved in a single plane, split them
        const uint8_t *uv = src + layout_width * layout_height;
        const size_t chroma_size = (width / 2) * (height / 2);
        cache.chroma.resize(chroma_size * 2);
        uint8_t *u = cache.chroma.data();
        uint8_t *v = u + chroma_size;
        for (size_t i = 0; i < chroma_size; i++) {
            u[i] = uv[2 * i];
            v[i] = uv[2 * i + 1];
        }
        slices[1] = u;
        slices[2] = v;
    }

    uint8_t *dst_slices[] = {
        dst,
    };

    const int dst_strides[] = {
        static_cast<int>(width * 4),
    };

    int error = sws_scale(context, slices, strides, 0, height, dst_slices, dst_strides);
    assert(error == height);
}

} // namespace renderer::texture

renderer::TextureCache::~TextureCache() {
    if (auto *context = static_cast<SwsContext *>(yuv_conversion_cache.sws_context)) {
        sws_freeContext(context);
        yuv_conversion_cache.sws_context = nullptr;
    }
}
