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
#include <renderer/d3d12/shader_compile.h>
#include <renderer/types.h>

#include <shader/spirv_recompiler.h>

#include <array>
#include <map>
#include <mutex>
#include <vector>

struct MemState;

namespace renderer::d3d12 {

struct DXState;
struct DXContext;

// Slots in the shared root signature.
//
// The numbering is part of the contract with the generated HLSL: each entry
// binds the registers documented in shader_compile.h, and the draw path sets
// them by these indices. Changing the order here without changing the register
// mapping there silently binds the wrong resources.
enum RootParameter : uint32_t {
    // Root descriptors, so the dynamic offsets the ring buffers hand out cost
    // nothing to apply. These match the vulkan dynamic uniform/storage buffers.
    ROOT_PARAM_VERTEX_UNIFORM_BLOCK = 0, // CBV b0, space0, vertex
    ROOT_PARAM_FRAGMENT_UNIFORM_BLOCK, // CBV b1, space0, pixel
    ROOT_PARAM_VERTEX_BUFFERS, // SRV t2, space0, vertex
    ROOT_PARAM_FRAGMENT_BUFFERS, // SRV t3, space0, pixel

    // Descriptor tables, filled from the per-frame shader-visible heaps.
    ROOT_PARAM_ATTACHMENTS, // SRV t0-t2, space1, pixel
    ROOT_PARAM_VERTEX_TEXTURES, // SRV t0-t15, space2, vertex
    ROOT_PARAM_VERTEX_SAMPLERS, // sampler s0-s15, space2, vertex
    ROOT_PARAM_FRAGMENT_TEXTURES, // SRV t0-t15, space3, pixel
    ROOT_PARAM_FRAGMENT_SAMPLERS, // sampler s0-s15, space3, pixel

    ROOT_PARAM_COUNT
};

// Compiled DXIL for one GXM program, kept alive for as long as PSOs reference it.
struct ShaderBlob {
    std::vector<uint8_t> bytecode;

    D3D12_SHADER_BYTECODE view() const {
        return { bytecode.data(), bytecode.size() };
    }
    bool valid() const {
        return !bytecode.empty();
    }
};

class PipelineCache {
public:
    DXState &state;

    explicit PipelineCache(DXState &state);

    bool init();
    void cleanup();

    ID3D12RootSignature *root_signature() const {
        return root_sig.Get();
    }

    // Compile (or fetch) the DXIL for a GXM program. Returns nullptr when the
    // translation or the DXC invocation failed.
    ShaderBlob *retrieve_shader(const SceGxmProgram *program, const Sha256Hash &hash, bool is_vertex,
        bool maskupdate, MemState &mem, const shader::Hints &hints);

    // Build (or fetch) the pipeline state matching the context's record state.
    ID3D12PipelineState *retrieve_pipeline(DXContext &context, SceGxmPrimitiveType type, MemState &mem);

    void read_pipeline_cache();
    void save_pipeline_cache();

    // Wall-clock second at which the pipeline library should next be flushed to
    // disk; set when a new pipeline is added.
    uint64_t next_pipeline_cache_save = ~0ULL;

private:
    ComPtr<ID3D12RootSignature> root_sig;

    // D3D12's own driver-level cache. Serialised alongside the shader cache so a
    // warm start skips PSO creation cost as well as shader compilation.
    ComPtr<ID3D12PipelineLibrary> pipeline_library;
    std::vector<uint8_t> pipeline_library_blob;

    std::map<Sha256Hash, ShaderBlob> shaders;
    std::map<uint64_t, ComPtr<ID3D12PipelineState>> pipelines;

    // retrieve_shader and retrieve_pipeline are called from the render thread
    // and from the precompile workers.
    std::mutex shaders_mutex;
    std::mutex pipelines_mutex;

    bool create_root_signature();
};

// Build the D3D12 input layout for a GXM vertex program. The semantics are
// TEXCOORD<location>, which is what SPIRV-Cross emits for HLSL vertex inputs.
std::vector<D3D12_INPUT_ELEMENT_DESC> build_input_layout(const SceGxmVertexProgram &vertex_program, MemState &mem);

// D3D12 rejects an attribute whose offset is not a multiple of the smaller of 4
// and its format size, and vulkan does not. A stream holding one is copied into
// an aligned layout when it is uploaded, and the input layout describes that copy.
struct StreamRepack {
    struct Copy {
        uint32_t source;
        uint32_t destination;
        uint32_t size;
    };

    bool repacked = false;
    uint32_t stride = 0;
    std::vector<Copy> copies;
};

std::array<StreamRepack, SCE_GXM_MAX_VERTEX_STREAMS> plan_stream_repack(const SceGxmVertexProgram &vertex_program);

} // namespace renderer::d3d12
