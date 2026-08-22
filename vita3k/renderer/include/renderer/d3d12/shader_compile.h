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

#include <util/fs.h>

#include <cstdint>
#include <string>
#include <vector>

namespace renderer::d3d12 {

// Register-space layout shared by the generated HLSL and the root signature.
//
// The SPIR-V produced by the shared recompiler for the Vulkan target uses four
// descriptor sets, and each one is mapped straight onto the HLSL register space
// of the same number. Keeping the binding index as the register index means the
// two sides cannot drift:
//
//   space0  b0        vertex render uniform block (RenderVertUniformBlock)
//   space0  b1        fragment render uniform block (RenderFragUniformBlock)
//   space0  t2        vertex GXM uniform buffer   (storage buffer)
//   space0  t3        fragment GXM uniform buffer (storage buffer)
//   space1  t0        colour attachment (framebuffer fetch emulation)
//   space1  t1        mask texture
//   space1  t2        raw colour attachment
//   space2  t0..t15   vertex textures,   s0..s15 their samplers
//   space3  t0..t15   fragment textures, s0..s15 their samplers
enum class RegisterSpace : uint32_t {
    Uniforms = 0,
    Attachments = 1,
    VertexTextures = 2,
    FragmentTextures = 3,
};

enum class ShaderStage {
    Vertex,
    Fragment,
};

struct CompiledShader {
    // DXIL container, ready to be handed to a pipeline state object.
    std::vector<uint8_t> bytecode;
    // Kept for shader debugging and for dumping alongside the GLSL the other
    // backends write out.
    std::string hlsl;

    bool valid() const {
        return !bytecode.empty();
    }
};

// Translate the recompiler's SPIR-V into HLSL for shader model 6.0, applying the
// register mapping documented above. Returns an empty string on failure.
std::string spirv_to_hlsl(const std::vector<uint32_t> &spirv, ShaderStage stage);

// Compile HLSL to a DXIL container through DXC. `error` receives the compiler
// diagnostics when compilation fails.
bool compile_hlsl_to_dxil(const std::string &hlsl, ShaderStage stage, const std::string &shader_name,
    std::vector<uint8_t> &bytecode, std::string &error);

// spirv_to_hlsl followed by compile_hlsl_to_dxil.
CompiledShader compile_spirv(const std::vector<uint32_t> &spirv, ShaderStage stage, const std::string &shader_name);

// Whether DXC could be loaded. Compilation always fails when this is false, so
// callers can report the cause once instead of per shader.
bool is_shader_compiler_available();
// Human-readable reason DXC is unavailable; empty when it loaded fine.
std::string_view shader_compiler_unavailable_reason();

// DXIL disk cache. Blobs are stored per shader hash so a warm start skips both
// the SPIR-V translation and the DXC invocation.
//
// Bumped whenever the SPIR-V to HLSL translation changes in a way that alters
// the generated code. The shader hash only covers the GXM program, so without
// this a cached blob from an older translation would be reused forever.
constexpr uint32_t DXIL_CACHE_VERSION = 5;
bool load_cached_dxil(const fs::path &shaders_path, const std::string &shader_name, std::vector<uint8_t> &bytecode);
void save_cached_dxil(const fs::path &shaders_path, const std::string &shader_name, const std::vector<uint8_t> &bytecode);

} // namespace renderer::d3d12
