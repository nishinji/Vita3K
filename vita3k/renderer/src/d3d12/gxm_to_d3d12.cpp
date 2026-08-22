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

#include <gxm/functions.h>
#include <util/log.h>

namespace renderer::d3d12 {

// Vertex attributes

DXGI_FORMAT translate_attribute_format(SceGxmAttributeFormat format, unsigned int component_count, bool is_integer, bool is_signed) {
    if (component_count == 0 || component_count > 4 || format > SCE_GXM_ATTRIBUTE_FORMAT_UNTYPED) {
        LOG_ERROR("Unsupported attribute format {}x{}", log_hex(format), component_count);
        return DXGI_FORMAT_UNKNOWN;
    }

    // DXGI has no three-component 8- or 16-bit formats and no scaled formats at
    // all. UNKNOWN entries mark the combinations the caller has to widen to four
    // components; is_attribute_format_supported reports the same thing.
    static constexpr DXGI_FORMAT formats_integer[][4] = {
        /*U8*/ { DXGI_FORMAT_R8_UINT, DXGI_FORMAT_R8G8_UINT, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_R8G8B8A8_UINT },
        /*S8*/ { DXGI_FORMAT_R8_SINT, DXGI_FORMAT_R8G8_SINT, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_R8G8B8A8_SINT },
        /*U16*/ { DXGI_FORMAT_R16_UINT, DXGI_FORMAT_R16G16_UINT, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_R16G16B16A16_UINT },
        /*S16*/ { DXGI_FORMAT_R16_SINT, DXGI_FORMAT_R16G16_SINT, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_R16G16B16A16_SINT },
    };

    static constexpr DXGI_FORMAT formats_float[][4] = {
        /*U8N*/ { DXGI_FORMAT_R8_UNORM, DXGI_FORMAT_R8G8_UNORM, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_R8G8B8A8_UNORM },
        /*S8N*/ { DXGI_FORMAT_R8_SNORM, DXGI_FORMAT_R8G8_SNORM, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_R8G8B8A8_SNORM },
        /*U16N*/ { DXGI_FORMAT_R16_UNORM, DXGI_FORMAT_R16G16_UNORM, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_R16G16B16A16_UNORM },
        /*S16N*/ { DXGI_FORMAT_R16_SNORM, DXGI_FORMAT_R16G16_SNORM, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_R16G16B16A16_SNORM },
        /*F16*/ { DXGI_FORMAT_R16_FLOAT, DXGI_FORMAT_R16G16_FLOAT, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_R16G16B16A16_FLOAT },
        /*F32*/ { DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R32G32_FLOAT, DXGI_FORMAT_R32G32B32_FLOAT, DXGI_FORMAT_R32G32B32A32_FLOAT },
    };

    static constexpr DXGI_FORMAT formats_untyped[][4] = {
        /*unsigned*/ { DXGI_FORMAT_R32_UINT, DXGI_FORMAT_R32G32_UINT, DXGI_FORMAT_R32G32B32_UINT, DXGI_FORMAT_R32G32B32A32_UINT },
        /*signed*/ { DXGI_FORMAT_R32_SINT, DXGI_FORMAT_R32G32_SINT, DXGI_FORMAT_R32G32B32_SINT, DXGI_FORMAT_R32G32B32A32_SINT },
    };

    const int format_idx = format;
    if (format_idx < SCE_GXM_ATTRIBUTE_FORMAT_U8N) {
        // Scaled formats do not exist in DXGI, so the integer table is used for
        // both cases and the shader does the int-to-float conversion.
        return formats_integer[format_idx][component_count - 1];
    } else if (format == SCE_GXM_ATTRIBUTE_FORMAT_UNTYPED) {
        return formats_untyped[is_signed][component_count - 1];
    } else {
        return formats_float[format_idx - SCE_GXM_ATTRIBUTE_FORMAT_U8N][component_count - 1];
    }
}

bool is_attribute_format_supported(SceGxmAttributeFormat format, unsigned int component_count) {
    return translate_attribute_format(format, component_count, true, false) != DXGI_FORMAT_UNKNOWN;
}

// Blending

D3D12_BLEND translate_blend_factor(SceGxmBlendFactor blend_factor) {
    switch (blend_factor) {
    case SCE_GXM_BLEND_FACTOR_ZERO:
        return D3D12_BLEND_ZERO;
    case SCE_GXM_BLEND_FACTOR_ONE:
        return D3D12_BLEND_ONE;
    case SCE_GXM_BLEND_FACTOR_SRC_COLOR:
        return D3D12_BLEND_SRC_COLOR;
    case SCE_GXM_BLEND_FACTOR_ONE_MINUS_SRC_COLOR:
        return D3D12_BLEND_INV_SRC_COLOR;
    case SCE_GXM_BLEND_FACTOR_SRC_ALPHA:
        return D3D12_BLEND_SRC_ALPHA;
    case SCE_GXM_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA:
        return D3D12_BLEND_INV_SRC_ALPHA;
    case SCE_GXM_BLEND_FACTOR_DST_COLOR:
        return D3D12_BLEND_DEST_COLOR;
    case SCE_GXM_BLEND_FACTOR_ONE_MINUS_DST_COLOR:
        return D3D12_BLEND_INV_DEST_COLOR;
    case SCE_GXM_BLEND_FACTOR_DST_ALPHA:
        return D3D12_BLEND_DEST_ALPHA;
    case SCE_GXM_BLEND_FACTOR_ONE_MINUS_DST_ALPHA:
        return D3D12_BLEND_INV_DEST_ALPHA;
    case SCE_GXM_BLEND_FACTOR_SRC_ALPHA_SATURATE:
        return D3D12_BLEND_SRC_ALPHA_SAT;
    case SCE_GXM_BLEND_FACTOR_DST_ALPHA_SATURATE:
        // Same approximation the vulkan backend makes: there is no destination
        // alpha saturate factor in either API.
        return D3D12_BLEND_DEST_ALPHA;
    default:
        LOG_ERROR("Unknown blend factor: {}", fmt::underlying(blend_factor));
        return D3D12_BLEND_ONE;
    }
}

D3D12_BLEND translate_blend_factor_alpha(SceGxmBlendFactor blend_factor) {
    // D3D12 rejects colour-channel factors in the alpha slot, so fold each one
    // onto the matching alpha factor. GXM applies the alpha channel of the
    // colour factor here, which is exactly what these map to.
    switch (blend_factor) {
    case SCE_GXM_BLEND_FACTOR_SRC_COLOR:
        return D3D12_BLEND_SRC_ALPHA;
    case SCE_GXM_BLEND_FACTOR_ONE_MINUS_SRC_COLOR:
        return D3D12_BLEND_INV_SRC_ALPHA;
    case SCE_GXM_BLEND_FACTOR_DST_COLOR:
        return D3D12_BLEND_DEST_ALPHA;
    case SCE_GXM_BLEND_FACTOR_ONE_MINUS_DST_COLOR:
        return D3D12_BLEND_INV_DEST_ALPHA;
    default:
        return translate_blend_factor(blend_factor);
    }
}

D3D12_BLEND_OP translate_blend_func(SceGxmBlendFunc blend_func) {
    switch (blend_func) {
    case SCE_GXM_BLEND_FUNC_NONE:
    case SCE_GXM_BLEND_FUNC_ADD:
        return D3D12_BLEND_OP_ADD;
    case SCE_GXM_BLEND_FUNC_SUBTRACT:
        return D3D12_BLEND_OP_SUBTRACT;
    case SCE_GXM_BLEND_FUNC_REVERSE_SUBTRACT:
        return D3D12_BLEND_OP_REV_SUBTRACT;
    case SCE_GXM_BLEND_FUNC_MIN:
        return D3D12_BLEND_OP_MIN;
    case SCE_GXM_BLEND_FUNC_MAX:
        return D3D12_BLEND_OP_MAX;
    default:
        LOG_ERROR("Unknown blend function: {}", fmt::underlying(blend_func));
        return D3D12_BLEND_OP_ADD;
    }
}

// Rasterisation

D3D12_PRIMITIVE_TOPOLOGY translate_primitive(SceGxmPrimitiveType primitive) {
    switch (primitive) {
    case SCE_GXM_PRIMITIVE_TRIANGLES:
        return D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    case SCE_GXM_PRIMITIVE_TRIANGLE_STRIP:
        return D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
    case SCE_GXM_PRIMITIVE_TRIANGLE_FAN:
        // D3D12 dropped triangle fans entirely; the index buffer is rewritten
        // into a list by the draw path.
        return D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    case SCE_GXM_PRIMITIVE_LINES:
        return D3D_PRIMITIVE_TOPOLOGY_LINELIST;
    case SCE_GXM_PRIMITIVE_POINTS:
        return D3D_PRIMITIVE_TOPOLOGY_POINTLIST;
    case SCE_GXM_PRIMITIVE_TRIANGLE_EDGES:
        LOG_ERROR_ONCE("Unsupported primitive type SCE_GXM_PRIMITIVE_TRIANGLE_EDGES");
        return D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    default:
        LOG_ERROR("Unknown primitive type: {}", fmt::underlying(primitive));
        return D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    }
}

D3D12_PRIMITIVE_TOPOLOGY_TYPE translate_primitive_type(SceGxmPrimitiveType primitive) {
    switch (primitive) {
    case SCE_GXM_PRIMITIVE_POINTS:
        return D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
    case SCE_GXM_PRIMITIVE_LINES:
        return D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
    default:
        return D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    }
}

D3D12_COMPARISON_FUNC translate_depth_func(SceGxmDepthFunc depth_func) {
    switch (depth_func) {
    case SCE_GXM_DEPTH_FUNC_NEVER:
        return D3D12_COMPARISON_FUNC_NEVER;
    case SCE_GXM_DEPTH_FUNC_LESS:
        return D3D12_COMPARISON_FUNC_LESS;
    case SCE_GXM_DEPTH_FUNC_EQUAL:
        return D3D12_COMPARISON_FUNC_EQUAL;
    case SCE_GXM_DEPTH_FUNC_LESS_EQUAL:
        return D3D12_COMPARISON_FUNC_LESS_EQUAL;
    case SCE_GXM_DEPTH_FUNC_GREATER:
        return D3D12_COMPARISON_FUNC_GREATER;
    case SCE_GXM_DEPTH_FUNC_NOT_EQUAL:
        return D3D12_COMPARISON_FUNC_NOT_EQUAL;
    case SCE_GXM_DEPTH_FUNC_GREATER_EQUAL:
        return D3D12_COMPARISON_FUNC_GREATER_EQUAL;
    case SCE_GXM_DEPTH_FUNC_ALWAYS:
        return D3D12_COMPARISON_FUNC_ALWAYS;
    default:
        LOG_ERROR("Unknown depth function: {}", fmt::underlying(depth_func));
        return D3D12_COMPARISON_FUNC_ALWAYS;
    }
}

D3D12_FILL_MODE translate_polygon_mode(SceGxmPolygonMode polygon_mode) {
    switch (polygon_mode) {
    case SCE_GXM_POLYGON_MODE_TRIANGLE_FILL:
    case SCE_GXM_POLYGON_MODE_POINT_10UV:
    case SCE_GXM_POLYGON_MODE_POINT:
    case SCE_GXM_POLYGON_MODE_POINT_01UV:
        return D3D12_FILL_MODE_SOLID;
    case SCE_GXM_POLYGON_MODE_TRIANGLE_LINE:
    case SCE_GXM_POLYGON_MODE_LINE:
        return D3D12_FILL_MODE_WIREFRAME;
    default:
        LOG_ERROR("Unknown polygon mode: {}", fmt::underlying(polygon_mode));
        return D3D12_FILL_MODE_SOLID;
    }
}

D3D12_CULL_MODE translate_cull_mode(SceGxmCullMode cull_mode) {
    switch (cull_mode) {
    case SCE_GXM_CULL_CCW:
        return D3D12_CULL_MODE_FRONT;
    case SCE_GXM_CULL_CW:
        return D3D12_CULL_MODE_BACK;
    case SCE_GXM_CULL_NONE:
        return D3D12_CULL_MODE_NONE;
    default:
        LOG_ERROR("Unknown cull mode: {}", fmt::underlying(cull_mode));
        return D3D12_CULL_MODE_NONE;
    }
}

D3D12_COMPARISON_FUNC translate_stencil_func(SceGxmStencilFunc stencil_func) {
    switch (stencil_func) {
    case SCE_GXM_STENCIL_FUNC_NEVER:
        return D3D12_COMPARISON_FUNC_NEVER;
    case SCE_GXM_STENCIL_FUNC_LESS:
        return D3D12_COMPARISON_FUNC_LESS;
    case SCE_GXM_STENCIL_FUNC_EQUAL:
        return D3D12_COMPARISON_FUNC_EQUAL;
    case SCE_GXM_STENCIL_FUNC_LESS_EQUAL:
        return D3D12_COMPARISON_FUNC_LESS_EQUAL;
    case SCE_GXM_STENCIL_FUNC_GREATER:
        return D3D12_COMPARISON_FUNC_GREATER;
    case SCE_GXM_STENCIL_FUNC_NOT_EQUAL:
        return D3D12_COMPARISON_FUNC_NOT_EQUAL;
    case SCE_GXM_STENCIL_FUNC_GREATER_EQUAL:
        return D3D12_COMPARISON_FUNC_GREATER_EQUAL;
    case SCE_GXM_STENCIL_FUNC_ALWAYS:
        return D3D12_COMPARISON_FUNC_ALWAYS;
    default:
        LOG_ERROR("Unknown stencil function: {}", fmt::underlying(stencil_func));
        return D3D12_COMPARISON_FUNC_ALWAYS;
    }
}

D3D12_STENCIL_OP translate_stencil_op(SceGxmStencilOp stencil_op) {
    switch (stencil_op) {
    case SCE_GXM_STENCIL_OP_KEEP:
        return D3D12_STENCIL_OP_KEEP;
    case SCE_GXM_STENCIL_OP_ZERO:
        return D3D12_STENCIL_OP_ZERO;
    case SCE_GXM_STENCIL_OP_REPLACE:
        return D3D12_STENCIL_OP_REPLACE;
    case SCE_GXM_STENCIL_OP_INCR:
        return D3D12_STENCIL_OP_INCR_SAT;
    case SCE_GXM_STENCIL_OP_DECR:
        return D3D12_STENCIL_OP_DECR_SAT;
    case SCE_GXM_STENCIL_OP_INVERT:
        return D3D12_STENCIL_OP_INVERT;
    case SCE_GXM_STENCIL_OP_INCR_WRAP:
        return D3D12_STENCIL_OP_INCR;
    case SCE_GXM_STENCIL_OP_DECR_WRAP:
        return D3D12_STENCIL_OP_DECR;
    default:
        LOG_ERROR("Unknown stencil operation: {}", fmt::underlying(stencil_op));
        return D3D12_STENCIL_OP_KEEP;
    }
}

DepthStencilFormats translate_depth_stencil_format(SceGxmDepthStencilFormat format) {
    // Depth surfaces are created typeless so the same resource can be bound as a
    // depth target and sampled as a texture, which GXM programs do freely.
    switch (format) {
    case SCE_GXM_DEPTH_STENCIL_FORMAT_D16:
        return {
            .resource = DXGI_FORMAT_R16_TYPELESS,
            .dsv = DXGI_FORMAT_D16_UNORM,
            .depth_srv = DXGI_FORMAT_R16_UNORM,
            .stencil_srv = DXGI_FORMAT_UNKNOWN,
        };

    case SCE_GXM_DEPTH_STENCIL_FORMAT_DF32:
    case SCE_GXM_DEPTH_STENCIL_FORMAT_DF32M:
        return {
            .resource = DXGI_FORMAT_R32_TYPELESS,
            .dsv = DXGI_FORMAT_D32_FLOAT,
            .depth_srv = DXGI_FORMAT_R32_FLOAT,
            .stencil_srv = DXGI_FORMAT_UNKNOWN,
        };

    case SCE_GXM_DEPTH_STENCIL_FORMAT_S8D24:
        return {
            .resource = DXGI_FORMAT_R24G8_TYPELESS,
            .dsv = DXGI_FORMAT_D24_UNORM_S8_UINT,
            .depth_srv = DXGI_FORMAT_R24_UNORM_X8_TYPELESS,
            .stencil_srv = DXGI_FORMAT_X24_TYPELESS_G8_UINT,
        };

    case SCE_GXM_DEPTH_STENCIL_FORMAT_DF32_S8:
    case SCE_GXM_DEPTH_STENCIL_FORMAT_DF32M_S8:
    case SCE_GXM_DEPTH_STENCIL_FORMAT_S8:
        // A stencil-only surface still needs a depth plane in D3D12, so the
        // combined format is used and the depth half simply goes unread.
        return {
            .resource = DXGI_FORMAT_R32G8X24_TYPELESS,
            .dsv = DXGI_FORMAT_D32_FLOAT_S8X24_UINT,
            .depth_srv = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS,
            .stencil_srv = DXGI_FORMAT_X32_TYPELESS_G8X24_UINT,
        };

    default:
        LOG_ERROR("Unknown depth stencil format {}", log_hex(format));
        return {
            .resource = DXGI_FORMAT_R32G8X24_TYPELESS,
            .dsv = DXGI_FORMAT_D32_FLOAT_S8X24_UINT,
            .depth_srv = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS,
            .stencil_srv = DXGI_FORMAT_X32_TYPELESS_G8X24_UINT,
        };
    }
}

// Component swizzles
//
// The vulkan backend expresses these as a vk::ComponentMapping; D3D12 folds the
// same information into the Shader4ComponentMapping field of an SRV, so the
// tables below are the direct equivalents of the ones in gxm_to_vulkan.cpp.
namespace {

constexpr uint32_t SWZ_R = D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_0;
constexpr uint32_t SWZ_G = D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_1;
constexpr uint32_t SWZ_B = D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_2;
constexpr uint32_t SWZ_A = D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_3;
constexpr uint32_t SWZ_0 = D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_0;
constexpr uint32_t SWZ_1 = D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_1;

constexpr uint32_t encode(uint32_t r, uint32_t g, uint32_t b, uint32_t a) {
    return D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(r, g, b, a);
}

constexpr uint32_t swizzle_identity = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;

// SceGxmSwizzle1Mode
constexpr uint32_t swizzle_r001 = encode(SWZ_R, SWZ_0, SWZ_0, SWZ_1);
constexpr uint32_t swizzle_r000 = encode(SWZ_R, SWZ_0, SWZ_0, SWZ_0);
constexpr uint32_t swizzle_r111 = encode(SWZ_R, SWZ_1, SWZ_1, SWZ_1);
constexpr uint32_t swizzle_rrrr = encode(SWZ_R, SWZ_R, SWZ_R, SWZ_R);
constexpr uint32_t swizzle_rrr0 = encode(SWZ_R, SWZ_R, SWZ_R, SWZ_0);
constexpr uint32_t swizzle_rrr1 = encode(SWZ_R, SWZ_R, SWZ_R, SWZ_1);
constexpr uint32_t swizzle_000r = encode(SWZ_0, SWZ_0, SWZ_0, SWZ_R);
constexpr uint32_t swizzle_111r = encode(SWZ_1, SWZ_1, SWZ_1, SWZ_R);

// SceGxmSwizzle2Mode
constexpr uint32_t swizzle_rg01 = encode(SWZ_R, SWZ_G, SWZ_0, SWZ_1);
constexpr uint32_t swizzle_gr01 = encode(SWZ_G, SWZ_R, SWZ_0, SWZ_1);
constexpr uint32_t swizzle_rg00 = encode(SWZ_R, SWZ_G, SWZ_0, SWZ_0);
constexpr uint32_t swizzle_rrrg = encode(SWZ_R, SWZ_R, SWZ_R, SWZ_G);
constexpr uint32_t swizzle_gggr = encode(SWZ_G, SWZ_G, SWZ_G, SWZ_R);
constexpr uint32_t swizzle_rgrg = encode(SWZ_R, SWZ_G, SWZ_R, SWZ_G);
constexpr uint32_t swizzle_gr00 = encode(SWZ_G, SWZ_R, SWZ_0, SWZ_0);

// SceGxmSwizzle3Mode
constexpr uint32_t swizzle_bgr1 = encode(SWZ_B, SWZ_G, SWZ_R, SWZ_1);
constexpr uint32_t swizzle_rgb1 = encode(SWZ_R, SWZ_G, SWZ_B, SWZ_1);

// SceGxmSwizzle4Mode
constexpr uint32_t swizzle_rgba = encode(SWZ_R, SWZ_G, SWZ_B, SWZ_A);
constexpr uint32_t swizzle_bgra = encode(SWZ_B, SWZ_G, SWZ_R, SWZ_A);
constexpr uint32_t swizzle_abgr = encode(SWZ_A, SWZ_B, SWZ_G, SWZ_R);
constexpr uint32_t swizzle_gbar = encode(SWZ_G, SWZ_B, SWZ_A, SWZ_R);
constexpr uint32_t swizzle_abg1 = encode(SWZ_A, SWZ_B, SWZ_G, SWZ_1);
constexpr uint32_t swizzle_gba1 = encode(SWZ_G, SWZ_B, SWZ_A, SWZ_1);
constexpr uint32_t swizzle_argb = encode(SWZ_A, SWZ_R, SWZ_G, SWZ_B);
constexpr uint32_t swizzle_grab = encode(SWZ_G, SWZ_R, SWZ_A, SWZ_B);
constexpr uint32_t swizzle_arg1 = encode(SWZ_A, SWZ_R, SWZ_G, SWZ_1);
constexpr uint32_t swizzle_gra1 = encode(SWZ_G, SWZ_R, SWZ_A, SWZ_1);

} // namespace

namespace color {

static uint32_t translate_swizzle1(SceGxmColorSwizzle1Mode mode) {
    switch (mode) {
    case SCE_GXM_COLOR_SWIZZLE1_R:
        return swizzle_r001;
    default:
        LOG_ERROR("Unknown swizzle mode {}", log_hex(mode));
        return swizzle_identity;
    }
}

static uint32_t translate_swizzle2(SceGxmColorSwizzle2Mode mode) {
    switch (mode) {
    case SCE_GXM_COLOR_SWIZZLE2_GR:
        return swizzle_rg01;
    case SCE_GXM_COLOR_SWIZZLE2_RG:
        return swizzle_gr01;
    default:
        LOG_ERROR("Unknown swizzle mode {}", log_hex(mode));
        return swizzle_identity;
    }
}

static uint32_t translate_swizzle3(SceGxmColorSwizzle3Mode mode) {
    switch (mode) {
    case SCE_GXM_COLOR_SWIZZLE3_BGR:
        return swizzle_rgb1;
    case SCE_GXM_COLOR_SWIZZLE3_RGB:
        return swizzle_bgr1;
    default:
        LOG_ERROR("Unknown swizzle mode {}", log_hex(mode));
        return swizzle_identity;
    }
}

// Used when the DXGI format itself already has a BGR layout.
static uint32_t translate_swizzle3_bgr(SceGxmColorSwizzle3Mode mode) {
    switch (mode) {
    case SCE_GXM_COLOR_SWIZZLE3_BGR:
        return swizzle_bgr1;
    case SCE_GXM_COLOR_SWIZZLE3_RGB:
        return swizzle_rgb1;
    default:
        LOG_ERROR("Unknown swizzle mode {}", log_hex(mode));
        return swizzle_identity;
    }
}

static uint32_t translate_swizzle4(SceGxmColorSwizzle4Mode mode) {
    switch (mode) {
    case SCE_GXM_COLOR_SWIZZLE4_ABGR:
        return swizzle_rgba;
    case SCE_GXM_COLOR_SWIZZLE4_ARGB:
        return swizzle_bgra;
    case SCE_GXM_COLOR_SWIZZLE4_RGBA:
        return swizzle_abgr;
    case SCE_GXM_COLOR_SWIZZLE4_BGRA:
        return swizzle_gbar;
    default:
        LOG_ERROR("Unknown swizzle mode {}", log_hex(mode));
        return swizzle_identity;
    }
}

// Used when the DXGI format itself already has an ABGR layout.
static uint32_t translate_swizzle4_abgr(SceGxmColorSwizzle4Mode mode) {
    switch (mode) {
    case SCE_GXM_COLOR_SWIZZLE4_ABGR:
        return swizzle_abgr;
    case SCE_GXM_COLOR_SWIZZLE4_ARGB:
        return swizzle_gbar;
    case SCE_GXM_COLOR_SWIZZLE4_RGBA:
        return swizzle_rgba;
    case SCE_GXM_COLOR_SWIZZLE4_BGRA:
        return swizzle_bgra;
    default:
        LOG_ERROR("Unknown swizzle mode {}", log_hex(mode));
        return swizzle_identity;
    }
}

uint32_t translate_swizzle(SceGxmColorFormat format) {
    const SceGxmColorBaseFormat base_format = gxm::get_base_format(format);
    const uint32_t swizzle = format & SCE_GXM_COLOR_SWIZZLE_MASK;
    switch (base_format) {
    case SCE_GXM_COLOR_BASE_FORMAT_U8:
    case SCE_GXM_COLOR_BASE_FORMAT_S8:
    case SCE_GXM_COLOR_BASE_FORMAT_U16:
    case SCE_GXM_COLOR_BASE_FORMAT_S16:
    case SCE_GXM_COLOR_BASE_FORMAT_F16:
    case SCE_GXM_COLOR_BASE_FORMAT_F32:
        return translate_swizzle1(static_cast<SceGxmColorSwizzle1Mode>(swizzle));

    case SCE_GXM_COLOR_BASE_FORMAT_U8U8:
    case SCE_GXM_COLOR_BASE_FORMAT_S8S8:
    case SCE_GXM_COLOR_BASE_FORMAT_U16U16:
    case SCE_GXM_COLOR_BASE_FORMAT_S16S16:
    case SCE_GXM_COLOR_BASE_FORMAT_F16F16:
    case SCE_GXM_COLOR_BASE_FORMAT_F32F32:
        return translate_swizzle2(static_cast<SceGxmColorSwizzle2Mode>(swizzle));

    case SCE_GXM_COLOR_BASE_FORMAT_U8U8U8:
    case SCE_GXM_COLOR_BASE_FORMAT_F11F11F10:
    case SCE_GXM_COLOR_BASE_FORMAT_SE5M9M9M9:
        return translate_swizzle3(static_cast<SceGxmColorSwizzle3Mode>(swizzle));

    case SCE_GXM_COLOR_BASE_FORMAT_U5U6U5:
        return translate_swizzle3_bgr(static_cast<SceGxmColorSwizzle3Mode>(swizzle));

    case SCE_GXM_COLOR_BASE_FORMAT_U8U8U8U8:
    case SCE_GXM_COLOR_BASE_FORMAT_S8S8S8S8:
    case SCE_GXM_COLOR_BASE_FORMAT_F16F16F16F16:
    // TODO: the swizzle for the following formats is not fully supported
    case SCE_GXM_COLOR_BASE_FORMAT_U2U10U10U10:
    case SCE_GXM_COLOR_BASE_FORMAT_U2F10F10F10:
        return translate_swizzle4(static_cast<SceGxmColorSwizzle4Mode>(swizzle));

    case SCE_GXM_COLOR_BASE_FORMAT_U4U4U4U4:
    // TODO: the swizzle for the following format is not fully supported
    case SCE_GXM_COLOR_BASE_FORMAT_U1U5U5U5:
        return translate_swizzle4_abgr(static_cast<SceGxmColorSwizzle4Mode>(swizzle));

    default:
        LOG_ERROR("Unknown format {}", log_hex(base_format));
        return swizzle_identity;
    }
}

DXGI_FORMAT translate_format(SceGxmColorBaseFormat format) {
    switch (format) {
    // classic unpacked formats
    case SCE_GXM_COLOR_BASE_FORMAT_U8:
        return DXGI_FORMAT_R8_UNORM;
    case SCE_GXM_COLOR_BASE_FORMAT_S8:
        return DXGI_FORMAT_R8_SNORM;
    case SCE_GXM_COLOR_BASE_FORMAT_U16:
        return DXGI_FORMAT_R16_UNORM;
    case SCE_GXM_COLOR_BASE_FORMAT_S16:
        return DXGI_FORMAT_R16_SNORM;
    case SCE_GXM_COLOR_BASE_FORMAT_F16:
        return DXGI_FORMAT_R16_FLOAT;
    case SCE_GXM_COLOR_BASE_FORMAT_F32:
        return DXGI_FORMAT_R32_FLOAT;

    case SCE_GXM_COLOR_BASE_FORMAT_U8U8:
        return DXGI_FORMAT_R8G8_UNORM;
    case SCE_GXM_COLOR_BASE_FORMAT_S8S8:
        return DXGI_FORMAT_R8G8_SNORM;
    case SCE_GXM_COLOR_BASE_FORMAT_U16U16:
        return DXGI_FORMAT_R16G16_UNORM;
    case SCE_GXM_COLOR_BASE_FORMAT_S16S16:
        return DXGI_FORMAT_R16G16_SNORM;
    case SCE_GXM_COLOR_BASE_FORMAT_F16F16:
        return DXGI_FORMAT_R16G16_FLOAT;
    case SCE_GXM_COLOR_BASE_FORMAT_F32F32:
        return DXGI_FORMAT_R32G32_FLOAT;

    case SCE_GXM_COLOR_BASE_FORMAT_U8U8U8U8:
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    case SCE_GXM_COLOR_BASE_FORMAT_S8S8S8S8:
        return DXGI_FORMAT_R8G8B8A8_SNORM;
    case SCE_GXM_COLOR_BASE_FORMAT_F16F16F16F16:
        return DXGI_FORMAT_R16G16B16A16_FLOAT;

    // packed formats
    case SCE_GXM_COLOR_BASE_FORMAT_U5U6U5:
        return DXGI_FORMAT_B5G6R5_UNORM;
    case SCE_GXM_COLOR_BASE_FORMAT_F11F11F10:
        return DXGI_FORMAT_R11G11B10_FLOAT;
    case SCE_GXM_COLOR_BASE_FORMAT_SE5M9M9M9:
        return DXGI_FORMAT_R9G9B9E5_SHAREDEXP;
    case SCE_GXM_COLOR_BASE_FORMAT_U8U8U8:
        // 24-bit packed RGB does not exist in DXGI; widen to RGBA8.
        return DXGI_FORMAT_R8G8B8A8_UNORM;

    case SCE_GXM_COLOR_BASE_FORMAT_U1U5U5U5:
        return DXGI_FORMAT_B5G5R5A1_UNORM;
    case SCE_GXM_COLOR_BASE_FORMAT_U4U4U4U4:
        return DXGI_FORMAT_B4G4R4A4_UNORM;
    case SCE_GXM_COLOR_BASE_FORMAT_U2U10U10U10:
        // DXGI has no A2R10G10B10; R10G10B10A2 puts red in the low bits
        // instead, so only the swizzles that survive that reordering are right.
        return DXGI_FORMAT_R10G10B10A2_UNORM;
    case SCE_GXM_COLOR_BASE_FORMAT_U2F10F10F10:
        // No float variant exists, so give it something wider.
        return DXGI_FORMAT_R16G16B16A16_FLOAT;

    default:
        LOG_ERROR("Unknown format {}", log_hex(format));
        return DXGI_FORMAT_UNKNOWN;
    }
}

} // namespace color

namespace texture {

static uint32_t translate_swizzle1(SceGxmTextureSwizzle1Mode mode) {
    switch (mode) {
    case SCE_GXM_TEXTURE_SWIZZLE1_R:
        return swizzle_r001;
    case SCE_GXM_TEXTURE_SWIZZLE1_000R:
        return swizzle_r000;
    case SCE_GXM_TEXTURE_SWIZZLE1_111R:
        return swizzle_r111;
    case SCE_GXM_TEXTURE_SWIZZLE1_RRRR:
        return swizzle_rrrr;
    case SCE_GXM_TEXTURE_SWIZZLE1_0RRR:
        return swizzle_rrr0;
    case SCE_GXM_TEXTURE_SWIZZLE1_1RRR:
        return swizzle_rrr1;
    case SCE_GXM_TEXTURE_SWIZZLE1_R000:
        return swizzle_000r;
    case SCE_GXM_TEXTURE_SWIZZLE1_R111:
        return swizzle_111r;
    default:
        LOG_ERROR("Unknown swizzle mode {}", log_hex(mode));
        return swizzle_identity;
    }
}

static uint32_t translate_swizzle2(SceGxmTextureSwizzle2Mode mode) {
    switch (mode) {
    case SCE_GXM_TEXTURE_SWIZZLE2_GR:
        return swizzle_rg01;
    case SCE_GXM_TEXTURE_SWIZZLE2_00GR:
        return swizzle_rg00;
    case SCE_GXM_TEXTURE_SWIZZLE2_GRRR:
        return swizzle_rrrg;
    case SCE_GXM_TEXTURE_SWIZZLE2_RGGG:
        return swizzle_gggr;
    case SCE_GXM_TEXTURE_SWIZZLE2_GRGR:
        return swizzle_rgrg;
    case SCE_GXM_TEXTURE_SWIZZLE2_00RG:
        return swizzle_gr00;
    default:
        LOG_ERROR("Unknown swizzle mode {}", log_hex(mode));
        return swizzle_identity;
    }
}

static uint32_t translate_swizzleds(SceGxmTextureSwizzle2ModeAlt mode) {
    switch (mode) {
    case SCE_GXM_TEXTURE_SWIZZLE2_SD:
    case SCE_GXM_TEXTURE_SWIZZLE2_DS:
        return swizzle_identity;
    default:
        LOG_ERROR("Unknown swizzle mode {}", log_hex(mode));
        return swizzle_identity;
    }
}

static uint32_t translate_swizzle3(SceGxmTextureSwizzle3Mode mode) {
    switch (mode) {
    case SCE_GXM_TEXTURE_SWIZZLE3_BGR:
        return swizzle_rgb1;
    case SCE_GXM_TEXTURE_SWIZZLE3_RGB:
        return swizzle_bgr1;
    default:
        LOG_ERROR("Unknown swizzle mode {}", log_hex(mode));
        return swizzle_identity;
    }
}

static uint32_t translate_swizzle3_bgr(SceGxmTextureSwizzle3Mode mode) {
    switch (mode) {
    case SCE_GXM_TEXTURE_SWIZZLE3_BGR:
        return swizzle_bgr1;
    case SCE_GXM_TEXTURE_SWIZZLE3_RGB:
        return swizzle_rgb1;
    default:
        LOG_ERROR("Unknown swizzle mode {}", log_hex(mode));
        return swizzle_identity;
    }
}

static uint32_t translate_swizzle4(SceGxmTextureSwizzle4Mode mode) {
    switch (mode) {
    case SCE_GXM_TEXTURE_SWIZZLE4_ABGR:
        return swizzle_rgba;
    case SCE_GXM_TEXTURE_SWIZZLE4_ARGB:
        return swizzle_bgra;
    case SCE_GXM_TEXTURE_SWIZZLE4_RGBA:
        return swizzle_abgr;
    case SCE_GXM_TEXTURE_SWIZZLE4_BGRA:
        return swizzle_gbar;
    case SCE_GXM_TEXTURE_SWIZZLE4_1BGR:
        return swizzle_rgb1;
    case SCE_GXM_TEXTURE_SWIZZLE4_1RGB:
        return swizzle_bgr1;
    case SCE_GXM_TEXTURE_SWIZZLE4_RGB1:
        return swizzle_abg1;
    case SCE_GXM_TEXTURE_SWIZZLE4_BGR1:
        return swizzle_gba1;
    default:
        LOG_ERROR("Unknown swizzle mode {}", log_hex(mode));
        return swizzle_identity;
    }
}

static uint32_t translate_swizzle4_abgr(SceGxmTextureSwizzle4Mode mode) {
    switch (mode) {
    case SCE_GXM_TEXTURE_SWIZZLE4_ABGR:
        return swizzle_abgr;
    case SCE_GXM_TEXTURE_SWIZZLE4_ARGB:
        return swizzle_gbar;
    case SCE_GXM_TEXTURE_SWIZZLE4_RGBA:
        return swizzle_rgba;
    case SCE_GXM_TEXTURE_SWIZZLE4_BGRA:
        return swizzle_bgra;
    default:
        LOG_ERROR("Unknown swizzle mode {}", log_hex(mode));
        return swizzle_identity;
    }
}

// DXGI_FORMAT_B4G4R4A4_UNORM orders its nibbles opposite to vulkan's R4G4B4A4_PACK16, so its table differs.
static uint32_t translate_swizzle4_b4g4r4a4(SceGxmTextureSwizzle4Mode mode) {
    switch (mode) {
    case SCE_GXM_TEXTURE_SWIZZLE4_ABGR:
        return swizzle_bgra;
    case SCE_GXM_TEXTURE_SWIZZLE4_ARGB:
        return swizzle_rgba;
    case SCE_GXM_TEXTURE_SWIZZLE4_RGBA:
        return swizzle_argb;
    case SCE_GXM_TEXTURE_SWIZZLE4_BGRA:
        return swizzle_grab;
    case SCE_GXM_TEXTURE_SWIZZLE4_1BGR:
        return swizzle_bgr1;
    case SCE_GXM_TEXTURE_SWIZZLE4_1RGB:
        return swizzle_rgb1;
    case SCE_GXM_TEXTURE_SWIZZLE4_RGB1:
        return swizzle_arg1;
    case SCE_GXM_TEXTURE_SWIZZLE4_BGR1:
        return swizzle_gra1;
    default:
        LOG_ERROR("Unknown swizzle mode {}", log_hex(mode));
        return swizzle_identity;
    }
}

static uint32_t translate_swizzleyuv(uint32_t mode) {
    // YUV is converted to RGBA before upload, so the surviving swizzle is
    // whatever the conversion produced.
    return swizzle_identity;
}

uint32_t translate_swizzle(SceGxmTextureFormat format) {
    const SceGxmTextureBaseFormat base_format = gxm::get_base_format(format);
    const uint32_t swizzle = format & SCE_GXM_TEXTURE_SWIZZLE_MASK;
    switch (base_format) {
    // 1 component
    case SCE_GXM_TEXTURE_BASE_FORMAT_U8:
    case SCE_GXM_TEXTURE_BASE_FORMAT_S8:
    case SCE_GXM_TEXTURE_BASE_FORMAT_U16:
    case SCE_GXM_TEXTURE_BASE_FORMAT_S16:
    case SCE_GXM_TEXTURE_BASE_FORMAT_F16:
    case SCE_GXM_TEXTURE_BASE_FORMAT_U32:
    case SCE_GXM_TEXTURE_BASE_FORMAT_S32:
    case SCE_GXM_TEXTURE_BASE_FORMAT_F32:
    case SCE_GXM_TEXTURE_BASE_FORMAT_F32M:
    case SCE_GXM_TEXTURE_BASE_FORMAT_UBC4:
    case SCE_GXM_TEXTURE_BASE_FORMAT_SBC4:
        return translate_swizzle1(static_cast<SceGxmTextureSwizzle1Mode>(swizzle));

    // 2 components
    case SCE_GXM_TEXTURE_BASE_FORMAT_U8U8:
    case SCE_GXM_TEXTURE_BASE_FORMAT_S8S8:
    case SCE_GXM_TEXTURE_BASE_FORMAT_U16U16:
    case SCE_GXM_TEXTURE_BASE_FORMAT_S16S16:
    case SCE_GXM_TEXTURE_BASE_FORMAT_F16F16:
    case SCE_GXM_TEXTURE_BASE_FORMAT_U32U32:
    case SCE_GXM_TEXTURE_BASE_FORMAT_F32F32:
    case SCE_GXM_TEXTURE_BASE_FORMAT_UBC5:
    case SCE_GXM_TEXTURE_BASE_FORMAT_SBC5:
        return translate_swizzle2(static_cast<SceGxmTextureSwizzle2Mode>(swizzle));

    case SCE_GXM_TEXTURE_BASE_FORMAT_X8U24:
        return translate_swizzleds(static_cast<SceGxmTextureSwizzle2ModeAlt>(swizzle));

    // 3 components
    case SCE_GXM_TEXTURE_BASE_FORMAT_U8U8U8:
    case SCE_GXM_TEXTURE_BASE_FORMAT_S8S8S8:
    case SCE_GXM_TEXTURE_BASE_FORMAT_F11F11F10:
    case SCE_GXM_TEXTURE_BASE_FORMAT_SE5M9M9M9:
        return translate_swizzle3(static_cast<SceGxmTextureSwizzle3Mode>(swizzle));

    case SCE_GXM_TEXTURE_BASE_FORMAT_U5U6U5:
        return translate_swizzle3_bgr(static_cast<SceGxmTextureSwizzle3Mode>(swizzle));

    // 4 components
    case SCE_GXM_TEXTURE_BASE_FORMAT_U8U3U3U2:
    case SCE_GXM_TEXTURE_BASE_FORMAT_U8U8U8U8:
    case SCE_GXM_TEXTURE_BASE_FORMAT_S8S8S8S8:
    case SCE_GXM_TEXTURE_BASE_FORMAT_U16U16U16U16:
    case SCE_GXM_TEXTURE_BASE_FORMAT_S16S16S16S16:
    case SCE_GXM_TEXTURE_BASE_FORMAT_F16F16F16F16:
    case SCE_GXM_TEXTURE_BASE_FORMAT_P8:
    case SCE_GXM_TEXTURE_BASE_FORMAT_P4:
    case SCE_GXM_TEXTURE_BASE_FORMAT_PVRT2BPP:
    case SCE_GXM_TEXTURE_BASE_FORMAT_PVRT4BPP:
    case SCE_GXM_TEXTURE_BASE_FORMAT_PVRTII2BPP:
    case SCE_GXM_TEXTURE_BASE_FORMAT_PVRTII4BPP:
    // TODO: the following is not fully supported
    case SCE_GXM_TEXTURE_BASE_FORMAT_U2U10U10U10:
    case SCE_GXM_TEXTURE_BASE_FORMAT_U2F10F10F10:
    case SCE_GXM_TEXTURE_BASE_FORMAT_UBC1:
    case SCE_GXM_TEXTURE_BASE_FORMAT_UBC2:
    case SCE_GXM_TEXTURE_BASE_FORMAT_UBC3:
        return translate_swizzle4(static_cast<SceGxmTextureSwizzle4Mode>(swizzle));

    case SCE_GXM_TEXTURE_BASE_FORMAT_U4U4U4U4:
        return translate_swizzle4_b4g4r4a4(static_cast<SceGxmTextureSwizzle4Mode>(swizzle));

    // TODO: the following is not fully supported
    case SCE_GXM_TEXTURE_BASE_FORMAT_U1U5U5U5:
        return translate_swizzle4_abgr(static_cast<SceGxmTextureSwizzle4Mode>(swizzle));

    case SCE_GXM_TEXTURE_BASE_FORMAT_YUV420P2:
    case SCE_GXM_TEXTURE_BASE_FORMAT_YUV420P3:
    case SCE_GXM_TEXTURE_BASE_FORMAT_YUV422:
        return translate_swizzleyuv(swizzle);

    default:
        LOG_ERROR("Unknown format {}", log_hex(base_format));
        return swizzle_identity;
    }
}

DXGI_FORMAT translate_format(SceGxmTextureBaseFormat base_format) {
    switch (base_format) {
    case SCE_GXM_TEXTURE_BASE_FORMAT_U8:
        return DXGI_FORMAT_R8_UNORM;
    case SCE_GXM_TEXTURE_BASE_FORMAT_S8:
        return DXGI_FORMAT_R8_SNORM;
    case SCE_GXM_TEXTURE_BASE_FORMAT_U16:
        return DXGI_FORMAT_R16_UNORM;
    case SCE_GXM_TEXTURE_BASE_FORMAT_S16:
        return DXGI_FORMAT_R16_SNORM;
    case SCE_GXM_TEXTURE_BASE_FORMAT_F16:
        return DXGI_FORMAT_R16_FLOAT;
    case SCE_GXM_TEXTURE_BASE_FORMAT_U32:
        return DXGI_FORMAT_R32_UINT;
    case SCE_GXM_TEXTURE_BASE_FORMAT_S32:
        return DXGI_FORMAT_R32_SINT;
    case SCE_GXM_TEXTURE_BASE_FORMAT_X8U24:
    case SCE_GXM_TEXTURE_BASE_FORMAT_F32:
    case SCE_GXM_TEXTURE_BASE_FORMAT_F32M:
        return DXGI_FORMAT_R32_FLOAT;

    case SCE_GXM_TEXTURE_BASE_FORMAT_U8U8:
        return DXGI_FORMAT_R8G8_UNORM;
    case SCE_GXM_TEXTURE_BASE_FORMAT_S8S8:
        return DXGI_FORMAT_R8G8_SNORM;
    case SCE_GXM_TEXTURE_BASE_FORMAT_U16U16:
        return DXGI_FORMAT_R16G16_UNORM;
    case SCE_GXM_TEXTURE_BASE_FORMAT_S16S16:
        return DXGI_FORMAT_R16G16_SNORM;
    case SCE_GXM_TEXTURE_BASE_FORMAT_F16F16:
        return DXGI_FORMAT_R16G16_FLOAT;
    case SCE_GXM_TEXTURE_BASE_FORMAT_U32U32:
        return DXGI_FORMAT_R32G32_UINT;
    case SCE_GXM_TEXTURE_BASE_FORMAT_F32F32:
        return DXGI_FORMAT_R32G32_FLOAT;

    case SCE_GXM_TEXTURE_BASE_FORMAT_U8U8U8U8:
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    case SCE_GXM_TEXTURE_BASE_FORMAT_S8S8S8S8:
        return DXGI_FORMAT_R8G8B8A8_SNORM;
    case SCE_GXM_TEXTURE_BASE_FORMAT_U16U16U16U16:
        return DXGI_FORMAT_R16G16B16A16_UNORM;
    case SCE_GXM_TEXTURE_BASE_FORMAT_S16S16S16S16:
        return DXGI_FORMAT_R16G16B16A16_SNORM;
    case SCE_GXM_TEXTURE_BASE_FORMAT_F16F16F16F16:
        return DXGI_FORMAT_R16G16B16A16_FLOAT;

    case SCE_GXM_TEXTURE_BASE_FORMAT_U5U6U5:
        return DXGI_FORMAT_B5G6R5_UNORM;
    case SCE_GXM_TEXTURE_BASE_FORMAT_F11F11F10:
        return DXGI_FORMAT_R11G11B10_FLOAT;
    case SCE_GXM_TEXTURE_BASE_FORMAT_SE5M9M9M9:
        return DXGI_FORMAT_R9G9B9E5_SHAREDEXP;

    // the following formats are all decompressed to u8u8u8u8 before upload
    case SCE_GXM_TEXTURE_BASE_FORMAT_U8U3U3U2:
    case SCE_GXM_TEXTURE_BASE_FORMAT_U8U8U8:
    case SCE_GXM_TEXTURE_BASE_FORMAT_P8:
    case SCE_GXM_TEXTURE_BASE_FORMAT_P4:
    case SCE_GXM_TEXTURE_BASE_FORMAT_PVRT2BPP:
    case SCE_GXM_TEXTURE_BASE_FORMAT_PVRT4BPP:
    case SCE_GXM_TEXTURE_BASE_FORMAT_PVRTII2BPP:
    case SCE_GXM_TEXTURE_BASE_FORMAT_PVRTII4BPP:
    case SCE_GXM_TEXTURE_BASE_FORMAT_YUV420P2:
    case SCE_GXM_TEXTURE_BASE_FORMAT_YUV420P3:
    case SCE_GXM_TEXTURE_BASE_FORMAT_YUV422:
        return DXGI_FORMAT_R8G8B8A8_UNORM;

    // no three-component snorm format exists, so an alpha channel is added
    case SCE_GXM_TEXTURE_BASE_FORMAT_S8S8S8:
        return DXGI_FORMAT_R8G8B8A8_SNORM;

    case SCE_GXM_TEXTURE_BASE_FORMAT_U4U4U4U4:
        return DXGI_FORMAT_B4G4R4A4_UNORM;
    case SCE_GXM_TEXTURE_BASE_FORMAT_U1U5U5U5:
        return DXGI_FORMAT_B5G5R5A1_UNORM;
    case SCE_GXM_TEXTURE_BASE_FORMAT_U2U10U10U10:
        // See the matching note in color::translate_format.
        return DXGI_FORMAT_R10G10B10A2_UNORM;
    case SCE_GXM_TEXTURE_BASE_FORMAT_U2F10F10F10:
        return DXGI_FORMAT_R16G16B16A16_FLOAT;

    case SCE_GXM_TEXTURE_BASE_FORMAT_UBC1:
        return DXGI_FORMAT_BC1_UNORM;
    case SCE_GXM_TEXTURE_BASE_FORMAT_UBC2:
        return DXGI_FORMAT_BC2_UNORM;
    case SCE_GXM_TEXTURE_BASE_FORMAT_UBC3:
        return DXGI_FORMAT_BC3_UNORM;
    case SCE_GXM_TEXTURE_BASE_FORMAT_UBC4:
        return DXGI_FORMAT_BC4_UNORM;
    case SCE_GXM_TEXTURE_BASE_FORMAT_SBC4:
        return DXGI_FORMAT_BC4_SNORM;
    case SCE_GXM_TEXTURE_BASE_FORMAT_UBC5:
        return DXGI_FORMAT_BC5_UNORM;
    case SCE_GXM_TEXTURE_BASE_FORMAT_SBC5:
        return DXGI_FORMAT_BC5_SNORM;
    case SCE_GXM_TEXTURE_BASE_FORMAT_UBC6H:
        return DXGI_FORMAT_BC6H_UF16;
    case SCE_GXM_TEXTURE_BASE_FORMAT_SBC6H:
        return DXGI_FORMAT_BC6H_SF16;
    case SCE_GXM_TEXTURE_BASE_FORMAT_UBC7:
        return DXGI_FORMAT_BC7_UNORM;

    default:
        // ASTC has no DXGI equivalent at all. The texture cache decompresses
        // those to RGBA8 before they reach here, so anything still arriving as
        // ASTC is a bug worth reporting rather than silently mishandling.
        LOG_ERROR("Unsupported texture format {}", log_hex(base_format));
        return DXGI_FORMAT_UNKNOWN;
    }
}

D3D12_TEXTURE_ADDRESS_MODE translate_address_mode(SceGxmTextureAddrMode src) {
    switch (src) {
    case SCE_GXM_TEXTURE_ADDR_REPEAT:
        return D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    case SCE_GXM_TEXTURE_ADDR_MIRROR:
        return D3D12_TEXTURE_ADDRESS_MODE_MIRROR;
    case SCE_GXM_TEXTURE_ADDR_CLAMP:
        return D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    case SCE_GXM_TEXTURE_ADDR_MIRROR_CLAMP:
        return D3D12_TEXTURE_ADDRESS_MODE_MIRROR_ONCE;
    case SCE_GXM_TEXTURE_ADDR_REPEAT_IGNORE_BORDER:
    case SCE_GXM_TEXTURE_ADDR_CLAMP_FULL_BORDER:
    case SCE_GXM_TEXTURE_ADDR_CLAMP_IGNORE_BORDER:
    case SCE_GXM_TEXTURE_ADDR_CLAMP_HALF_BORDER:
        LOG_ERROR_ONCE("Unhandled border color address mode, texture will be corrupted. Please report it to the developers.");
        return (src == SCE_GXM_TEXTURE_ADDR_REPEAT_IGNORE_BORDER)
            ? D3D12_TEXTURE_ADDRESS_MODE_WRAP
            : D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    default:
        LOG_ERROR("Unknown address mode {}", log_hex(src));
        return D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    }
}

D3D12_FILTER translate_filter(SceGxmTextureFilter min_filter, SceGxmTextureFilter mag_filter,
    SceGxmTextureMipFilter mip_filter, bool anisotropic) {
    if (anisotropic)
        return D3D12_FILTER_ANISOTROPIC;

    auto is_linear = [](SceGxmTextureFilter filter) {
        switch (filter) {
        case SCE_GXM_TEXTURE_FILTER_LINEAR:
        case SCE_GXM_TEXTURE_FILTER_MIPMAP_LINEAR:
            return true;
        case SCE_GXM_TEXTURE_FILTER_POINT:
        case SCE_GXM_TEXTURE_FILTER_MIPMAP_POINT:
            return false;
        default:
            LOG_ERROR("Unknown texture filter {}", log_hex(filter));
            return false;
        }
    };

    // Same rule as the vulkan backend: mip filtering being enabled at all is
    // what selects linear sampling between mip levels.
    static constexpr D3D12_FILTER filters[2][2][2] = {
        // min = point
        { { D3D12_FILTER_MIN_MAG_MIP_POINT, D3D12_FILTER_MIN_MAG_POINT_MIP_LINEAR },
            { D3D12_FILTER_MIN_POINT_MAG_LINEAR_MIP_POINT, D3D12_FILTER_MIN_POINT_MAG_MIP_LINEAR } },
        // min = linear
        { { D3D12_FILTER_MIN_LINEAR_MAG_MIP_POINT, D3D12_FILTER_MIN_LINEAR_MAG_POINT_MIP_LINEAR },
            { D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT, D3D12_FILTER_MIN_MAG_MIP_LINEAR } },
    };

    return filters[is_linear(min_filter)][is_linear(mag_filter)][mip_filter == SCE_GXM_TEXTURE_MIP_FILTER_ENABLED];
}

} // namespace texture

} // namespace renderer::d3d12
