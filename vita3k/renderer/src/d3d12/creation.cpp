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

#include <renderer/d3d12/functions.h>
#include <renderer/d3d12/gxm_to_d3d12.h>
#include <renderer/d3d12/state.h>
#include <renderer/types.h>

#include <xxh3.h>

#include <features/state.h>
#include <gxm/types.h>
#include <util/log.h>

namespace renderer::d3d12 {

// DXContext construction and its recording lifecycle live in context.cpp.

bool create(DXState &state, std::unique_ptr<Context> &context, MemState &mem) {
    auto dx_context = std::make_unique<DXContext>(state, mem);
    if (!dx_context->create_resources())
        return false;

    context = std::move(dx_context);
    return true;
}

// DXRenderTarget

static void create_depth_stencil(DXState &state, DXRenderTarget &render_target, const SceGxmRenderTargetParams &params) {
    uint32_t width = render_target.scaled_width;
    uint32_t height = render_target.scaled_height;
    // Same sizing as the vulkan render target: 4x MSAA without downscale renders at twice the size.
    if (params.multisampleMode == SCE_GXM_MULTISAMPLE_4X) {
        width *= 2;
        height *= 2;
    }

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
        .MipLevels = 1,
        .Format = DXRenderTarget::depth_stencil_format,
        .SampleDesc = { .Count = 1, .Quality = 0 },
        .Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN,
        .Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL | D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE,
    };

    const D3D12_CLEAR_VALUE clear_value{
        .Format = DXRenderTarget::depth_stencil_format,
        .DepthStencil = { .Depth = 1.0f, .Stencil = 0 },
    };

    if (!dx_check(state.device->CreateCommittedResource(&heap_props, D3D12_HEAP_FLAG_NONE, &desc,
                      D3D12_RESOURCE_STATE_DEPTH_WRITE, &clear_value, IID_PPV_ARGS(&render_target.depth_stencil.resource)),
            "creating a render target depth stencil"))
        return;

    render_target.depth_stencil.state = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    render_target.depth_stencil.resource->SetName(L"Vita3K render target depth stencil");

    render_target.dsv = state.dsv_heap.allocate();
    if (!render_target.dsv.valid()) {
        render_target.depth_stencil.reset();
        return;
    }

    const D3D12_DEPTH_STENCIL_VIEW_DESC dsv_desc{
        .Format = DXRenderTarget::depth_stencil_format,
        .ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D,
        .Flags = D3D12_DSV_FLAG_NONE,
        .Texture2D = { .MipSlice = 0 },
    };
    state.device->CreateDepthStencilView(render_target.depth_stencil.get(), &dsv_desc, render_target.dsv.cpu);
}

bool create(DXState &state, std::unique_ptr<RenderTarget> &rt, const SceGxmRenderTargetParams &params, const FeatureState &features) {
    auto render_target = std::make_unique<DXRenderTarget>();

    render_target->width = static_cast<uint16_t>(params.width);
    render_target->height = static_cast<uint16_t>(params.height);
    render_target->scaled_width = static_cast<uint16_t>(params.width * state.res_multiplier);
    render_target->scaled_height = static_cast<uint16_t>(params.height * state.res_multiplier);
    render_target->multisample_mode = params.multisampleMode;

    create_depth_stencil(state, *render_target, params);

    rt = std::move(render_target);
    return true;
}

void destroy(DXState &state, std::unique_ptr<RenderTarget> &rt) {
    if (auto *render_target = static_cast<DXRenderTarget *>(rt.get())) {
        state.dsv_heap.free(render_target->dsv);
        // A frame still in flight may be testing against it.
        if (render_target->depth_stencil)
            state.frame().destroy_queue.push_back(render_target->depth_stencil.resource);
    }

    rt.reset();
}

// Shader programs
//
// The GXM program is translated to SPIR-V by the shared recompiler and from
// there to DXIL; both halves are keyed on the same hash the other backends use,
// which is filled in by the shared renderer::create() wrappers.

bool create(std::unique_ptr<VertexProgram> &vp, DXState &state, const SceGxmProgram &program) {
    vp = std::make_unique<DXVertexProgram>();
    return true;
}

bool create(std::unique_ptr<FragmentProgram> &fp, DXState &state, const SceGxmProgram &program, const SceGxmBlendInfo *blend) {
    fp = std::make_unique<DXFragmentProgram>();

    DXFragmentProgram *fp_dx = reinterpret_cast<DXFragmentProgram *>(fp.get());

    // Programs writing native colour bypass the blend unit entirely.
    if (blend != nullptr && !program.is_native_color()) {
        UINT8 color_mask = 0;
        if (blend->colorMask & SCE_GXM_COLOR_MASK_R)
            color_mask |= D3D12_COLOR_WRITE_ENABLE_RED;
        if (blend->colorMask & SCE_GXM_COLOR_MASK_G)
            color_mask |= D3D12_COLOR_WRITE_ENABLE_GREEN;
        if (blend->colorMask & SCE_GXM_COLOR_MASK_B)
            color_mask |= D3D12_COLOR_WRITE_ENABLE_BLUE;
        if (blend->colorMask & SCE_GXM_COLOR_MASK_A)
            color_mask |= D3D12_COLOR_WRITE_ENABLE_ALPHA;

        fp_dx->blending = D3D12_RENDER_TARGET_BLEND_DESC{
            .BlendEnable = (blend->colorFunc != SCE_GXM_BLEND_FUNC_NONE) || (blend->alphaFunc != SCE_GXM_BLEND_FUNC_NONE),
            .LogicOpEnable = FALSE,
            .SrcBlend = translate_blend_factor(blend->colorSrc),
            .DestBlend = translate_blend_factor(blend->colorDst),
            .BlendOp = translate_blend_func(blend->colorFunc),
            // D3D12 forbids colour-channel factors in the alpha slot.
            .SrcBlendAlpha = translate_blend_factor_alpha(blend->alphaSrc),
            .DestBlendAlpha = translate_blend_factor_alpha(blend->alphaDst),
            .BlendOpAlpha = translate_blend_func(blend->alphaFunc),
            .LogicOp = D3D12_LOGIC_OP_NOOP,
            .RenderTargetWriteMask = color_mask,
        };
    } else {
        fp_dx->blending = D3D12_RENDER_TARGET_BLEND_DESC{
            .BlendEnable = FALSE,
            .LogicOpEnable = FALSE,
            .SrcBlend = D3D12_BLEND_ONE,
            .DestBlend = D3D12_BLEND_ZERO,
            .BlendOp = D3D12_BLEND_OP_ADD,
            .SrcBlendAlpha = D3D12_BLEND_ONE,
            .DestBlendAlpha = D3D12_BLEND_ZERO,
            .BlendOpAlpha = D3D12_BLEND_OP_ADD,
            .LogicOp = D3D12_LOGIC_OP_NOOP,
            .RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL,
        };
    }

    // Part of the pipeline key, so it has to cover the whole blend description.
    fp_dx->blending_hash = XXH_INLINE_XXH3_64bits(&fp_dx->blending, sizeof(D3D12_RENDER_TARGET_BLEND_DESC));
    return true;
}

// The DXTextureCache implementation lives in texture.cpp.

} // namespace renderer::d3d12
