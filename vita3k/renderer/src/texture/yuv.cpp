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

#include <util/log.h>

extern "C" {
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace renderer::texture {

// TEMPORARY DEBUG INSTRUMENTATION - remove once the video colour issue is found
// dumps an AV_PIX_FMT_RGB0 buffer (4 bytes per pixel, 4th byte ignored) as a binary ppm
void dbg_dump_rgb0_ppm(const char *name, const uint8_t *rgb0, uint32_t width, uint32_t height) {
    // the video fades in from black, so dump uploads from the middle where the logo is visible
    static int upload_index = -1;
    upload_index++;
    if (upload_index != 60 && upload_index != 120)
        return;

    const std::string file_name = fmt::format("{}_u{}.ppm", name, upload_index);
    std::ofstream f(file_name, std::ios::binary | std::ios::trunc);
    if (!f) {
        LOG_ERROR("[VDBG] cannot open {}", file_name);
        return;
    }
    const std::string header = fmt::format("P6\n{} {}\n255\n", width, height);
    f.write(header.data(), static_cast<std::streamsize>(header.size()));

    std::vector<uint8_t> row(static_cast<size_t>(width) * 3);
    for (uint32_t y = 0; y < height; y++) {
        for (uint32_t x = 0; x < width; x++) {
            row[x * 3 + 0] = rgb0[(static_cast<size_t>(y) * width + x) * 4 + 0];
            row[x * 3 + 1] = rgb0[(static_cast<size_t>(y) * width + x) * 4 + 1];
            row[x * 3 + 2] = rgb0[(static_cast<size_t>(y) * width + x) * 4 + 2];
        }
        f.write(reinterpret_cast<const char *>(row.data()), static_cast<std::streamsize>(row.size()));
    }
    LOG_INFO("[VDBG] wrote {}", std::filesystem::absolute(file_name).string());

    // report how the (undefined) 4th byte of AV_PIX_FMT_RGB0 actually came out
    uint32_t a_min = 255, a_max = 0;
    uint64_t a_sum = 0;
    const size_t n = static_cast<size_t>(width) * height;
    for (size_t i = 0; i < n; i++) {
        const uint8_t a = rgb0[i * 4 + 3];
        a_min = std::min<uint32_t>(a_min, a);
        a_max = std::max<uint32_t>(a_max, a);
        a_sum += a;
    }
    LOG_INFO("[VDBG] rgb0 4th byte (alpha): min={} max={} avg={}", a_min, a_max, a_sum / n);
}

static SwsContext *get_sws_context(YUVConversionCache &cache, size_t width, size_t height, bool is_p3) {
    bool recreate = false;
    auto *context = static_cast<SwsContext *>(cache.sws_context);
    if (cache.width != width || cache.height != height || cache.is_p3 != is_p3) {
        recreate = true;
        cache.width = width;
        cache.height = height;
        cache.is_p3 = is_p3;
    } else if (context == nullptr) {
        recreate = true;
    }

    if (recreate) {
        if (context != nullptr) {
            sws_freeContext(context);
            context = nullptr;
        }
        const AVPixelFormat format = is_p3 ? AV_PIX_FMT_YUV420P : AV_PIX_FMT_NV12;
        context = sws_getContext(width, height, format, width, height, AV_PIX_FMT_RGB0,
            0, nullptr, nullptr, nullptr);
        cache.sws_context = context;
    }
    return context;
}

void yuv420_texture_to_rgb(YUVConversionCache &cache, uint8_t *dst, const uint8_t *src, uint32_t width, uint32_t height, uint32_t layout_width, uint32_t layout_height, bool is_p3) {
    SwsContext *context = get_sws_context(cache, width, height, is_p3);
    assert(context);

    const uint8_t *slices[] = {
        src, // Y Slice
        src + layout_width * layout_height, // U(V for P2) Slice
        src + layout_width * layout_height + layout_width * layout_height / 4, // V Slice (for P3)
    };

    int strides[] = {
        static_cast<int>(width),
        static_cast<int>(width / 2),
        static_cast<int>(width / 2),
    };
    if (!is_p3) {
        // src only have two slices
        strides[1] = static_cast<int>(width);
        strides[2] = 0;
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
