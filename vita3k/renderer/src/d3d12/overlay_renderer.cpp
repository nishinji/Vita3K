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
// Copyright RPCS3
// SPDX-License-Identifier: GPL-2.0
// Code heavily referenced/taken from https://github.com/RPCS3/rpcs3/tree/master/rpcs3/Emu/RSX/Overlays

#include <renderer/d3d12/overlay_renderer.h>

#include <renderer/d3d12/shader_compile.h>
#include <renderer/d3d12/state.h>

#include <overlay/font.h>
#include <util/align.h>
#include <util/log.h>

#include <algorithm>
#include <chrono>
#include <cmath>

namespace renderer::d3d12 {

// The space the overlay lays itself out in, before it is scaled onto whatever
// part of the window the Vita image occupies.
static constexpr float VIRTUAL_WIDTH = 960.f;
static constexpr float VIRTUAL_HEIGHT = 544.f;

// Staging for the overlay's vertices and its font/image uploads. A font page is
// a megabyte, so this has room for a handful of them plus a frame of geometry.
static constexpr uint64_t UPLOAD_BUFFER_SIZE = 32 * 1024 * 1024;

// A placed footprint has to start on a 512-byte boundary, which is the coarser
// of the two alignments the copies here need.
static constexpr uint64_t UPLOAD_ALIGNMENT = D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT;

static const char *OVERLAY_VERTEX_HLSL = R"(
cbuffer OverlayConstants : register(b0) {
    float4 ui_scale;
    float4 albedo;
    float4 viewport;
    float4 clip_bounds;
    uint vertex_config;
    uint fragment_config;
    float timestamp;
    float blur_intensity;
    float4 sdf_params;
    float4 sdf_origin;
    float4 sdf_border_color;
};

struct VSInput {
    float4 pos : POSITION; // xy = position, zw = texcoord
};

struct VSOutput {
    float4 position : SV_Position;
    float2 tc0 : TEXCOORD0;
    float4 color : TEXCOORD1;
    float4 clip_rect : TEXCOORD2;
};

float2 snap_to_grid(float2 normalized) {
    return floor(normalized * viewport.xy + float2(0.5, 0.5)) / viewport.xy;
}

float4 clip_to_ndc(float4 coord, bool flip_vertically) {
    float4 ret = (coord * ui_scale.zwzw) / ui_scale.xyxy;
    if (flip_vertically)
        ret.yw = 1.0 - ret.yw;
    return ret;
}

float4 ndc_to_window(float4 coord) {
    return coord * viewport.xyxy + viewport.zwzw;
}

float4 make_aabb(float4 coords) {
    float4 result = coords;
    if (coords.x > coords.z)
        result.xz = coords.zx;
    if (coords.y > coords.w)
        result.yw = coords.wy;
    return result;
}

VSOutput main(VSInput input) {
    const bool no_vertex_snap = (vertex_config & 1u) != 0u;
    const bool flip_vertically = (vertex_config & 2u) != 0u;

    VSOutput output;
    output.tc0 = input.pos.zw;
    output.color = albedo;
    output.clip_rect = make_aabb(ndc_to_window(clip_to_ndc(clip_bounds, flip_vertically)));

    float4 pos = float4(clip_to_ndc(input.pos, flip_vertically).xy, 0.5, 1.0);
    if (!no_vertex_snap)
        pos.xy = snap_to_grid(pos.xy);

    output.position = (pos + pos) - 1.0;
    // The shader is a port of the vulkan one, whose clip space has +Y pointing
    // down. D3D12 has it pointing up, so without this the overlay would come out
    // upside down.
    output.position.y = -output.position.y;
    return output;
}
)";

static const char *OVERLAY_PIXEL_HLSL = R"(
#define SAMPLER_MODE_NONE      0u
#define SAMPLER_MODE_FONT2D    1u
#define SAMPLER_MODE_FONT3D    2u
#define SAMPLER_MODE_TEXTURE2D 3u

#define SDF_DISABLED  0u
#define SDF_ELLIPSE   1u
#define SDF_BOX       2u
#define SDF_ROUND_BOX 3u

Texture2D fs0 : register(t0);
Texture2DArray fs1 : register(t1);
SamplerState image_sampler : register(s0);
SamplerState font_sampler : register(s1);

cbuffer OverlayConstants : register(b0) {
    float4 ui_scale;
    float4 albedo;
    float4 viewport;
    float4 clip_bounds;
    uint vertex_config;
    uint fragment_config;
    float timestamp;
    float blur_intensity;
    float4 sdf_params;
    float4 sdf_origin;
    float4 sdf_border_color;
};

struct PSInput {
    float4 position : SV_Position;
    float2 tc0 : TEXCOORD0;
    float4 color : TEXCOORD1;
    float4 clip_rect : TEXCOORD2;
};

float4 SDF_blend(float sd, float border_width, float4 inner_color, float4 border_color, float4 outer_color) {
    const float fw = fwidth(sd);
    const float a = smoothstep(-border_width + fw, -border_width - fw, sd);
    const float b = smoothstep(fw, -fw, sd);
    float4 c = lerp(outer_color, border_color, b);
    c = lerp(c, inner_color, a);
    return c;
}

float SDF_fn(uint sdf, float2 frag_coord) {
    const float2 p = floor(frag_coord) - sdf_origin.xy;
    const float2 hs = sdf_params.xy;
    const float r = sdf_params.z;
    float2 v;

    switch (sdf) {
    case SDF_ELLIPSE:
        return (length(p / hs) - 1.0) * length(hs);
    case SDF_BOX:
        v = abs(p) - hs;
        return length(max(v, 0.0)) + min(max(v.x, v.y), 0.0);
    case SDF_ROUND_BOX:
        v = abs(p) - (hs - r);
        return length(max(v, 0.0)) + min(max(v.x, v.y), 0.0) - r;
    default:
        return -1.0;
    }
}

float4 blur_sample(float2 coord, float2 tex_offset) {
    float2 coords[9];
    coords[0] = coord - tex_offset;
    coords[1] = coord + float2(0.0, -tex_offset.y);
    coords[2] = coord + float2(tex_offset.x, -tex_offset.y);
    coords[3] = coord + float2(-tex_offset.x, 0.0);
    coords[4] = coord;
    coords[5] = coord + float2(tex_offset.x, 0.0);
    coords[6] = coord + float2(-tex_offset.x, tex_offset.y);
    coords[7] = coord + float2(0.0, tex_offset.y);
    coords[8] = coord + tex_offset;

    const float weights[9] = {
        1.0, 2.0, 1.0,
        2.0, 4.0, 2.0,
        1.0, 2.0, 1.0
    };

    float4 blurred = float4(0.0, 0.0, 0.0, 0.0);
    [unroll] for (int n = 0; n < 9; ++n) {
        blurred += fs0.Sample(image_sampler, coords[n]) * weights[n];
    }

    return blurred / 16.0;
}

float4 sample_image(float2 coord, float blur_str) {
    const float4 original = fs0.Sample(image_sampler, coord);
    if (blur_str == 0.0)
        return original;

    uint tex_w, tex_h;
    fs0.GetDimensions(tex_w, tex_h);

    const float2 constraints = 1.0 / float2(640.0, 360.0);
    const float2 res_offset = 1.0 / float2(tex_w, tex_h);
    const float2 tex_offset = max(res_offset, constraints);

    const float4 blur0 = blur_sample(coord + float2(-res_offset.x, 0.0), tex_offset);
    const float4 blur1 = blur_sample(coord + float2(res_offset.x, 0.0), tex_offset);
    const float4 blur2 = blur_sample(coord + float2(0.0, res_offset.y), tex_offset);

    const float4 blurred = (blur0 + blur1 + blur2) / 3.0;
    return lerp(original, blurred, blur_str);
}

float4 main(PSInput input) : SV_Target {
    const bool clip_fragments = (fragment_config & 1u) != 0u;
    const bool use_pulse_glow = (fragment_config & 2u) != 0u;
    const uint sampler_mode = (fragment_config >> 2u) & 3u;
    const uint sdf = (fragment_config >> 4u) & 3u;
    const bool use_gloss = (fragment_config & 64u) != 0u;
    const bool use_btn_gloss = (fragment_config & 128u) != 0u;

    // SV_Position matches gl_FragCoord: both count from the top-left corner with
    // the sample at the pixel centre, so the clip test and the SDF origin need no
    // adjustment for the flipped clip space.
    const float2 frag_coord = input.position.xy;

    if (clip_fragments) {
        if (frag_coord.x < input.clip_rect.x || frag_coord.x > input.clip_rect.z || frag_coord.y < input.clip_rect.y || frag_coord.y > input.clip_rect.w) {
            discard;
        }
    }

    float4 diff_color = input.color;
    if (use_pulse_glow) {
        diff_color.a *= (sin(timestamp) + 1.0) * 0.5;
    }

    if (use_gloss) {
        const float gloss_h = sdf_params.x;
        const float feather = sdf_params.y;
        const float opacity = sdf_params.z;
        const float u = input.tc0.x;
        const float v = input.tc0.y;

        const float g = v < gloss_h ? pow((gloss_h - v) / gloss_h, 1.8) : 0.0;

        const float s = smoothstep(0.0, feather, u) * (1.0 - smoothstep(1.0 - feather, 1.0, u));

        diff_color = float4(1.0, 1.0, 1.0, g * s * opacity);
    }

    if (use_btn_gloss) {
        const float u = input.tc0.x;
        const float v = input.tc0.y;

        float glossH, opacity, bottomOpacity, curveLift, aspect, rFrac;
        const bool combined = (sdf != SDF_DISABLED);

        if (combined) {
            glossH = sdf_origin.z;
            const float packedVal = sdf_origin.w;
            opacity = floor(packedVal) / 100.0;
            bottomOpacity = frac(packedVal);
            aspect = sdf_params.x / max(sdf_params.y, 0.001);
            rFrac = sdf_params.z / max(sdf_params.y, 0.001);
            curveLift = glossH * 0.3;
        } else {
            glossH = sdf_params.x;
            curveLift = sdf_params.y;
            opacity = sdf_params.z;
            rFrac = sdf_params.w;
            aspect = sdf_origin.x;
            bottomOpacity = sdf_origin.y;
        }

        const float u_edge = min(u, 1.0 - u);
        const float edge_frac = max(rFrac / (2.0 * max(aspect, 0.001)), 0.08);
        const float edge_t = smoothstep(0.0, edge_frac, u_edge);
        const float boundary = glossH - (1.0 - edge_t) * curveLift;

        float g = 0.0;
        if (v < boundary) {
            const float t = v / boundary;
            g = 1.0 - 0.6 * t - 0.4 * t * t;
        }

        const float bzone = 0.15;
        const float bt = max(0.0, (v - (1.0 - bzone)) / bzone);
        const float bottom_g = bt * bt * bottomOpacity;

        const float total = max(g * opacity, bottom_g);

        if (combined) {
            diff_color = float4(diff_color.rgb + float3(total, total, total), diff_color.a + total);
        } else {
            const float2 p = float2((u - 0.5) * 2.0 * aspect, (v - 0.5) * 2.0);
            const float2 hs = float2(aspect, 1.0);
            const float2 q = abs(p) - (hs - rFrac);
            const float d = length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - rFrac;
            const float mask = 1.0 - smoothstep(-fwidth(d) * 1.5, fwidth(d) * 1.5, d);
            diff_color = float4(1.0, 1.0, 1.0, total * mask);
        }
    }

    if (sdf != SDF_DISABLED) {
        const float border_w = sdf_params.w;
        const float d = SDF_fn(sdf, frag_coord);
        diff_color = SDF_blend(d, border_w, diff_color, sdf_border_color, float4(sdf_border_color.rgb, 0.0));
    }

    float4 ocol;
    switch (sampler_mode) {
    case SAMPLER_MODE_FONT2D:
        ocol = fs0.Sample(font_sampler, input.tc0).rrrr * diff_color;
        break;
    case SAMPLER_MODE_FONT3D:
        ocol = fs1.Sample(font_sampler, float3(input.tc0.x, frac(input.tc0.y), trunc(input.tc0.y))).rrrr * diff_color;
        break;
    case SAMPLER_MODE_TEXTURE2D:
        ocol = sample_image(input.tc0, blur_intensity).rgba * diff_color;
        break;
    default:
        ocol = diff_color;
        break;
    }

    return ocol;
}
)";

OverlayRenderer::~OverlayRenderer() {
    destroy();
}

bool OverlayRenderer::init(DXState &dx_state) {
    state = &dx_state;

    if (!create_root_signature())
        return false;

    if (!create_pipelines())
        return false;

    if (!upload.create(state->device.Get(), UPLOAD_BUFFER_SIZE, UPLOAD_ALIGNMENT, L"Vita3K overlay upload"))
        return false;

    // The pixel shader always reads t1 as a Texture2DArray, so the commands that
    // do not use a font still need something bound there. The shared 1x1 default
    // texture can be viewed as a one-layer array, which saves a resource.
    dummy_array_srv = state->srv_staging_heap.allocate();
    if (!dummy_array_srv.valid()) {
        LOG_ERROR("D3D12: no room in the SRV heap for the overlay dummy texture");
        return false;
    }

    const D3D12_SHADER_RESOURCE_VIEW_DESC dummy_desc{
        .Format = DXGI_FORMAT_R8G8B8A8_UNORM,
        .ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY,
        .Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING,
        .Texture2DArray = {
            .MostDetailedMip = 0,
            .MipLevels = 1,
            .FirstArraySlice = 0,
            .ArraySize = 1,
            .PlaneSlice = 0,
            .ResourceMinLODClamp = 0.0f,
        },
    };
    state->device->CreateShaderResourceView(state->default_image.get(), &dummy_desc, dummy_array_srv.cpu);

    return true;
}

bool OverlayRenderer::create_root_signature() {
    const D3D12_DESCRIPTOR_RANGE srv_range{
        .RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV,
        .NumDescriptors = 2,
        .BaseShaderRegister = 0,
        .RegisterSpace = 0,
        .OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND,
    };

    D3D12_ROOT_PARAMETER params[2] = {};
    // The whole constant block lives in the root signature. At 32 DWORDs it is
    // half the budget, and it changes for every draw, which is exactly what root
    // constants are for.
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants = {
        .ShaderRegister = 0,
        .RegisterSpace = 0,
        .Num32BitValues = sizeof(OverlayRootConstants) / sizeof(uint32_t),
    };
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable = { 1, &srv_range };
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    // Both samplers are fixed, so they can be baked into the root signature and
    // no sampler heap is needed at all.
    D3D12_STATIC_SAMPLER_DESC samplers[2] = {};
    // s0: images, with mip filtering to match the vulkan image sampler.
    samplers[0] = D3D12_STATIC_SAMPLER_DESC{
        .Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR,
        .AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
        .AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
        .AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
        .MipLODBias = 0.0f,
        .MaxAnisotropy = 1,
        .ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER,
        .BorderColor = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK,
        .MinLOD = 0.0f,
        .MaxLOD = D3D12_FLOAT32_MAX,
        .ShaderRegister = 0,
        .RegisterSpace = 0,
        .ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL,
    };
    // s1: font atlases, which have no mip chain.
    samplers[1] = samplers[0];
    samplers[1].Filter = D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT;
    samplers[1].MaxLOD = 0.0f;
    samplers[1].ShaderRegister = 1;

    const D3D12_ROOT_SIGNATURE_DESC root_desc{
        .NumParameters = 2,
        .pParameters = params,
        .NumStaticSamplers = 2,
        .pStaticSamplers = samplers,
        .Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT
            | D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS
            | D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS
            | D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS,
    };

    ComPtr<ID3DBlob> serialized;
    ComPtr<ID3DBlob> error;
    const HRESULT hr = D3D12SerializeRootSignature(&root_desc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &error);
    if (FAILED(hr)) {
        const std::string message = error
            ? std::string(static_cast<const char *>(error->GetBufferPointer()), error->GetBufferSize())
            : hresult_to_string(hr);
        LOG_ERROR("D3D12: failed to serialize the overlay root signature: {}", message);
        return false;
    }

    if (!dx_check(state->device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
                      IID_PPV_ARGS(&root_sig)),
            "creating the overlay root signature"))
        return false;

    root_sig->SetName(L"Vita3K overlay root signature");
    return true;
}

bool OverlayRenderer::create_pipelines() {
    std::vector<uint8_t> vertex_bytecode;
    std::vector<uint8_t> pixel_bytecode;
    std::string compile_error;

    if (!compile_hlsl_to_dxil(OVERLAY_VERTEX_HLSL, ShaderStage::Vertex, "overlay_vertex", vertex_bytecode, compile_error)) {
        LOG_ERROR("D3D12: could not compile the overlay vertex shader: {}", compile_error);
        return false;
    }

    if (!compile_hlsl_to_dxil(OVERLAY_PIXEL_HLSL, ShaderStage::Fragment, "overlay_pixel", pixel_bytecode, compile_error)) {
        LOG_ERROR("D3D12: could not compile the overlay pixel shader: {}", compile_error);
        return false;
    }

    const D3D12_INPUT_ELEMENT_DESC input_element{
        .SemanticName = "POSITION",
        .SemanticIndex = 0,
        .Format = DXGI_FORMAT_R32G32B32A32_FLOAT,
        .InputSlot = 0,
        .AlignedByteOffset = 0,
        .InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,
        .InstanceDataStepRate = 0,
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pipeline_desc{};
    pipeline_desc.pRootSignature = root_sig.Get();
    pipeline_desc.VS = { vertex_bytecode.data(), vertex_bytecode.size() };
    pipeline_desc.PS = { pixel_bytecode.data(), pixel_bytecode.size() };
    pipeline_desc.InputLayout = { &input_element, 1 };
    pipeline_desc.NumRenderTargets = 1;
    pipeline_desc.RTVFormats[0] = state->swapchain_format;
    pipeline_desc.SampleDesc = { .Count = 1, .Quality = 0 };
    pipeline_desc.SampleMask = UINT_MAX;

    pipeline_desc.RasterizerState = D3D12_RASTERIZER_DESC{
        .FillMode = D3D12_FILL_MODE_SOLID,
        .CullMode = D3D12_CULL_MODE_NONE,
        .FrontCounterClockwise = FALSE,
        .DepthBias = D3D12_DEFAULT_DEPTH_BIAS,
        .DepthBiasClamp = D3D12_DEFAULT_DEPTH_BIAS_CLAMP,
        .SlopeScaledDepthBias = D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS,
        .DepthClipEnable = TRUE,
        .MultisampleEnable = FALSE,
        .AntialiasedLineEnable = FALSE,
        .ForcedSampleCount = 0,
        .ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF,
    };

    // Straight alpha over the presented frame; the destination alpha is left
    // alone so the swapchain stays opaque.
    pipeline_desc.BlendState.RenderTarget[0] = D3D12_RENDER_TARGET_BLEND_DESC{
        .BlendEnable = TRUE,
        .LogicOpEnable = FALSE,
        .SrcBlend = D3D12_BLEND_SRC_ALPHA,
        .DestBlend = D3D12_BLEND_INV_SRC_ALPHA,
        .BlendOp = D3D12_BLEND_OP_ADD,
        .SrcBlendAlpha = D3D12_BLEND_ZERO,
        .DestBlendAlpha = D3D12_BLEND_ONE,
        .BlendOpAlpha = D3D12_BLEND_OP_ADD,
        .LogicOp = D3D12_LOGIC_OP_NOOP,
        .RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL,
    };

    pipeline_desc.DepthStencilState.DepthEnable = FALSE;
    pipeline_desc.DepthStencilState.StencilEnable = FALSE;
    pipeline_desc.DSVFormat = DXGI_FORMAT_UNKNOWN;

    // A D3D12 pipeline only bakes in the topology class, so one pipeline covers
    // strips and lists alike and the exact topology is set while recording.
    static constexpr D3D12_PRIMITIVE_TOPOLOGY_TYPE types[2] = {
        D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE,
        D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE,
    };

    for (int i = 0; i < 2; i++) {
        pipeline_desc.PrimitiveTopologyType = types[i];
        if (!dx_check(state->device->CreateGraphicsPipelineState(&pipeline_desc, IID_PPV_ARGS(&pipelines[i])),
                "creating an overlay pipeline state"))
            return false;
    }

    pipelines[0]->SetName(L"Vita3K overlay triangle pipeline");
    pipelines[1]->SetName(L"Vita3K overlay line pipeline");

    return true;
}

void OverlayRenderer::release_texture(OverlayTexture &texture) {
    if (texture.srv.valid()) {
        // The shader-visible copy of this descriptor lives in the frame ring, so
        // the staging slot can go back to the pool right away.
        state->srv_staging_heap.free(texture.srv);
        texture.srv = {};
    }

    // A font atlas is rebuilt when new glyph pages appear, which can happen while
    // an earlier frame is still sampling the old one. D3D12 keeps no reference of
    // its own, so the resource is handed to the frame destroy queue and released
    // once that frame's fence has been passed.
    if (texture.image)
        state->frame().destroy_queue.push_back(texture.image.resource);

    texture.image.reset();
    texture.width = 0;
    texture.height = 0;
    texture.layers = 1;
    texture.mip_levels = 1;
    texture.format = DXGI_FORMAT_UNKNOWN;
}

void OverlayRenderer::destroy() {
    if (!state)
        return;

    for (auto &[font, texture] : font_atlases)
        release_texture(texture);
    font_atlases.clear();

    for (auto &[key, texture] : raw_images)
        release_texture(texture);
    raw_images.clear();

    for (OverlayTexture &texture : icons)
        release_texture(texture);
    icons.clear();

    if (dummy_array_srv.valid()) {
        state->srv_staging_heap.free(dummy_array_srv);
        dummy_array_srv = {};
    }

    upload.destroy();

    for (ComPtr<ID3D12PipelineState> &pipeline : pipelines)
        pipeline.Reset();
    root_sig.Reset();

    prepared_views.clear();

    resources.free_resources();
    resources_loaded = false;

    state = nullptr;
}

bool OverlayRenderer::create_texture(OverlayTexture &texture, DXGI_FORMAT format,
    uint32_t width, uint32_t height, uint32_t layers, uint32_t mip_levels, bool array_view) {
    release_texture(texture);

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
        .DepthOrArraySize = static_cast<UINT16>(layers),
        .MipLevels = static_cast<UINT16>(mip_levels),
        .Format = format,
        .SampleDesc = { .Count = 1, .Quality = 0 },
        .Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN,
        .Flags = D3D12_RESOURCE_FLAG_NONE,
    };

    if (!dx_check(state->device->CreateCommittedResource(&heap_props, D3D12_HEAP_FLAG_NONE, &desc,
                      D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&texture.image.resource)),
            "creating an overlay texture"))
        return false;

    texture.image.state = D3D12_RESOURCE_STATE_COPY_DEST;
    texture.image.resource->SetName(array_view ? L"Vita3K overlay font atlas" : L"Vita3K overlay image");
    texture.width = width;
    texture.height = height;
    texture.layers = layers;
    texture.mip_levels = mip_levels;
    texture.format = format;

    texture.srv = state->srv_staging_heap.allocate();
    if (!texture.srv.valid()) {
        LOG_ERROR("D3D12: no room in the SRV heap for an overlay texture");
        texture.image.reset();
        return false;
    }

    D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc{
        .Format = format,
        .Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING,
    };

    if (array_view) {
        srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
        srv_desc.Texture2DArray = {
            .MostDetailedMip = 0,
            .MipLevels = mip_levels,
            .FirstArraySlice = 0,
            .ArraySize = layers,
            .PlaneSlice = 0,
            .ResourceMinLODClamp = 0.0f,
        };
    } else {
        srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv_desc.Texture2D = {
            .MostDetailedMip = 0,
            .MipLevels = mip_levels,
            .PlaneSlice = 0,
            .ResourceMinLODClamp = 0.0f,
        };
    }

    state->device->CreateShaderResourceView(texture.image.get(), &srv_desc, texture.srv.cpu);
    return true;
}

bool OverlayRenderer::upload_subresource(ID3D12GraphicsCommandList *cmd_list, OverlayTexture &texture,
    uint32_t subresource, const uint8_t *data, uint32_t width, uint32_t height, uint32_t bytes_per_pixel) {
    const uint32_t row_bytes = width * bytes_per_pixel;
    const uint32_t upload_pitch = align(row_bytes, static_cast<uint32_t>(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT));
    const uint64_t upload_size = static_cast<uint64_t>(upload_pitch) * height;

    UploadRingBuffer::Allocation allocation = upload.allocate(upload_size);
    if (!allocation.valid()) {
        // Dropping the upload leaves the texture with whatever it held before,
        // which is better than tearing down the frame over a font page.
        LOG_WARN_ONCE("D3D12: the overlay upload buffer is full; a texture was not uploaded");
        return false;
    }

    for (uint32_t y = 0; y < height; y++)
        memcpy(allocation.cpu + static_cast<uint64_t>(y) * upload_pitch, data + static_cast<uint64_t>(y) * row_bytes, row_bytes);

    const D3D12_TEXTURE_COPY_LOCATION destination{
        .pResource = texture.image.get(),
        .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,
        .SubresourceIndex = subresource,
    };

    D3D12_TEXTURE_COPY_LOCATION source{};
    source.pResource = allocation.resource;
    source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    source.PlacedFootprint.Offset = allocation.offset;
    source.PlacedFootprint.Footprint = D3D12_SUBRESOURCE_FOOTPRINT{
        .Format = texture.format,
        .Width = width,
        .Height = height,
        .Depth = 1,
        .RowPitch = upload_pitch,
    };

    cmd_list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
    return true;
}

void OverlayRenderer::upload_font(ID3D12GraphicsCommandList *cmd_list, BarrierBatcher &barriers, const overlay::font *font) {
    if (!font)
        return;

    const auto dims = font->get_glyph_data_dimensions();
    if (dims.depth == 0)
        return;

    OverlayTexture &texture = font_atlases[font];
    if (texture.image && texture.layers == dims.depth)
        return;

    const std::vector<uint8_t> &glyph_data = font->get_glyph_data();
    const size_t page_size = static_cast<size_t>(dims.width) * dims.height;
    if (glyph_data.size() < page_size * dims.depth)
        return;

    // One R8 layer per glyph page, addressed by the third texture coordinate.
    if (!create_texture(texture, DXGI_FORMAT_R8_UNORM, dims.width, dims.height, dims.depth, 1, true))
        return;

    barriers.transition(texture.image, D3D12_RESOURCE_STATE_COPY_DEST);
    barriers.flush(cmd_list);

    for (uint32_t layer = 0; layer < dims.depth; layer++) {
        // With one mip level the subresource index is just the array slice.
        upload_subresource(cmd_list, texture, layer,
            glyph_data.data() + page_size * layer, dims.width, dims.height, 1);
    }

    barriers.transition(texture.image, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    barriers.flush(cmd_list);
}

// Box filter one mip level down. D3D12 has no blit that filters, so unlike the
// vulkan backend the chain is built on the CPU; the images involved are decoded
// once at load time, so the cost is paid once.
static void downsample(const uint8_t *source, uint32_t source_w, uint32_t source_h,
    uint8_t *destination, uint32_t destination_w, uint32_t destination_h, uint32_t channels) {
    for (uint32_t y = 0; y < destination_h; y++) {
        const uint32_t y0 = std::min(y * 2, source_h - 1);
        const uint32_t y1 = std::min(y * 2 + 1, source_h - 1);

        for (uint32_t x = 0; x < destination_w; x++) {
            const uint32_t x0 = std::min(x * 2, source_w - 1);
            const uint32_t x1 = std::min(x * 2 + 1, source_w - 1);

            for (uint32_t c = 0; c < channels; c++) {
                const uint32_t sum = source[(y0 * source_w + x0) * channels + c]
                    + source[(y0 * source_w + x1) * channels + c]
                    + source[(y1 * source_w + x0) * channels + c]
                    + source[(y1 * source_w + x1) * channels + c];
                destination[(y * destination_w + x) * channels + c] = static_cast<uint8_t>((sum + 2) / 4);
            }
        }
    }
}

void OverlayRenderer::upload_image(ID3D12GraphicsCommandList *cmd_list, BarrierBatcher &barriers,
    const overlay::image_info_base *info, OverlayTexture &destination) {
    if (!info || !info->get_data())
        return;

    const uint32_t width = static_cast<uint32_t>(info->w);
    const uint32_t height = static_cast<uint32_t>(info->h);
    const uint32_t channels = static_cast<uint32_t>(info->channels);
    if (width == 0 || height == 0 || (channels != 1 && channels != 4))
        return;

    const DXGI_FORMAT format = channels == 4 ? DXGI_FORMAT_R8G8B8A8_UNORM : DXGI_FORMAT_R8_UNORM;
    const bool need_mipmaps = std::max(width, height) > MIPMAP_SIZE_THRESHOLD;
    const uint32_t mip_levels = need_mipmaps
        ? static_cast<uint32_t>(std::floor(std::log2(std::max(width, height)))) + 1
        : 1;

    if (!destination.image || destination.width != width || destination.height != height || destination.format != format) {
        if (!create_texture(destination, format, width, height, 1, mip_levels, false))
            return;
    }

    barriers.transition(destination.image, D3D12_RESOURCE_STATE_COPY_DEST);
    barriers.flush(cmd_list);

    upload_subresource(cmd_list, destination, 0, info->get_data(), width, height, channels);

    if (need_mipmaps) {
        // Ping-pong between two scratch buffers, halving each time.
        std::vector<uint8_t> current(info->get_data(),
            info->get_data() + static_cast<size_t>(width) * height * channels);
        std::vector<uint8_t> next;

        uint32_t mip_w = width;
        uint32_t mip_h = height;
        for (uint32_t level = 1; level < mip_levels; level++) {
            const uint32_t next_w = std::max(mip_w / 2, 1u);
            const uint32_t next_h = std::max(mip_h / 2, 1u);

            next.resize(static_cast<size_t>(next_w) * next_h * channels);
            downsample(current.data(), mip_w, mip_h, next.data(), next_w, next_h, channels);

            upload_subresource(cmd_list, destination, level, next.data(), next_w, next_h, channels);

            current.swap(next);
            mip_w = next_w;
            mip_h = next_h;
        }
    }

    barriers.transition(destination.image, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    barriers.flush(cmd_list);

    info->dirty = false;
}

void OverlayRenderer::prepare(ID3D12GraphicsCommandList *cmd_list, BarrierBatcher &barriers,
    const overlay::display_manager &manager, float viewport_w, float viewport_h) {
    prepared_views.clear();

    if (!state || !pipelines[0] || !cmd_list)
        return;

    upload.reclaim(state->completed_fence_value());

    manager.lock_shared();

    for (const auto &view : manager.get_views()) {
        if (!view || !view->visible.load())
            continue;

        view->set_render_viewport(
            static_cast<uint16_t>(std::min<uint32_t>(static_cast<uint32_t>(viewport_w), UINT16_MAX)),
            static_cast<uint16_t>(std::min<uint32_t>(static_cast<uint32_t>(viewport_h), UINT16_MAX)));

        overlay::compiled_resource compiled = view->get_compiled();

        for (const auto &draw_cmd : compiled.draw_commands) {
            const auto &config = draw_cmd.config;

            if (config.font_ref) {
                upload_font(cmd_list, barriers, config.font_ref);
            } else if (config.texture_ref == overlay::raw_image && config.external_data_ref) {
                OverlayTexture &texture = raw_images[config.external_data_ref];
                const auto *info = static_cast<const overlay::image_info_base *>(config.external_data_ref);
                if (!texture.image || info->dirty)
                    upload_image(cmd_list, barriers, info, texture);
            } else if (config.texture_ref > 0
                && config.texture_ref != overlay::font_file
                && config.texture_ref != overlay::game_icon
                && config.texture_ref != overlay::backbuffer
                && config.texture_ref < overlay::raw_image) {
                if (!resources_loaded) {
                    resources.load_files();
                    resources_loaded = true;
                    icons.clear();
                }

                const size_t index = static_cast<size_t>(config.texture_ref - 1);
                if (index < resources.texture_raw_data.size()) {
                    if (icons.size() <= index)
                        icons.resize(index + 1);

                    OverlayTexture &icon = icons[index];
                    const auto *info = resources.texture_raw_data[index].get();
                    if (info && (!icon.image || info->dirty))
                        upload_image(cmd_list, barriers, info, icon);
                }
            }
        }

        prepared_views.push_back({ view, std::move(compiled) });
    }

    manager.unlock_shared();
}

void OverlayRenderer::draw_command(ID3D12GraphicsCommandList *cmd_list,
    const overlay::compiled_resource::command &draw_cmd,
    float viewport_x, float viewport_y, float viewport_w, float viewport_h) {
    if (draw_cmd.verts.empty())
        return;

    const auto &config = draw_cmd.config;

    // D3D12 has no triangle fan, so one is turned into a triangle list here. No
    // overlay element uses fans today, but the primitive type is part of the
    // compiled command, so it has to be handled.
    std::vector<overlay::vertex> expanded;
    const overlay::vertex *vertices = draw_cmd.verts.data();
    uint32_t vertex_count = static_cast<uint32_t>(draw_cmd.verts.size());

    if (config.primitives == overlay::primitive_type::triangle_fan) {
        if (vertex_count < 3)
            return;

        expanded.reserve(static_cast<size_t>(vertex_count - 2) * 3);
        for (uint32_t i = 1; i + 1 < vertex_count; i++) {
            expanded.push_back(draw_cmd.verts[0]);
            expanded.push_back(draw_cmd.verts[i]);
            expanded.push_back(draw_cmd.verts[i + 1]);
        }

        vertices = expanded.data();
        vertex_count = static_cast<uint32_t>(expanded.size());
    }

    const uint64_t data_size = static_cast<uint64_t>(vertex_count) * sizeof(overlay::vertex);
    UploadRingBuffer::Allocation allocation = upload.allocate(data_size);
    if (!allocation.valid()) {
        LOG_WARN_ONCE("D3D12: the overlay upload buffer is full; a draw was skipped");
        return;
    }

    memcpy(allocation.cpu, vertices, data_size);

    // Pick the textures this command samples. Anything left unbound gets the
    // shared 1x1 default, since the shader reads both slots unconditionally.
    DescriptorHandle texture_2d = state->default_srv;
    DescriptorHandle texture_array = dummy_array_srv;

    const uint8_t ref = config.texture_ref;
    if (ref == overlay::image_resource_none || ref == overlay::game_icon || ref == overlay::backbuffer) {
        // Nothing to sample: the command is a flat colour or a pure SDF shape.
    } else if (config.font_ref) {
        const auto it = font_atlases.find(config.font_ref);
        if (it != font_atlases.end() && it->second.srv.valid())
            texture_array = it->second.srv;
    } else if (ref == overlay::raw_image && config.external_data_ref) {
        const auto it = raw_images.find(config.external_data_ref);
        if (it != raw_images.end() && it->second.srv.valid())
            texture_2d = it->second.srv;
    } else {
        const size_t index = static_cast<size_t>(ref - 1);
        if (index < icons.size() && icons[index].srv.valid())
            texture_2d = icons[index].srv;
    }

    FrameObject &frame_object = state->frame();
    const DescriptorHandle table = frame_object.view_descriptors.allocate(2);
    if (!table.valid()) {
        LOG_WARN_ONCE("D3D12: ran out of frame view descriptors while drawing the overlay");
        return;
    }

    const uint32_t increment = frame_object.view_descriptors.increment();
    D3D12_CPU_DESCRIPTOR_HANDLE slot = table.cpu;
    state->device->CopyDescriptorsSimple(1, slot, texture_2d.cpu, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    slot.ptr += increment;
    state->device->CopyDescriptorsSimple(1, slot, texture_array.cpu, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    OverlayRootConstants constants{};

    constants.ui_scale[0] = VIRTUAL_WIDTH;
    constants.ui_scale[1] = VIRTUAL_HEIGHT;
    constants.ui_scale[2] = 1.f;
    constants.ui_scale[3] = 1.f;

    constants.albedo[0] = config.color.r;
    constants.albedo[1] = config.color.g;
    constants.albedo[2] = config.color.b;
    constants.albedo[3] = config.color.a;

    constants.viewport[0] = viewport_w;
    constants.viewport[1] = viewport_h;
    constants.viewport[2] = viewport_x;
    constants.viewport[3] = viewport_y;

    constants.clip_bounds[0] = config.clip_rect.x1;
    constants.clip_bounds[1] = config.clip_rect.y1;
    constants.clip_bounds[2] = config.clip_rect.x2;
    constants.clip_bounds[3] = config.clip_rect.y2;

    uint32_t vertex_config = 0;
    if (config.disable_vertex_snap)
        vertex_config |= 1u;
    constants.vertex_config = vertex_config;

    uint32_t fragment_config = 0;
    if (config.clip_region)
        fragment_config |= 1u;
    if (config.pulse_glow)
        fragment_config |= 2u;

    uint32_t sampler_mode = 0;
    if (config.font_ref) {
        sampler_mode = 2;
    } else if (config.texture_ref == overlay::font_file) {
        sampler_mode = 1;
    } else if (config.texture_ref != overlay::image_resource_none
        && config.texture_ref != overlay::game_icon
        && config.texture_ref != overlay::backbuffer) {
        sampler_mode = 3;
    }
    fragment_config |= (sampler_mode & 3u) << 2u;

    const bool is_sdf = config.active_effect == overlay::compiled_resource::effect_type::sdf
        && config.effect.sdf.func != overlay::sdf_function::none;
    const bool is_gloss = config.active_effect == overlay::compiled_resource::effect_type::gloss;
    const bool is_btn_gloss = config.active_effect == overlay::compiled_resource::effect_type::btn_gloss;
    const bool has_sdf_btn_gloss = is_sdf && config.effect.sdf.btn_gloss_height > 0.f;

    const uint32_t sdf_type = is_sdf ? static_cast<uint32_t>(config.effect.sdf.func) : 0u;
    fragment_config |= (sdf_type & 3u) << 4u;
    if (is_gloss)
        fragment_config |= 64u;
    if (is_btn_gloss || has_sdf_btn_gloss)
        fragment_config |= 128u;
    constants.fragment_config = fragment_config;

    constants.timestamp = config.get_sinus_value();
    constants.blur_intensity = static_cast<float>(config.blur_strength);

    if (is_sdf) {
        // The SDF is defined in virtual space and evaluated against SV_Position,
        // so it has to be moved into the pixel space of the letterboxed area.
        auto sdf = config.effect.sdf;

        overlay::areaf target_viewport;
        target_viewport.x1 = viewport_x;
        target_viewport.y1 = viewport_y;
        target_viewport.x2 = viewport_x + viewport_w;
        target_viewport.y2 = viewport_y + viewport_h;
        sdf.transform(target_viewport, { VIRTUAL_WIDTH, VIRTUAL_HEIGHT });

        constants.sdf_params[0] = sdf.hx;
        constants.sdf_params[1] = sdf.hy;
        constants.sdf_params[2] = sdf.br;
        constants.sdf_params[3] = sdf.bw;
        constants.sdf_origin[0] = sdf.cx;
        constants.sdf_origin[1] = sdf.cy;

        // The button gloss shares the SDF slots when both are active.
        if (has_sdf_btn_gloss) {
            constants.sdf_origin[2] = sdf.btn_gloss_height;
            constants.sdf_origin[3] = std::round(sdf.btn_gloss_opacity * 100.f) + sdf.btn_gloss_bottom_opacity;
        }

        constants.sdf_border_color[0] = sdf.border_color.r;
        constants.sdf_border_color[1] = sdf.border_color.g;
        constants.sdf_border_color[2] = sdf.border_color.b;
        constants.sdf_border_color[3] = sdf.border_color.a;
    } else if (is_gloss) {
        const auto &gloss = config.effect.gloss;
        constants.sdf_params[0] = gloss.height;
        constants.sdf_params[1] = gloss.feather;
        constants.sdf_params[2] = gloss.opacity;
    } else if (is_btn_gloss) {
        const auto &btn_gloss = config.effect.btn_gloss;
        constants.sdf_params[0] = btn_gloss.height;
        constants.sdf_params[1] = btn_gloss.curve_lift;
        constants.sdf_params[2] = btn_gloss.opacity;
        constants.sdf_params[3] = btn_gloss.border_radius_frac;
        constants.sdf_origin[0] = btn_gloss.aspect;
        constants.sdf_origin[1] = btn_gloss.bottom_opacity;
    }

    cmd_list->SetGraphicsRoot32BitConstants(0, sizeof(OverlayRootConstants) / sizeof(uint32_t), &constants, 0);
    cmd_list->SetGraphicsRootDescriptorTable(1, table.gpu);

    const D3D12_VERTEX_BUFFER_VIEW vertex_view{
        .BufferLocation = allocation.gpu,
        .SizeInBytes = static_cast<UINT>(data_size),
        .StrideInBytes = sizeof(overlay::vertex),
    };
    cmd_list->IASetVertexBuffers(0, 1, &vertex_view);

    switch (config.primitives) {
    case overlay::primitive_type::quad_list:
        cmd_list->SetPipelineState(pipelines[0].Get());
        cmd_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
        // Every four vertices form their own strip, so they cannot be drawn in
        // one call without an index buffer.
        for (uint32_t quad = 0; quad < vertex_count / 4; quad++)
            cmd_list->DrawInstanced(4, 1, quad * 4, 0);
        break;

    case overlay::primitive_type::triangle_strip:
        cmd_list->SetPipelineState(pipelines[0].Get());
        cmd_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
        cmd_list->DrawInstanced(vertex_count, 1, 0, 0);
        break;

    case overlay::primitive_type::triangle_fan:
        cmd_list->SetPipelineState(pipelines[0].Get());
        cmd_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        cmd_list->DrawInstanced(vertex_count, 1, 0, 0);
        break;

    case overlay::primitive_type::line_list:
        cmd_list->SetPipelineState(pipelines[1].Get());
        cmd_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_LINELIST);
        cmd_list->DrawInstanced(vertex_count, 1, 0, 0);
        break;

    case overlay::primitive_type::line_strip:
        cmd_list->SetPipelineState(pipelines[1].Get());
        cmd_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_LINESTRIP);
        cmd_list->DrawInstanced(vertex_count, 1, 0, 0);
        break;
    }
}

void OverlayRenderer::render(ID3D12GraphicsCommandList *cmd_list,
    float viewport_x, float viewport_y, float viewport_w, float viewport_h) {
    if (!state || !cmd_list || prepared_views.empty() || !pipelines[0]) {
        prepared_views.clear();
        return;
    }

    FrameObject &frame_object = state->frame();
    ID3D12DescriptorHeap *heaps[] = { frame_object.view_descriptors.handle() };
    cmd_list->SetDescriptorHeaps(1, heaps);
    cmd_list->SetGraphicsRootSignature(root_sig.Get());

    const D3D12_VIEWPORT viewport{
        .TopLeftX = viewport_x,
        .TopLeftY = viewport_y,
        .Width = viewport_w,
        .Height = viewport_h,
        .MinDepth = 0.0f,
        .MaxDepth = 1.0f,
    };
    cmd_list->RSSetViewports(1, &viewport);

    const D3D12_RECT scissor{
        .left = static_cast<LONG>(viewport_x),
        .top = static_cast<LONG>(viewport_y),
        .right = static_cast<LONG>(viewport_x + viewport_w),
        .bottom = static_cast<LONG>(viewport_y + viewport_h),
    };
    cmd_list->RSSetScissorRects(1, &scissor);

    for (PreparedView &prepared : prepared_views) {
        for (const auto &draw_cmd : prepared.compiled.draw_commands)
            draw_command(cmd_list, draw_cmd, viewport_x, viewport_y, viewport_w, viewport_h);

        const auto timestamp_us = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
                                      .count();
        prepared.view->update(static_cast<uint64_t>(timestamp_us));
    }

    prepared_views.clear();
}

void OverlayRenderer::retire(uint64_t fence_value) {
    upload.retire(fence_value);
}

} // namespace renderer::d3d12
