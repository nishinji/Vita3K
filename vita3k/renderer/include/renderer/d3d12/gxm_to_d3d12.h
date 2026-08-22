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

#include <renderer/d3d12/common.h>

#include <gxm/types.h>

namespace renderer::d3d12 {

// DXGI has no scaled formats and no three-component 8/16-bit formats, so
// `is_integer` is always true for the integer families and three-component
// attributes are widened to four by the caller. See DXState::late_init, which
// turns off support_scaled_attribute_formats and support_rgb_attributes so the
// recompiler converts in the shader instead.
DXGI_FORMAT translate_attribute_format(SceGxmAttributeFormat format, unsigned int component_count, bool is_integer, bool is_signed);
// Whether DXGI can express this attribute with the requested component count.
// False means the caller should retry with four components.
bool is_attribute_format_supported(SceGxmAttributeFormat format, unsigned int component_count);

D3D12_BLEND translate_blend_factor(SceGxmBlendFactor blend_factor);
// Alpha blend factors may not reference a colour channel in D3D12, so the
// colour factors have to be folded onto their alpha equivalents.
D3D12_BLEND translate_blend_factor_alpha(SceGxmBlendFactor blend_factor);
D3D12_BLEND_OP translate_blend_func(SceGxmBlendFunc blend_func);
D3D12_PRIMITIVE_TOPOLOGY translate_primitive(SceGxmPrimitiveType primitive);
// The coarse category a PSO is created with, as opposed to the exact topology
// bound on the command list.
D3D12_PRIMITIVE_TOPOLOGY_TYPE translate_primitive_type(SceGxmPrimitiveType primitive);
D3D12_COMPARISON_FUNC translate_depth_func(SceGxmDepthFunc depth_func);
D3D12_FILL_MODE translate_polygon_mode(SceGxmPolygonMode polygon_mode);
D3D12_CULL_MODE translate_cull_mode(SceGxmCullMode cull_mode);
D3D12_COMPARISON_FUNC translate_stencil_func(SceGxmStencilFunc stencil_func);
D3D12_STENCIL_OP translate_stencil_op(SceGxmStencilOp stencil_op);

// Depth/stencil surface format, plus the formats to read the two aspects back
// through as shader resources (D3D12 needs typeless resources for that).
struct DepthStencilFormats {
    DXGI_FORMAT resource = DXGI_FORMAT_UNKNOWN;
    DXGI_FORMAT dsv = DXGI_FORMAT_UNKNOWN;
    DXGI_FORMAT depth_srv = DXGI_FORMAT_UNKNOWN;
    DXGI_FORMAT stencil_srv = DXGI_FORMAT_UNKNOWN;
};

DepthStencilFormats translate_depth_stencil_format(SceGxmDepthStencilFormat format);

namespace color {
DXGI_FORMAT translate_format(SceGxmColorBaseFormat base_format);
// Encoded D3D12_SHADER_COMPONENT_MAPPING, for SRVs that sample a colour surface.
uint32_t translate_swizzle(SceGxmColorFormat format);
} // namespace color

namespace texture {
DXGI_FORMAT translate_format(SceGxmTextureBaseFormat base_format);
uint32_t translate_swizzle(SceGxmTextureFormat format);
D3D12_TEXTURE_ADDRESS_MODE translate_address_mode(SceGxmTextureAddrMode src);
// D3D12 packs min/mag/mip filtering and anisotropy into a single enum, so the
// pieces are gathered and encoded together rather than translated one by one.
D3D12_FILTER translate_filter(SceGxmTextureFilter min_filter, SceGxmTextureFilter mag_filter,
    SceGxmTextureMipFilter mip_filter, bool anisotropic);
} // namespace texture

} // namespace renderer::d3d12
