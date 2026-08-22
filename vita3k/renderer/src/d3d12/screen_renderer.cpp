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

#include <renderer/d3d12/screen_renderer.h>

#include <renderer/d3d12/shader_compile.h>
#include <renderer/d3d12/state.h>

#include <display/state.h>
#include <mem/state.h>
#include <util/align.h>
#include <util/log.h>

#include <algorithm>

namespace renderer::d3d12 {

// A fullscreen triangle generated from the vertex id, so no vertex buffer or
// input layout is needed.
static const char *SCREEN_VERTEX_HLSL = R"(
struct VSOutput {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

VSOutput main(uint vertex_id : SV_VertexID) {
    VSOutput output;
    // Ids 0,1,2 give uv (0,0), (2,0), (0,2): a triangle that covers the whole
    // viewport with the unit square inscribed in it.
    output.uv = float2((vertex_id << 1) & 2, vertex_id & 2);
    // Flip y: texture space runs downwards, clip space upwards.
    output.position = float4(output.uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return output;
}
)";

static const char *SCREEN_PIXEL_HLSL = R"(
Texture2D source_texture : register(t0);
SamplerState source_sampler : register(s0);

// Which part of the source holds the frame. A surface can be larger than the
// image being presented, and the image can sit at an offset inside it.
cbuffer SourceRegion : register(b0) {
    float2 uv_scale;
    float2 uv_offset;
};

struct VSOutput {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

float4 main(VSOutput input) : SV_Target {
    return source_texture.Sample(source_sampler, input.uv * uv_scale + uv_offset);
}
)";

ScreenRenderer::ScreenRenderer(DXState &state)
    : state(state) {
}

bool ScreenRenderer::create() {
    // Root signature: the source texture and its sampler, nothing else. Both are
    // tables because the sampler is swapped when the screen filter changes.
    const D3D12_DESCRIPTOR_RANGE srv_range{
        .RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV,
        .NumDescriptors = 1,
        .BaseShaderRegister = 0,
        .RegisterSpace = 0,
        .OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND,
    };

    const D3D12_DESCRIPTOR_RANGE sampler_range{
        .RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER,
        .NumDescriptors = 1,
        .BaseShaderRegister = 0,
        .RegisterSpace = 0,
        .OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND,
    };

    D3D12_ROOT_PARAMETER params[3] = {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[0].DescriptorTable = { 1, &srv_range };
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable = { 1, &sampler_range };
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    // Four floats is small enough to live directly in the root signature.
    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[2].Constants = { .ShaderRegister = 0, .RegisterSpace = 0, .Num32BitValues = 4 };
    params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    const D3D12_ROOT_SIGNATURE_DESC root_desc{
        .NumParameters = 3,
        .pParameters = params,
        .NumStaticSamplers = 0,
        .pStaticSamplers = nullptr,
        .Flags = D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS
            | D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS
            | D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS,
    };

    ComPtr<ID3DBlob> serialized;
    ComPtr<ID3DBlob> error;
    HRESULT hr = D3D12SerializeRootSignature(&root_desc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &error);
    if (FAILED(hr)) {
        const std::string message = error
            ? std::string(static_cast<const char *>(error->GetBufferPointer()), error->GetBufferSize())
            : hresult_to_string(hr);
        LOG_ERROR("D3D12: failed to serialize the screen root signature: {}", message);
        return false;
    }

    if (!dx_check(state.device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
                      IID_PPV_ARGS(&root_sig)),
            "creating the screen root signature"))
        return false;

    root_sig->SetName(L"Vita3K screen root signature");

    // Shaders. These go through the same DXC path as the GXM shaders, so a
    // missing dxcompiler.dll shows up here as a clear, single failure.
    std::vector<uint8_t> vertex_bytecode;
    std::vector<uint8_t> pixel_bytecode;
    std::string compile_error;

    if (!compile_hlsl_to_dxil(SCREEN_VERTEX_HLSL, ShaderStage::Vertex, "screen_vertex", vertex_bytecode, compile_error)) {
        LOG_ERROR("D3D12: could not compile the screen vertex shader: {}", compile_error);
        return false;
    }

    if (!compile_hlsl_to_dxil(SCREEN_PIXEL_HLSL, ShaderStage::Fragment, "screen_pixel", pixel_bytecode, compile_error)) {
        LOG_ERROR("D3D12: could not compile the screen pixel shader: {}", compile_error);
        return false;
    }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pipeline_desc{};
    pipeline_desc.pRootSignature = root_sig.Get();
    pipeline_desc.VS = { vertex_bytecode.data(), vertex_bytecode.size() };
    pipeline_desc.PS = { pixel_bytecode.data(), pixel_bytecode.size() };
    pipeline_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pipeline_desc.NumRenderTargets = 1;
    pipeline_desc.RTVFormats[0] = state.swapchain_format;
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

    pipeline_desc.BlendState.RenderTarget[0] = D3D12_RENDER_TARGET_BLEND_DESC{
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

    // No depth buffer is involved in presentation.
    pipeline_desc.DepthStencilState.DepthEnable = FALSE;
    pipeline_desc.DepthStencilState.StencilEnable = FALSE;
    pipeline_desc.DSVFormat = DXGI_FORMAT_UNKNOWN;

    if (!dx_check(state.device->CreateGraphicsPipelineState(&pipeline_desc, IID_PPV_ARGS(&pipeline)),
            "creating the screen pipeline state"))
        return false;

    pipeline->SetName(L"Vita3K screen pipeline");

    // Samplers: point at index 0, linear at index 1.
    if (!sampler_heap.create(state.device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, 2, true, L"Vita3K screen samplers"))
        return false;

    D3D12_SAMPLER_DESC sampler_desc{};
    sampler_desc.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler_desc.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler_desc.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler_desc.MaxLOD = D3D12_FLOAT32_MAX;
    sampler_desc.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;

    sampler_desc.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    state.device->CreateSampler(&sampler_desc, sampler_heap.at(0).cpu);

    sampler_desc.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    state.device->CreateSampler(&sampler_desc, sampler_heap.at(1).cpu);

    return true;
}

void ScreenRenderer::destroy() {
    for (SourceTexture &texture : source_textures) {
        if (texture.srv.valid()) {
            state.srv_staging_heap.free(texture.srv);
            texture.srv = {};
        }
        texture.image.reset();
        texture.width = 0;
        texture.height = 0;
    }

    sampler_heap.destroy();
    pipeline.Reset();
    root_sig.Reset();
}

bool ScreenRenderer::ensure_source_texture(SourceTexture &texture, uint32_t width, uint32_t height) {
    if (texture.image && texture.width == width && texture.height == height)
        return true;

    // The old texture may still be referenced by an in-flight frame, so hand it
    // to the frame destroy queue rather than releasing it here.
    if (texture.image)
        state.frame().destroy_queue.push_back(texture.image.resource);

    texture.image.reset();

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
        .Format = DXGI_FORMAT_R8G8B8A8_UNORM,
        .SampleDesc = { .Count = 1, .Quality = 0 },
        .Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN,
        .Flags = D3D12_RESOURCE_FLAG_NONE,
    };

    if (!dx_check(state.device->CreateCommittedResource(&heap_props, D3D12_HEAP_FLAG_NONE, &desc,
                      D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&texture.image.resource)),
            "creating the screen source texture"))
        return false;

    texture.image.state = D3D12_RESOURCE_STATE_COPY_DEST;
    texture.image.resource->SetName(L"Vita3K screen source");
    texture.width = width;
    texture.height = height;

    if (!texture.srv.valid()) {
        texture.srv = state.srv_staging_heap.allocate();
        if (!texture.srv.valid())
            return false;
    }

    const D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc{
        .Format = DXGI_FORMAT_R8G8B8A8_UNORM,
        .ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D,
        .Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING,
        .Texture2D = { .MostDetailedMip = 0, .MipLevels = 1, .PlaneSlice = 0, .ResourceMinLODClamp = 0.0f },
    };
    state.device->CreateShaderResourceView(texture.image.get(), &srv_desc, texture.srv.cpu);

    return true;
}

bool ScreenRenderer::render(ID3D12GraphicsCommandList *cmd_list, BarrierBatcher &barriers,
    const DisplayFrameInfo &frame, MemState &mem, const D3D12_VIEWPORT &viewport) {
    if (!pipeline || !frame.base)
        return false;

    const uint32_t width = static_cast<uint32_t>(frame.image_size.x);
    const uint32_t height = static_cast<uint32_t>(frame.image_size.y);
    if (width == 0 || height == 0)
        return false;

    FrameObject &frame_object = state.frame();
    SourceTexture &texture = source_textures[state.current_frame_idx];

    if (!ensure_source_texture(texture, width, height))
        return false;

    // Copy the guest framebuffer into the upload ring. D3D12 requires each row
    // of a texture copy to start on a 256-byte boundary, so the rows cannot be
    // memcpy'd in one go unless the pitches happen to agree.
    const uint32_t row_bytes = width * 4;
    const uint32_t upload_pitch = align(row_bytes, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);
    const uint64_t upload_size = static_cast<uint64_t>(upload_pitch) * height;

    UploadRingBuffer::Allocation allocation = frame_object.upload_buffer.allocate(upload_size);
    if (!allocation.valid()) {
        LOG_WARN_ONCE("D3D12: the frame upload buffer is too small to hold the display frame");
        return false;
    }

    const uint8_t *source = static_cast<const uint8_t *>(frame.base.get(mem));
    if (!source)
        return false;

    // frame.pitch is in pixels, not bytes.
    const uint32_t source_pitch = frame.pitch * 4;
    for (uint32_t y = 0; y < height; y++)
        memcpy(allocation.cpu + static_cast<uint64_t>(y) * upload_pitch, source + static_cast<uint64_t>(y) * source_pitch, row_bytes);

    barriers.transition(texture.image, D3D12_RESOURCE_STATE_COPY_DEST);
    barriers.flush(cmd_list);

    const D3D12_TEXTURE_COPY_LOCATION destination{
        .pResource = texture.image.get(),
        .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,
        .SubresourceIndex = 0,
    };

    D3D12_TEXTURE_COPY_LOCATION source_location{};
    source_location.pResource = allocation.resource;
    source_location.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    source_location.PlacedFootprint.Offset = allocation.offset;
    source_location.PlacedFootprint.Footprint = D3D12_SUBRESOURCE_FOOTPRINT{
        .Format = DXGI_FORMAT_R8G8B8A8_UNORM,
        .Width = width,
        .Height = height,
        .Depth = 1,
        .RowPitch = upload_pitch,
    };

    cmd_list->CopyTextureRegion(&destination, 0, 0, 0, &source_location, nullptr);

    barriers.transition(texture.image, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    barriers.flush(cmd_list);

    // The upload covers the whole texture, so the shader samples all of it.
    static constexpr float whole_texture_scale[2] = { 1.0f, 1.0f };
    static constexpr float whole_texture_offset[2] = { 0.0f, 0.0f };

    return draw_fullscreen(cmd_list, texture.srv, whole_texture_scale, whole_texture_offset, viewport);
}

bool ScreenRenderer::render_surface(ID3D12GraphicsCommandList *cmd_list, BarrierBatcher &barriers,
    Resource &image, DescriptorHandle image_srv, const Viewport &source, const D3D12_VIEWPORT &viewport) {
    if (!pipeline || !image || !image_srv.valid())
        return false;

    if (source.texture_width == 0 || source.texture_height == 0)
        return false;

    barriers.transition(image, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    barriers.flush(cmd_list);

    // The frame can be a sub-rectangle of a larger surface.
    const float uv_scale[2] = {
        static_cast<float>(source.width) / static_cast<float>(source.texture_width),
        static_cast<float>(source.height) / static_cast<float>(source.texture_height),
    };
    const float uv_offset[2] = {
        static_cast<float>(source.offset_x) / static_cast<float>(source.texture_width),
        static_cast<float>(source.offset_y) / static_cast<float>(source.texture_height),
    };

    return draw_fullscreen(cmd_list, image_srv, uv_scale, uv_offset, viewport);
}

bool ScreenRenderer::draw_fullscreen(ID3D12GraphicsCommandList *cmd_list, DescriptorHandle source_srv,
    const float uv_scale[2], const float uv_offset[2], const D3D12_VIEWPORT &viewport) {
    FrameObject &frame_object = state.frame();

    // A descriptor is only visible to the GPU from the heap bound for this draw,
    // so the long-lived CPU descriptor is copied into the frame ring each time.
    const DescriptorHandle srv = frame_object.view_descriptors.allocate(1);
    if (!srv.valid()) {
        LOG_WARN_ONCE("D3D12: ran out of frame view descriptors while presenting");
        return false;
    }

    state.device->CopyDescriptorsSimple(1, srv.cpu, source_srv.cpu, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    ID3D12DescriptorHeap *heaps[] = { frame_object.view_descriptors.handle(), sampler_heap.handle() };
    cmd_list->SetDescriptorHeaps(2, heaps);

    cmd_list->SetGraphicsRootSignature(root_sig.Get());
    cmd_list->SetGraphicsRootDescriptorTable(0, srv.gpu);
    cmd_list->SetGraphicsRootDescriptorTable(1, sampler_heap.at(use_linear_filter ? 1 : 0).gpu);

    const float region[4] = { uv_scale[0], uv_scale[1], uv_offset[0], uv_offset[1] };
    cmd_list->SetGraphicsRoot32BitConstants(2, 4, region, 0);

    cmd_list->SetPipelineState(pipeline.Get());
    cmd_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmd_list->RSSetViewports(1, &viewport);

    const D3D12_RECT scissor{
        .left = static_cast<LONG>(viewport.TopLeftX),
        .top = static_cast<LONG>(viewport.TopLeftY),
        .right = static_cast<LONG>(viewport.TopLeftX + viewport.Width),
        .bottom = static_cast<LONG>(viewport.TopLeftY + viewport.Height),
    };
    cmd_list->RSSetScissorRects(1, &scissor);

    cmd_list->DrawInstanced(3, 1, 0, 0);

    return true;
}

} // namespace renderer::d3d12
