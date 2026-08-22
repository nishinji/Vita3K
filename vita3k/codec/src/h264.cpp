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

#include <codec/state.h>

#include <util/log.h>

extern "C" {
#include <libavcodec/avcodec.h>
}

#include <cassert>

// ---------------------------------------------------------------------------
// TEMPORARY DEBUG INSTRUMENTATION - remove once the video colour issue is found
// ---------------------------------------------------------------------------
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace video_dbg {

constexpr bool enabled = true;

// which decoded frame indices to dump (logo.mp4 is ~30fps and fades in from black,
// so frame 60 / 120 land in the middle where the logo is fully drawn)
inline bool should_dump(int frame_index) {
    return frame_index == 60 || frame_index == 120;
}

inline void yuv_to_rgb(int y, int u, int v, uint8_t *out) {
    const float a = 1.164f * static_cast<float>(y - 16);
    const auto cl = [](float x) { return static_cast<uint8_t>(std::clamp(x, 0.0f, 255.0f)); };
    out[0] = cl(a + 1.596f * static_cast<float>(v - 128));
    out[1] = cl(a - 0.813f * static_cast<float>(v - 128) - 0.391f * static_cast<float>(u - 128));
    out[2] = cl(a + 2.018f * static_cast<float>(u - 128));
}

inline void write_ppm(const std::string &name, const std::vector<uint8_t> &rgb, uint32_t w, uint32_t h) {
    std::ofstream f(name, std::ios::binary | std::ios::trunc);
    if (!f) {
        LOG_ERROR("[VDBG] cannot open {}", name);
        return;
    }
    const std::string header = fmt::format("P6\n{} {}\n255\n", w, h);
    f.write(header.data(), static_cast<std::streamsize>(header.size()));
    f.write(reinterpret_cast<const char *>(rgb.data()), static_cast<std::streamsize>(rgb.size()));
    LOG_INFO("[VDBG] wrote {} ({}x{})", std::filesystem::absolute(name).string(), w, h);
}

// dump an AVFrame (planar yuv420p as produced by ffmpeg)
inline void dump_avframe(const std::string &name, const AVFrame *frame) {
    const uint32_t w = static_cast<uint32_t>(frame->width);
    const uint32_t h = static_cast<uint32_t>(frame->height);
    std::vector<uint8_t> rgb(static_cast<size_t>(w) * h * 3);
    for (uint32_t y = 0; y < h; y++) {
        for (uint32_t x = 0; x < w; x++) {
            const int Y = frame->data[0][frame->linesize[0] * y + x];
            const int U = frame->data[1][frame->linesize[1] * (y / 2) + x / 2];
            const int V = frame->data[2][frame->linesize[2] * (y / 2) + x / 2];
            yuv_to_rgb(Y, U, V, &rgb[(static_cast<size_t>(y) * w + x) * 3]);
        }
    }
    write_ppm(name, rgb, w, h);
}

// dump exactly what we wrote into the guest buffer
inline void dump_guest_buffer(const std::string &name, const uint8_t *buf, uint32_t w, uint32_t h, bool is_p3) {
    std::vector<uint8_t> rgb(static_cast<size_t>(w) * h * 3);
    const uint8_t *plane_y = buf;
    const uint8_t *plane_1 = buf + static_cast<size_t>(w) * h;
    const uint8_t *plane_2 = plane_1 + static_cast<size_t>(w) * h / 4;
    for (uint32_t y = 0; y < h; y++) {
        for (uint32_t x = 0; x < w; x++) {
            const int Y = plane_y[static_cast<size_t>(w) * y + x];
            int U, V;
            if (is_p3) {
                U = plane_1[static_cast<size_t>(w / 2) * (y / 2) + x / 2];
                V = plane_2[static_cast<size_t>(w / 2) * (y / 2) + x / 2];
            } else {
                U = plane_1[static_cast<size_t>(w) * (y / 2) + (x / 2) * 2 + 0];
                V = plane_1[static_cast<size_t>(w) * (y / 2) + (x / 2) * 2 + 1];
            }
            yuv_to_rgb(Y, U, V, &rgb[(static_cast<size_t>(y) * w + x) * 3]);
        }
    }
    write_ppm(name, rgb, w, h);
}

} // namespace video_dbg

void copy_yuv_data_from_frame(AVFrame *frame, uint8_t *dest, const uint32_t width, const uint32_t height, bool is_p3) {
    for (size_t i = 0; i < height; i++) {
        memcpy(dest, &frame->data[0][frame->linesize[0] * i], width);
        dest += width;
    }

    if (is_p3) {
        for (size_t i = 0; i < height / 2; i++) {
            memcpy(dest, &frame->data[1][frame->linesize[1] * i], width / 2);
            dest += width / 2;
        }
        for (size_t i = 0; i < height / 2; i++) {
            memcpy(dest, &frame->data[2][frame->linesize[2] * i], width / 2);
            dest += width / 2;
        }
    } else {
        // p2 format, U and V are interleaved
        for (size_t i = 0; i < height / 2; i++) {
            const uint8_t *src_u = &frame->data[1][frame->linesize[1] * i];
            const uint8_t *src_v = &frame->data[2][frame->linesize[2] * i];
            for (size_t j = 0; j < width / 2; j++) {
                dest[0] = src_u[j];
                dest[1] = src_v[j];
                dest += 2;
            }
        }
    }
}

uint32_t H264DecoderState::buffer_size(DecoderSize size) {
    return size.width * size.height * 3 / 2;
}

uint32_t H264DecoderState::get(DecoderQuery query) {
    switch (query) {
    case DecoderQuery::WIDTH: return context->width;
    case DecoderQuery::HEIGHT: return context->height;
    default: return 0;
    }
}

bool H264DecoderState::send(const uint8_t *data, uint32_t size) {
    std::lock_guard<std::mutex> lock(codec_mutex);

    int error = 0;

    std::vector<uint8_t> au_frame(size + AV_INPUT_BUFFER_PADDING_SIZE);
    memcpy(au_frame.data(), data, size);

    AVPacket *packet = av_packet_alloc();
    if (!packet) {
        LOG_ERROR("Error allocating H264 packet.");
        return false;
    }
    error = av_parser_parse2(
        parser, // AVCodecParserContext *s,
        context, // AVCodecContext *avctx,
        &packet->data, // uint8_t **poutbuf,
        &packet->size, // int *poutbuf_size,
        au_frame.data(), // const uint8_t *buf,
        size, // int buf_size,
        pts == ~0ull ? AV_NOPTS_VALUE : pts, // int64_t pts,
        dts == ~0ull ? AV_NOPTS_VALUE : dts, // int64_t dts,
        0 // int64_t pos
    );
    if (error < 0) {
        LOG_WARN("Error parsing H264 packet: {}.", codec_error_name(error));
        av_packet_free(&packet);
        return false;
    }

    packet->pts = parser->pts;
    packet->dts = parser->dts;

    error = avcodec_send_packet(context, packet);
    av_packet_free(&packet);
    if (error < 0) {
        LOG_WARN("Error sending H264 packet: {}.", codec_error_name(error));
        return false;
    }

    return true;
}

bool H264DecoderState::receive(uint8_t *data, DecoderSize *size) {
    AVFrame *frame = av_frame_alloc();

    int error = avcodec_receive_frame(context, frame);
    if (error < 0) {
        LOG_WARN("Error receiving H264 frame: {}.", codec_error_name(error));
        av_frame_free(&frame);
        return false;
    }

    if (data) {
        copy_yuv_data_from_frame(frame, data, width_in, height_in, output_yuvp3);
    }

    if constexpr (video_dbg::enabled) {
        static int frame_index = 0;
        LOG_INFO_ONCE("[VDBG h264] avframe {}x{} fmt={} linesize={}/{}/{} range={} colorspace={} | dest {}x{} p3={}",
            frame->width, frame->height, static_cast<int>(frame->format),
            frame->linesize[0], frame->linesize[1], frame->linesize[2],
            static_cast<int>(frame->color_range), static_cast<int>(frame->colorspace),
            width_in, height_in, output_yuvp3);
        // the video fades in from black, so dump frames from the middle where the logo is visible
        if (video_dbg::should_dump(frame_index) && frame->width > 0 && frame->data[1] && frame->data[2]) {
            video_dbg::dump_avframe(fmt::format("vdbg_f{}_1_ffmpeg.ppm", frame_index), frame);
            if (data && width_in > 0 && height_in > 0)
                video_dbg::dump_guest_buffer(fmt::format("vdbg_f{}_2_guestbuf.ppm", frame_index), data, width_in, height_in, output_yuvp3);
        }
        frame_index++;
    }

    if (size) {
        *size = { { static_cast<uint32_t>(context->width), static_cast<uint32_t>(context->height) } };
    }

    width_out = frame->width;
    height_out = frame->height;

    pts_out = frame->pts;

    av_frame_free(&frame);
    return true;
}

void H264DecoderState::configure(void *options) {
    auto *opt = static_cast<H264DecoderOptions *>(options);

    pts = static_cast<uint64_t>(opt->pts_upper) << 32u | static_cast<uint64_t>(opt->pts_lower);
    dts = static_cast<uint64_t>(opt->dts_upper) << 32u | static_cast<uint64_t>(opt->dts_lower);
}

void H264DecoderState::set_res(const uint32_t width, const uint32_t height) {
    width_in = width;
    height_in = height;
}

void H264DecoderState::get_res(uint32_t &width, uint32_t &height) {
    width = width_out;
    height = height_out;
}

void H264DecoderState::get_pts(uint32_t &upper, uint32_t &lower) {
    upper = pts_out >> 32u;
    lower = pts_out & 0xFFFFFFFF;
}

void H264DecoderState::set_output_format(bool is_yuv_p3) {
    this->output_yuvp3 = is_yuv_p3;
}

H264DecoderState::H264DecoderState(uint32_t width, uint32_t height) {
    const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    assert(codec);

    parser = av_parser_init(codec->id);
    assert(parser);
    parser->flags |= PARSER_FLAG_COMPLETE_FRAMES;

    context = avcodec_alloc_context3(codec);
    assert(context);
    context->width = width;
    context->height = height;

    int result = avcodec_open2(context, codec, nullptr);
    assert(result == 0);
}

H264DecoderState::~H264DecoderState() {
    av_parser_close(parser);
}
