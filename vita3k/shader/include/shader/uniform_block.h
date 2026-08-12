#pragma once

#include <gxm/types.h>
#include <util/align.h>

#include <array>
#include <cstring>
#include <utility>

namespace shader {

struct RenderVertUniformBlock {
    std::array<float, 4> viewport_flip;
    float viewport_flag;
    float screen_width;
    float screen_height;
    float z_offset;
    float z_scale;
};

// used internally to identify the field by the shader recompiler
// it is put next to the RenderVertUniformBlock so we don't forget to update both fields every time
enum VertUniformFieldId : uint32_t {
    VERT_UNIFORM_viewport_flip,
    VERT_UNIFORM_viewport_flag,
    VERT_UNIFORM_screen_width,
    VERT_UNIFORM_screen_height,
    VERT_UNIFORM_z_offset,
    VERT_UNIFORM_z_scale
};

struct RenderFragUniformBlock {
    float back_disabled;
    float front_disabled;
    float writing_mask;
    float use_raw_image;
    float res_multiplier;
};

enum FragUniformFieldId : uint32_t {
    FRAG_UNIFORM_back_disabled,
    FRAG_UNIFORM_front_disabled,
    FRAG_UNIFORM_writing_mask,
    FRAG_UNIFORM_use_raw_image,
    FRAG_UNIFORM_res_multiplier
};

template <typename T>
struct UniformBlockExtended {
    T base_block;

    uint64_t buffer_addresses[SCE_GXM_REAL_MAX_UNIFORM_BUFFER] = {};
    std::pair<float, float> viewport_ratio[SCE_GXM_MAX_TEXTURE_UNITS] = {};
    std::pair<float, float> viewport_offset[SCE_GXM_MAX_TEXTURE_UNITS] = {};
    bool changed = false;
    // set by the backend: opengl declares the block as std140, vulkan as the standard layout
    bool std140 = false;
    uint16_t buffer_count = 0;
    uint16_t texture_count = 0;

    void set_buffer_count(uint16_t new_buffer_count) {
        if (new_buffer_count == buffer_count)
            return;

        changed = true;
        buffer_count = new_buffer_count;
    }

    void set_texture_count(uint16_t new_texture_count) {
        if (new_texture_count == texture_count)
            return;

        changed = true;
        texture_count = new_texture_count;
    }

    void set_buffer_address(int idx, uint64_t buffer_address) {
        if (buffer_addresses[idx] != buffer_address) {
            changed = true;
            buffer_addresses[idx] = buffer_address;
        }
    }

    // OpenGL reads this block as a std140 uniform block, and std140 rounds the alignment and the
    // stride of every array up to 16 bytes. Vulkan uses the tighter standard layout and keeps the
    // 8-byte elements, so the two backends do not lay the arrays out the same way.
    static constexpr uint32_t get_array_stride(bool std140) {
        return std140 ? 16 : 8;
    }

    static constexpr uint32_t get_buffer_addresses_offset(uint16_t buffer_count, uint16_t texture_count, bool std140) {
        return align(sizeof(T), get_array_stride(std140));
    }

    void set_viewport_ratio(int idx, const std::pair<float, float> &ratio) {
        if (viewport_ratio[idx] != ratio) {
            changed = true;
            viewport_ratio[idx] = ratio;
        }
    }

    static constexpr uint32_t get_viewport_ratio_offset(uint16_t buffer_count, uint16_t texture_count, bool std140) {
        return get_buffer_addresses_offset(buffer_count, texture_count, std140) + buffer_count * get_array_stride(std140);
    }

    void set_viewport_offset(int idx, const std::pair<float, float> &offset) {
        if (viewport_offset[idx] != offset) {
            changed = true;
            viewport_offset[idx] = offset;
        }
    }

    static constexpr uint32_t get_viewport_offset_offset(uint16_t buffer_count, uint16_t texture_count, bool std140) {
        return get_viewport_ratio_offset(buffer_count, texture_count, std140) + texture_count * get_array_stride(std140);
    }

    static constexpr uint32_t get_size(const uint16_t buffer_count, const uint16_t texture_count, bool std140) {
        return get_viewport_offset_offset(buffer_count, texture_count, std140) + texture_count * get_array_stride(std140);
    }

    uint32_t get_size() const {
        return get_size(buffer_count, texture_count, std140);
    }

    static constexpr uint32_t get_max_size() {
        // std140 is the roomier of the two layouts, so this fits either
        return get_size(SCE_GXM_REAL_MAX_UNIFORM_BUFFER, SCE_GXM_MAX_TEXTURE_UNITS, true);
    }

    void copy_to(uint8_t *buffer) {
        const uint32_t stride = get_array_stride(std140);

        memcpy(buffer, &base_block, sizeof(T));
        buffer += get_buffer_addresses_offset(buffer_count, texture_count, std140);

        // buffer addresses
        for (uint16_t i = 0; i < buffer_count; i++)
            memcpy(buffer + i * stride, &buffer_addresses[i], sizeof(buffer_addresses[0]));
        buffer += buffer_count * stride;

        // texture viewport fields
        for (uint16_t i = 0; i < texture_count; i++)
            memcpy(buffer + i * stride, &viewport_ratio[i], sizeof(viewport_ratio[0]));
        buffer += texture_count * stride;

        for (uint16_t i = 0; i < texture_count; i++)
            memcpy(buffer + i * stride, &viewport_offset[i], sizeof(viewport_offset[0]));

        changed = false;
    }
};

typedef UniformBlockExtended<RenderVertUniformBlock> RenderVertUniformBlockExtended;
typedef UniformBlockExtended<RenderFragUniformBlock> RenderFragUniformBlockExtended;

} // namespace shader
