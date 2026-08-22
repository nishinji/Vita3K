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

#include <renderer/d3d12/pipeline_cache.h>

#include <renderer/d3d12/gxm_to_d3d12.h>
#include <renderer/d3d12/state.h>
#include <renderer/d3d12/types.h>

#include <gxm/functions.h>
#include <util/align.h>
#include <util/log.h>
#include <util/string_utils.h>

#include <xxh3.h>

#include <chrono>

namespace renderer::d3d12 {

// Everything before vertex_streams is what the pipeline depends on; the same
// split the vulkan backend uses, so the two keys stay comparable.
constexpr size_t record_pipeline_len = offsetof(GxmRecordState, vertex_streams);

// How long to wait after a new pipeline appears before flushing the library.
constexpr uint64_t PIPELINE_CACHE_SAVE_DELAY_SECONDS = 10;

PipelineCache::PipelineCache(DXState &state)
    : state(state) {
}

// Root signature

bool PipelineCache::create_root_signature() {
    // Descriptor ranges. Register spaces mirror the SPIR-V descriptor sets; see
    // the table in shader_compile.h.
    D3D12_DESCRIPTOR_RANGE attachment_range{
        .RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV,
        .NumDescriptors = 3,
        .BaseShaderRegister = 0,
        .RegisterSpace = static_cast<UINT>(RegisterSpace::Attachments),
        .OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND,
    };

    D3D12_DESCRIPTOR_RANGE vertex_texture_range{
        .RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV,
        .NumDescriptors = SCE_GXM_MAX_TEXTURE_UNITS,
        .BaseShaderRegister = 0,
        .RegisterSpace = static_cast<UINT>(RegisterSpace::VertexTextures),
        .OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND,
    };

    D3D12_DESCRIPTOR_RANGE vertex_sampler_range{
        .RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER,
        .NumDescriptors = SCE_GXM_MAX_TEXTURE_UNITS,
        .BaseShaderRegister = 0,
        .RegisterSpace = static_cast<UINT>(RegisterSpace::VertexTextures),
        .OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND,
    };

    D3D12_DESCRIPTOR_RANGE fragment_texture_range = vertex_texture_range;
    fragment_texture_range.RegisterSpace = static_cast<UINT>(RegisterSpace::FragmentTextures);

    D3D12_DESCRIPTOR_RANGE fragment_sampler_range = vertex_sampler_range;
    fragment_sampler_range.RegisterSpace = static_cast<UINT>(RegisterSpace::FragmentTextures);

    D3D12_ROOT_PARAMETER params[ROOT_PARAM_COUNT] = {};

    auto set_root_descriptor = [&](uint32_t index, D3D12_ROOT_PARAMETER_TYPE type, UINT shader_register,
                                   RegisterSpace space, D3D12_SHADER_VISIBILITY visibility) {
        params[index].ParameterType = type;
        params[index].Descriptor.ShaderRegister = shader_register;
        params[index].Descriptor.RegisterSpace = static_cast<UINT>(space);
        params[index].ShaderVisibility = visibility;
    };

    auto set_table = [&](uint32_t index, const D3D12_DESCRIPTOR_RANGE &range, D3D12_SHADER_VISIBILITY visibility) {
        params[index].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[index].DescriptorTable.NumDescriptorRanges = 1;
        params[index].DescriptorTable.pDescriptorRanges = &range;
        params[index].ShaderVisibility = visibility;
    };

    set_root_descriptor(ROOT_PARAM_VERTEX_UNIFORM_BLOCK, D3D12_ROOT_PARAMETER_TYPE_CBV, 0,
        RegisterSpace::Uniforms, D3D12_SHADER_VISIBILITY_VERTEX);
    set_root_descriptor(ROOT_PARAM_FRAGMENT_UNIFORM_BLOCK, D3D12_ROOT_PARAMETER_TYPE_CBV, 1,
        RegisterSpace::Uniforms, D3D12_SHADER_VISIBILITY_PIXEL);
    set_root_descriptor(ROOT_PARAM_VERTEX_BUFFERS, D3D12_ROOT_PARAMETER_TYPE_SRV, 2,
        RegisterSpace::Uniforms, D3D12_SHADER_VISIBILITY_VERTEX);
    set_root_descriptor(ROOT_PARAM_FRAGMENT_BUFFERS, D3D12_ROOT_PARAMETER_TYPE_SRV, 3,
        RegisterSpace::Uniforms, D3D12_SHADER_VISIBILITY_PIXEL);

    set_table(ROOT_PARAM_ATTACHMENTS, attachment_range, D3D12_SHADER_VISIBILITY_PIXEL);
    set_table(ROOT_PARAM_VERTEX_TEXTURES, vertex_texture_range, D3D12_SHADER_VISIBILITY_VERTEX);
    set_table(ROOT_PARAM_VERTEX_SAMPLERS, vertex_sampler_range, D3D12_SHADER_VISIBILITY_VERTEX);
    set_table(ROOT_PARAM_FRAGMENT_TEXTURES, fragment_texture_range, D3D12_SHADER_VISIBILITY_PIXEL);
    set_table(ROOT_PARAM_FRAGMENT_SAMPLERS, fragment_sampler_range, D3D12_SHADER_VISIBILITY_PIXEL);

    const D3D12_ROOT_SIGNATURE_DESC desc{
        .NumParameters = ROOT_PARAM_COUNT,
        .pParameters = params,
        .NumStaticSamplers = 0,
        .pStaticSamplers = nullptr,
        .Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT
            | D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS
            | D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS
            | D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS,
    };

    ComPtr<ID3DBlob> serialized;
    ComPtr<ID3DBlob> error;
    const HRESULT hr = D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &error);
    if (FAILED(hr)) {
        const std::string message = error
            ? std::string(static_cast<const char *>(error->GetBufferPointer()), error->GetBufferSize())
            : hresult_to_string(hr);
        LOG_ERROR("D3D12: failed to serialize the root signature: {}", message);
        return false;
    }

    if (!dx_check(state.device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
                      IID_PPV_ARGS(&root_sig)),
            "creating the root signature"))
        return false;

    root_sig->SetName(L"Vita3K GXM root signature");
    return true;
}

bool PipelineCache::init() {
    if (!create_root_signature())
        return false;

    read_pipeline_cache();
    return true;
}

void PipelineCache::cleanup() {
    pipelines.clear();
    shaders.clear();
    pipeline_library.Reset();
    pipeline_library_blob.clear();
    root_sig.Reset();
}

// Shaders

ShaderBlob *PipelineCache::retrieve_shader(const SceGxmProgram *program, const Sha256Hash &hash, bool is_vertex,
    bool maskupdate, MemState &mem, const shader::Hints &hints) {
    {
        std::lock_guard<std::mutex> guard(shaders_mutex);
        auto it = shaders.find(hash);
        if (it != shaders.end())
            return it->second.valid() ? &it->second : nullptr;
    }

    const std::string hash_text = hex_string(hash);
    const std::string shader_name = fmt::format("{}-{}", hash_text, is_vertex ? "vert" : "frag");

    std::vector<uint8_t> bytecode;
    if (!load_cached_dxil(state.shaders_path, shader_name, bytecode)) {
        // Reuse the shared recompiler: the SPIR-V it produces for the vulkan
        // target is what the HLSL translation expects.
        const shader::GeneratedShader generated = shader::convert_gxp(*program, hash_text, state.features,
            shader::Target::SpirVVulkan, hints, maskupdate);

        if (generated.spirv.empty()) {
            LOG_ERROR("D3D12: the recompiler produced no SPIR-V for {}", shader_name);
        } else {
            const CompiledShader compiled = compile_spirv(generated.spirv,
                is_vertex ? ShaderStage::Vertex : ShaderStage::Fragment, shader_name);
            bytecode = compiled.bytecode;

            if (!bytecode.empty())
                save_cached_dxil(state.shaders_path, shader_name, bytecode);
        }
    }

    std::lock_guard<std::mutex> guard(shaders_mutex);
    // Another thread may have compiled the same shader while this one worked.
    auto [it, inserted] = shaders.try_emplace(hash);
    if (inserted)
        it->second.bytecode = std::move(bytecode);

    if (!it->second.valid())
        return nullptr;

    state.m_shaders_compiled_count++;
    state.m_shaders_compiled_time = std::chrono::steady_clock::now();

    return &it->second;
}

// Input layout

// How an attribute is fetched: the DXGI format, and how many vec4 elements a matrix attribute spans.
struct FetchedAttribute {
    SceGxmAttributeFormat format;
    uint8_t component_count;
    uint32_t array_size;
    uint32_t array_element_size;
};

static FetchedAttribute fetched_attribute(const SceGxmVertexAttribute &attribute, const shader::usse::AttributeInformation &info) {
    FetchedAttribute fetched{
        .format = static_cast<SceGxmAttributeFormat>(attribute.format),
        .component_count = attribute.componentCount,
        // Non-one values only occur when a matrix is passed as an attribute; it
        // is then packed as an array of vec4.
        .array_size = 1,
        .array_element_size = 0,
    };

    // Register-format attributes are fetched raw, as bytes, shorts or words.
    if (info.regformat) {
        switch (info.gxm_type) {
        case SCE_GXM_PARAMETER_TYPE_U8:
        case SCE_GXM_PARAMETER_TYPE_S8:
        case SCE_GXM_PARAMETER_TYPE_C10:
            fetched.format = SCE_GXM_ATTRIBUTE_FORMAT_U8;
            break;
        case SCE_GXM_PARAMETER_TYPE_U16:
        case SCE_GXM_PARAMETER_TYPE_S16:
        case SCE_GXM_PARAMETER_TYPE_F16:
            fetched.format = SCE_GXM_ATTRIBUTE_FORMAT_U16;
            break;
        default:
            fetched.format = SCE_GXM_ATTRIBUTE_FORMAT_UNTYPED;
            break;
        }

        fetched.component_count = static_cast<uint8_t>(info.component_count);
        if (info.gxm_type == SCE_GXM_PARAMETER_TYPE_C10)
            // 10-bit rather than 8-bit components
            fetched.component_count = static_cast<uint8_t>((fetched.component_count * 10 + 7) / 8);

        if (fetched.component_count > 4) {
            fetched.array_size = (fetched.component_count + 3) / 4;
            fetched.array_element_size = 4 * gxm::attribute_format_size(fetched.format);
            fetched.component_count = 4;
        }
    }

    // No three-component 8/16-bit DXGI format exists; widen to four. The
    // shader input stays a three-component vector, so the extra channel is
    // simply discarded.
    if (fetched.component_count == 3 && !is_attribute_format_supported(fetched.format, fetched.component_count))
        fetched.component_count = 4;

    return fetched;
}

static uint32_t fetched_size(const FetchedAttribute &fetched) {
    if (fetched.array_size > 1)
        return fetched.array_size * fetched.array_element_size;
    return gxm::attribute_format_size(fetched.format) * fetched.component_count;
}

// Calls `visit(attribute, info, fetched, offset)` for every attribute the shader
// reads, with the offset it has in the uploaded stream.
template <typename Visit>
static void for_each_attribute(const SceGxmVertexProgram &vertex_program, Visit &&visit) {
    const VertexProgram *vp = vertex_program.renderer_data.get();
    if (!vp)
        return;

    struct Entry {
        const SceGxmVertexAttribute *attribute;
        const shader::usse::AttributeInformation *info;
        FetchedAttribute fetched;
    };
    std::vector<Entry> entries;
    std::array<bool, SCE_GXM_MAX_VERTEX_STREAMS> repacked{};

    for (const SceGxmVertexAttribute &attribute : vertex_program.attributes) {
        if (!vp->attribute_infos.contains(attribute.regIndex) || attribute.streamIndex >= SCE_GXM_MAX_VERTEX_STREAMS)
            continue;

        const shader::usse::AttributeInformation &info = vp->attribute_infos.at(attribute.regIndex);
        const FetchedAttribute fetched = fetched_attribute(attribute, info);
        // D3D12 rejects a misaligned offset, and a misaligned stride makes the hardware fetch garbage for every later vertex.
        const uint32_t alignment = std::min(gxm::attribute_format_size(fetched.format) * fetched.component_count, 4u);
        const uint32_t stride = vertex_program.streams[attribute.streamIndex].stride;
        if (alignment > 1 && ((attribute.offset % alignment) != 0 || (stride % alignment) != 0))
            repacked[attribute.streamIndex] = true;

        entries.push_back({ &attribute, &info, fetched });
    }

    std::array<uint32_t, SCE_GXM_MAX_VERTEX_STREAMS> cursor{};
    for (const Entry &entry : entries) {
        const uint32_t stream = entry.attribute->streamIndex;
        uint32_t offset = entry.attribute->offset;
        if (repacked[stream]) {
            offset = cursor[stream];
            cursor[stream] = align(offset + fetched_size(entry.fetched), 4);
        }

        visit(*entry.attribute, *entry.info, entry.fetched, offset, repacked[stream]);
    }
}

std::array<StreamRepack, SCE_GXM_MAX_VERTEX_STREAMS> plan_stream_repack(const SceGxmVertexProgram &vertex_program) {
    std::array<StreamRepack, SCE_GXM_MAX_VERTEX_STREAMS> plan;
    for_each_attribute(vertex_program, [&](const SceGxmVertexAttribute &attribute, const shader::usse::AttributeInformation &, const FetchedAttribute &fetched, uint32_t offset, bool repacked) {
        if (!repacked)
            return;

        StreamRepack &stream = plan[attribute.streamIndex];
        const uint32_t size = fetched_size(fetched);
        stream.repacked = true;
        stream.copies.push_back({ attribute.offset, offset, size });
        // A guest stride of 0 repeats one vertex, and so does the copy.
        if (vertex_program.streams[attribute.streamIndex].stride != 0)
            stream.stride = std::max(stream.stride, align(offset + size, 4));
    });
    return plan;
}

std::vector<D3D12_INPUT_ELEMENT_DESC> build_input_layout(const SceGxmVertexProgram &vertex_program, MemState &mem) {
    std::vector<D3D12_INPUT_ELEMENT_DESC> elements;

    for_each_attribute(vertex_program, [&](const SceGxmVertexAttribute &attribute, const shader::usse::AttributeInformation &info, const FetchedAttribute &fetched, uint32_t offset, bool) {
        // DXGI has no scaled formats, so every attribute is fed to the shader as
        // an integer and converted there.
        const DXGI_FORMAT format = translate_attribute_format(fetched.format, fetched.component_count,
            true, info.is_signed);
        if (format == DXGI_FORMAT_UNKNOWN) {
            LOG_ERROR("D3D12: no DXGI format for attribute format {} with {} components",
                log_hex(fetched.format), fetched.component_count);
            return;
        }

        const SceGxmVertexStream &stream = vertex_program.streams[attribute.streamIndex];
        const bool is_instanced = gxm::is_stream_instancing(static_cast<SceGxmIndexSource>(stream.indexSource));
        for (uint32_t i = 0; i < fetched.array_size; i++) {
            elements.push_back(D3D12_INPUT_ELEMENT_DESC{
                // SPIRV-Cross names HLSL vertex inputs TEXCOORD<location>.
                .SemanticName = "TEXCOORD",
                .SemanticIndex = info.location + i,
                .Format = format,
                .InputSlot = attribute.streamIndex,
                .AlignedByteOffset = offset + i * fetched.array_element_size,
                .InputSlotClass = is_instanced
                    ? D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA
                    : D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,
                .InstanceDataStepRate = is_instanced ? 1u : 0u,
            });
        }
    });

    return elements;
}

// Pipeline states

static D3D12_DEPTH_STENCILOP_DESC build_stencil_op(const GxmStencilStateOp &op) {
    return D3D12_DEPTH_STENCILOP_DESC{
        .StencilFailOp = translate_stencil_op(op.stencil_fail),
        .StencilDepthFailOp = translate_stencil_op(op.depth_fail),
        .StencilPassOp = translate_stencil_op(op.depth_pass),
        .StencilFunc = translate_stencil_func(op.func),
    };
}

ID3D12PipelineState *PipelineCache::retrieve_pipeline(DXContext &context, SceGxmPrimitiveType type, MemState &mem) {
    const GxmRecordState &record = context.record;

    // Same key recipe as the vulkan backend, so the two caches stay in step.
    uint64_t key = XXH3_64bits(&record, record_pipeline_len);

    SceGxmFragmentProgram &fragment_program_gxm = *record.fragment_program.get(mem);
    const DXFragmentProgram &fragment_program = *reinterpret_cast<DXFragmentProgram *>(
        fragment_program_gxm.renderer_data.get());
    key ^= fragment_program.blending_hash;

    SceGxmVertexProgram &vertex_program_gxm = *record.vertex_program.get(mem);
    key ^= vertex_program_gxm.key_hash;

    key ^= static_cast<uint64_t>(type);
    // The render target formats are baked into a D3D12 PSO, unlike a vulkan
    // pipeline where they live in the render pass.
    key ^= static_cast<uint64_t>(context.current_color_format) << 8;
    key ^= static_cast<uint64_t>(context.current_depth_format) << 24;

    // The stencil masks live past vertex_streams, so record_pipeline_len does
    // not cover them. Vulkan can leave them dynamic; D3D12 bakes them into the
    // PSO, so a change of mask has to produce a different pipeline.
    key ^= static_cast<uint64_t>(record.front_stencil_state_values.compare_mask) << 40;
    key ^= static_cast<uint64_t>(record.front_stencil_state_values.write_mask) << 48;

    // Depth bias is dynamic state in vulkan but rasterizer state in D3D12, so it
    // also has to distinguish pipelines.
    key ^= static_cast<uint64_t>(static_cast<uint32_t>(record.depth_bias_unit)) << 16;
    key ^= static_cast<uint64_t>(static_cast<uint32_t>(record.depth_bias_slope)) << 32;

    {
        std::lock_guard<std::mutex> guard(pipelines_mutex);
        auto it = pipelines.find(key);
        // A null entry is a pipeline that was already rejected once; returning it
        // skips the draw again without paying for the rebuild.
        if (it != pipelines.end())
            return it->second.Get();
    }

    // Shader hints have to reflect the surface currently bound, exactly as in
    // the vulkan pipeline cache.
    context.shader_hints.color_format = record.color_surface.colorFormat;
    context.shader_hints.attributes = &vertex_program_gxm.attributes;

    const SceGxmProgram *gxm_vertex_shader = vertex_program_gxm.program.get(mem);
    const SceGxmProgram *gxm_fragment_shader = fragment_program_gxm.program.get(mem);

    ShaderBlob *vertex_blob = retrieve_shader(gxm_vertex_shader, record.vertex_program_hash, true, false, mem, context.shader_hints);
    ShaderBlob *fragment_blob = retrieve_shader(gxm_fragment_shader, record.fragment_program_hash, false,
        record.is_maskupdate, mem, context.shader_hints);

    if (!vertex_blob || !fragment_blob)
        return nullptr;

    const std::vector<D3D12_INPUT_ELEMENT_DESC> input_layout = build_input_layout(vertex_program_gxm, mem);

    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
    desc.pRootSignature = root_sig.Get();
    desc.VS = vertex_blob->view();
    desc.PS = fragment_blob->view();
    desc.InputLayout = { input_layout.data(), static_cast<UINT>(input_layout.size()) };
    desc.PrimitiveTopologyType = translate_primitive_type(type);
    desc.SampleMask = UINT_MAX;
    desc.SampleDesc = { .Count = context.current_sample_count, .Quality = 0 };

    // Rasterizer.
    desc.RasterizerState = D3D12_RASTERIZER_DESC{
        .FillMode = translate_polygon_mode(record.front_polygon_mode),
        .CullMode = translate_cull_mode(record.cull_mode),
        // flip_vert_y leaves screen-space winding as in vulkan, where GXM front faces are counter-clockwise.
        .FrontCounterClockwise = TRUE,
        .DepthBias = record.depth_bias_unit,
        .DepthBiasClamp = D3D12_DEFAULT_DEPTH_BIAS_CLAMP,
        .SlopeScaledDepthBias = static_cast<FLOAT>(record.depth_bias_slope),
        // GXM never clips against the near/far planes the way D3D does by
        // default; depth clipping off matches the vulkan depth clamp setup.
        .DepthClipEnable = FALSE,
        .MultisampleEnable = context.current_sample_count > 1,
        .AntialiasedLineEnable = FALSE,
        .ForcedSampleCount = 0,
        .ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF,
    };

    // Depth and stencil.
    const bool has_depth = context.current_depth_format != DXGI_FORMAT_UNKNOWN;

    // GXM and vulkan carry a compare/write mask per face; D3D12 has a single
    // pair shared by both. The front values win when the two disagree.
    const bool two_sided = record.two_sided == SCE_GXM_TWO_SIDED_ENABLED;
    if (two_sided
        && (record.front_stencil_state_values.compare_mask != record.back_stencil_state_values.compare_mask
            || record.front_stencil_state_values.write_mask != record.back_stencil_state_values.write_mask)) {
        LOG_WARN_ONCE("D3D12: per-face stencil masks are not expressible, using the front face values");
    }
    desc.DepthStencilState = D3D12_DEPTH_STENCIL_DESC{
        .DepthEnable = has_depth,
        .DepthWriteMask = record.front_depth_write_mode == SCE_GXM_DEPTH_WRITE_ENABLED
            ? D3D12_DEPTH_WRITE_MASK_ALL
            : D3D12_DEPTH_WRITE_MASK_ZERO,
        .DepthFunc = translate_depth_func(record.front_depth_func),
        .StencilEnable = has_depth,
        // D3D12 bakes the masks into the PSO, while GXM treats them as dynamic
        // state. They are therefore part of the pipeline key through the record.
        .StencilReadMask = record.front_stencil_state_values.compare_mask,
        .StencilWriteMask = record.front_stencil_state_values.write_mask,
        .FrontFace = build_stencil_op(record.front_stencil_state_op),
        .BackFace = build_stencil_op(two_sided ? record.back_stencil_state_op : record.front_stencil_state_op),
    };
    desc.DSVFormat = context.current_depth_format;

    // Blending. Only one render target is ever bound.
    desc.BlendState = D3D12_BLEND_DESC{
        .AlphaToCoverageEnable = FALSE,
        .IndependentBlendEnable = FALSE,
    };
    desc.BlendState.RenderTarget[0] = fragment_program.blending;

    if (context.current_color_format != DXGI_FORMAT_UNKNOWN) {
        desc.NumRenderTargets = 1;
        desc.RTVFormats[0] = context.current_color_format;
    }

    ComPtr<ID3D12PipelineState> pipeline;

    // The pipeline library keys on a name; the hash is unique and stable, which
    // is exactly what it needs.
    const std::wstring pipeline_name = string_utils::utf_to_wide(fmt::format("pso-{:016x}", key));

    if (pipeline_library) {
        const HRESULT hr = pipeline_library->LoadGraphicsPipeline(pipeline_name.c_str(), &desc, IID_PPV_ARGS(&pipeline));
        if (FAILED(hr) && hr != E_INVALIDARG) {
            // E_INVALIDARG simply means "not in the library yet".
            LOG_WARN("D3D12: reading a pipeline from the library failed: {}", hresult_to_string(hr));
        }
    }

    if (!pipeline) {
        const bool created = dx_check(state.device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pipeline)),
            "creating a graphics pipeline state");

        // A rejected pipeline is always accompanied by a validation message
        // naming the offending field; pull it out now, while the pipeline that
        // caused it is still the subject of the log.
        if (!created) {
            LOG_ERROR("D3D12: the rejected pipeline had key {:016x}, colour format {}, depth format {}, vertex {}, fragment {}",
                key, static_cast<int>(desc.RTVFormats[0]), static_cast<int>(desc.DSVFormat),
                hex_string(record.vertex_program_hash), hex_string(record.fragment_program_hash));

            // The input layout is the usual culprit and the validation message
            // does not say which element is at fault, so spell the whole thing
            // out.
            for (size_t i = 0; i < input_layout.size(); i++) {
                const D3D12_INPUT_ELEMENT_DESC &element = input_layout[i];
                LOG_ERROR("D3D12:   input[{}] TEXCOORD{} format {} slot {} offset {} {}",
                    i, element.SemanticIndex, static_cast<int>(element.Format),
                    element.InputSlot, element.AlignedByteOffset,
                    element.InputSlotClass == D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA ? "per-instance" : "per-vertex");
            }
            state.drain_debug_messages();

            // Remember the failure. Without this the same pipeline is rebuilt
            // for every draw that wants it, which floods the log and costs more
            // than the draw would have.
            std::lock_guard<std::mutex> guard(pipelines_mutex);
            pipelines.try_emplace(key, nullptr);
            return nullptr;
        }

        if (pipeline_library) {
            const HRESULT hr = pipeline_library->StorePipeline(pipeline_name.c_str(), pipeline.Get());
            if (SUCCEEDED(hr)) {
                const auto now = std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::system_clock::now().time_since_epoch())
                                     .count();
                next_pipeline_cache_save = static_cast<uint64_t>(now) + PIPELINE_CACHE_SAVE_DELAY_SECONDS;
            }
        }

        state.shaders_count_compiled++;
    }

    std::lock_guard<std::mutex> guard(pipelines_mutex);
    auto [it, inserted] = pipelines.try_emplace(key, pipeline);
    return it->second.Get();
}

// Disk cache

static fs::path pipeline_library_path(const DXState &state) {
    // Bump when the PSO description or the shaders change: stale entries keep their names, so new pipelines could never be stored.
    return state.shaders_path / fmt::format("pipeline-cache-dx-v2-{}.bin", DXIL_CACHE_VERSION);
}

void PipelineCache::read_pipeline_cache() {
    ComPtr<ID3D12Device1> device1;
    if (FAILED(state.device.As(&device1))) {
        LOG_INFO("D3D12: ID3D12Device1 is unavailable, pipeline state caching is disabled");
        return;
    }

    const fs::path path = pipeline_library_path(state);
    fs::ifstream file(path, std::ios::in | std::ios::binary | std::ios::ate);
    if (file.is_open()) {
        const std::streamsize size = file.tellg();
        if (size > 0) {
            file.seekg(0, std::ios::beg);
            pipeline_library_blob.resize(static_cast<size_t>(size));
            if (!file.read(reinterpret_cast<char *>(pipeline_library_blob.data()), size))
                pipeline_library_blob.clear();
        }
    }

    // The blob has to outlive the library, which reads straight out of it.
    HRESULT hr = device1->CreatePipelineLibrary(pipeline_library_blob.data(),
        pipeline_library_blob.size(), IID_PPV_ARGS(&pipeline_library));

    if (FAILED(hr) && !pipeline_library_blob.empty()) {
        // A driver or hardware change invalidates the blob; start over rather
        // than lose pipeline caching for the whole session.
        LOG_INFO("D3D12: the stored pipeline cache is no longer usable, recreating it");
        pipeline_library_blob.clear();
        hr = device1->CreatePipelineLibrary(nullptr, 0, IID_PPV_ARGS(&pipeline_library));
    }

    if (FAILED(hr)) {
        LOG_WARN("D3D12: could not create a pipeline library: {}", hresult_to_string(hr));
        pipeline_library.Reset();
    }
}

void PipelineCache::save_pipeline_cache() {
    if (!pipeline_library)
        return;

    const SIZE_T size = pipeline_library->GetSerializedSize();
    if (size == 0)
        return;

    std::vector<uint8_t> blob(size);
    if (!dx_check(pipeline_library->Serialize(blob.data(), size), "serializing the pipeline library"))
        return;

    fs::create_directories(state.shaders_path);

    fs::ofstream file(pipeline_library_path(state), std::ios::out | std::ios::binary | std::ios::trunc);
    if (!file.is_open()) {
        LOG_WARN("D3D12: could not write the pipeline cache");
        return;
    }

    file.write(reinterpret_cast<const char *>(blob.data()), static_cast<std::streamsize>(blob.size()));
}

} // namespace renderer::d3d12
