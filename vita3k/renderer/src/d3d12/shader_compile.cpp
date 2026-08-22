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

#include <renderer/d3d12/shader_compile.h>

#include <util/log.h>
#include <util/string_utils.h>

#include <spirv_hlsl.hpp>

#include <dxcapi.h>

#include <algorithm>
#include <filesystem>
#include <mutex>
#include <vector>

namespace renderer::d3d12 {

// Shader model 6.0 is the first that requires DXIL, which is what DXC emits.
// Anything the recompiler produces fits comfortably inside 6.0.
constexpr uint32_t HLSL_SHADER_MODEL = 60;

// DXC loading
//
// dxcompiler.dll is not part of a stock Windows install, so it is resolved at
// runtime rather than linked against. Failing to find it disables shader
// compilation but must not stop the emulator from starting.
namespace {

class DxcLoader {
public:
    static DxcLoader &instance() {
        static DxcLoader loader;
        return loader;
    }

    bool available() const {
        return compiler != nullptr && utils != nullptr;
    }

    std::string_view unavailable_reason() const {
        return reason;
    }

    IDxcCompiler3 *get_compiler() const {
        return compiler.Get();
    }
    IDxcUtils *get_utils() const {
        return utils.Get();
    }

private:
    DxcLoader() {
        load();
    }

    ~DxcLoader() {
        // The COM objects have to go before the library that defines them.
        compiler.Reset();
        utils.Reset();
        if (library)
            FreeLibrary(library);
    }

    // Directory holding the redistributable DXC, next to the executable.
    static std::filesystem::path dxc_directory() {
        std::vector<wchar_t> buffer(MAX_PATH);
        const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0 || length >= buffer.size())
            return {};

        return std::filesystem::path(buffer.data()).parent_path() / L"dxc";
    }

    void load() {
        // DXC must NOT sit next to the executable. Graphics drivers load their
        // own dxcompiler.dll by name, and the module search order puts the
        // executable directory first, so a copy there gets picked up by the
        // driver instead of its own and crashes it -- the AMD driver
        // (amdxc64.dll) faults inside D3D12CreateDevice when that happens.
        // Keeping it in a subdirectory and loading it by absolute path avoids
        // shadowing anything.
        const std::filesystem::path directory = dxc_directory();
        const std::wstring compiler_path = (directory / L"dxcompiler.dll").wstring();

        // LOAD_WITH_ALTERED_SEARCH_PATH makes the loader resolve dxcompiler's own
        // dependency on dxil.dll from that same directory.
        library = LoadLibraryExW(compiler_path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!library) {
            reason = "dxcompiler.dll could not be loaded from the dxc directory next to the "
                     "Vita3K executable; the DirectX 12 backend cannot compile shaders without it";
            LOG_ERROR("D3D12: {}", reason);
            return;
        }

        auto create_instance = reinterpret_cast<DxcCreateInstanceProc>(
            GetProcAddress(library, "DxcCreateInstance"));
        if (!create_instance) {
            reason = "dxcompiler.dll does not export DxcCreateInstance";
            LOG_ERROR("D3D12: {}", reason);
            return;
        }

        if (FAILED(create_instance(CLSID_DxcUtils, IID_PPV_ARGS(&utils)))) {
            reason = "could not create the DXC utils object";
            LOG_ERROR("D3D12: {}", reason);
            return;
        }

        if (FAILED(create_instance(CLSID_DxcCompiler, IID_PPV_ARGS(&compiler)))) {
            reason = "could not create the DXC compiler object";
            LOG_ERROR("D3D12: {}", reason);
            utils.Reset();
            return;
        }

        LOG_INFO("D3D12: loaded the DirectX Shader Compiler");
    }

    HMODULE library = nullptr;
    ComPtr<IDxcUtils> utils;
    ComPtr<IDxcCompiler3> compiler;
    std::string reason;
};

// DXC is documented as thread-compatible rather than thread-safe, and the
// shader cache is warmed from several threads.
std::mutex dxc_mutex;

spv::ExecutionModel to_execution_model(ShaderStage stage) {
    return stage == ShaderStage::Vertex ? spv::ExecutionModelVertex : spv::ExecutionModelFragment;
}

// Register every resource the module declares at (space = descriptor set,
// register = binding), which is the mapping the root signature is built from.
void apply_register_mapping(spirv_cross::CompilerHLSL &compiler, ShaderStage stage) {
    const spirv_cross::ShaderResources resources = compiler.get_shader_resources();
    const spv::ExecutionModel model = to_execution_model(stage);

    auto map_resource = [&](const spirv_cross::Resource &resource) {
        const uint32_t set = compiler.get_decoration(resource.id, spv::DecorationDescriptorSet);
        const uint32_t bind = compiler.get_decoration(resource.id, spv::DecorationBinding);

        spirv_cross::HLSLResourceBinding binding{};
        binding.stage = model;
        binding.desc_set = set;
        binding.binding = bind;
        // Only the entry matching the resource type is consulted, so filling all
        // four keeps this one function correct for every resource kind.
        binding.cbv.register_space = set;
        binding.cbv.register_binding = bind;
        binding.srv.register_space = set;
        binding.srv.register_binding = bind;
        binding.uav.register_space = set;
        binding.uav.register_binding = bind;
        binding.sampler.register_space = set;
        binding.sampler.register_binding = bind;

        compiler.add_hlsl_resource_binding(binding);
    };

    for (const auto &resource : resources.uniform_buffers)
        map_resource(resource);
    for (const auto &resource : resources.storage_buffers)
        map_resource(resource);
    for (const auto &resource : resources.sampled_images)
        map_resource(resource);
    for (const auto &resource : resources.separate_images)
        map_resource(resource);
    for (const auto &resource : resources.separate_samplers)
        map_resource(resource);
    for (const auto &resource : resources.storage_images)
        map_resource(resource);
    for (const auto &resource : resources.subpass_inputs)
        map_resource(resource);
}

std::string blob_to_string(IDxcBlobUtf8 *blob) {
    if (!blob || blob->GetStringLength() == 0)
        return {};

    return std::string(blob->GetStringPointer(), blob->GetStringLength());
}

} // namespace

bool is_shader_compiler_available() {
    return DxcLoader::instance().available();
}

std::string_view shader_compiler_unavailable_reason() {
    return DxcLoader::instance().unavailable_reason();
}

// D3D12 links a vertex and pixel shader by comparing their signatures, and the
// register a stage-IO element lands in is decided by its position in the struct.
// SPIRV-Cross emits only the locations a stage actually uses, so a pixel shader
// reading a subset of the vertex outputs ends up with the same semantic in a
// different register, which the runtime rejects with a linkage error.
//
// Both stages therefore get one fixed layout: every location the recompiler can
// assign, in order, with unused float4 members in the gaps, then SV_Position and
// only after it anything else. The shaders are compiled separately and cached
// per GXM program, so neither one can see the other's signature; see the note in
// PipelineCache about compiling them as a pair, which would remove the need for
// this entirely.
static void pad_stage_io(std::string &hlsl, const std::string &struct_name) {
    const std::string header = "struct " + struct_name;
    const size_t struct_start = hlsl.find(header);
    if (struct_start == std::string::npos)
        return;

    const size_t body_start = hlsl.find('{', struct_start);
    const size_t body_end = hlsl.find("};", struct_start);
    if (body_start == std::string::npos || body_end == std::string::npos || body_end < body_start)
        return;

    const std::string body = hlsl.substr(body_start + 1, body_end - body_start - 1);

    // Split the body into lines and sort out which ones carry a TEXCOORD.
    std::vector<std::string> texcoord_lines;
    std::string position_line;
    std::vector<std::string> other_lines;
    std::vector<uint32_t> locations;

    size_t line_start = 0;
    while (line_start < body.size()) {
        size_t line_end = body.find('\n', line_start);
        if (line_end == std::string::npos)
            line_end = body.size();

        const std::string line = body.substr(line_start, line_end - line_start);
        line_start = line_end + 1;

        const size_t semantic = line.find(": TEXCOORD");
        if (semantic == std::string::npos) {
            if (line.find(": SV_Position") != std::string::npos)
                position_line = line;
            else if (line.find_first_not_of(" \t\r") != std::string::npos)
                other_lines.push_back(line);
            continue;
        }

        const size_t digits = semantic + 10;
        size_t digits_end = digits;
        while (digits_end < line.size() && line[digits_end] >= '0' && line[digits_end] <= '9')
            digits_end++;

        if (digits_end == digits)
            return;

        locations.push_back(static_cast<uint32_t>(std::stoul(line.substr(digits, digits_end - digits))));
        texcoord_lines.push_back(line);
    }

    // The recompiler puts varyings at locations 0 to 13 (name_map in spirv_recompiler.cpp).
    constexpr uint32_t VARYING_LOCATION_COUNT = 14;
    uint32_t location_count = VARYING_LOCATION_COUNT;
    for (const uint32_t location : locations)
        location_count = std::max(location_count, location + 1);

    // Guard against a pathological shader turning into a huge struct.
    constexpr uint32_t MAX_PADDED_LOCATION = 64;
    if (location_count > MAX_PADDED_LOCATION) {
        LOG_WARN_ONCE("D3D12: stage IO uses location {}, which is too sparse to pad", location_count - 1);
        return;
    }

    std::string padded;
    for (uint32_t location = 0; location < location_count; location++) {
        const auto it = std::find(locations.begin(), locations.end(), location);
        if (it != locations.end()) {
            padded += texcoord_lines[static_cast<size_t>(it - locations.begin())];
        } else {
            padded += fmt::format("    float4 spirv_cross_pad_{0} : TEXCOORD{0};", location);
        }
        padded += "\n";
    }

    // Keeps system-generated inputs such as SV_IsFrontFace off the register the vertex shader gives the position.
    padded += position_line.empty() ? "    float4 spirv_cross_pad_position : SV_Position;" : position_line;
    padded += "\n";

    for (const std::string &line : other_lines)
        padded += line + "\n";

    hlsl.replace(body_start + 1, body_end - body_start - 1, "\n" + padded);
}

std::string spirv_to_hlsl(const std::vector<uint32_t> &spirv, ShaderStage stage) {
    if (spirv.empty())
        return {};

    try {
        spirv_cross::CompilerHLSL compiler(spirv);

        spirv_cross::CompilerGLSL::Options common_options;
        // The depth range is already 0..1 in both APIs, so no clip-space fixup.
        common_options.vertex.fixup_clipspace = false;
        // Vulkan clip space has +Y pointing down, D3D12 has it up, and the
        // recompiler targets Vulkan. Without this the whole image is rendered
        // upside down. This is separate from the GXM viewport flip carried in
        // the render uniform block, which handles the guest's own convention.
        common_options.vertex.flip_vert_y = true;
        compiler.set_common_options(common_options);

        spirv_cross::CompilerHLSL::Options hlsl_options;
        hlsl_options.shader_model = HLSL_SHADER_MODEL;
        // Emit Texture2D/SamplerState pairs rather than legacy combined
        // samplers, which is what the descriptor tables are laid out for.
        hlsl_options.use_entry_point_name = false;
        // GXM programs rely on point size, which HLSL has no direct equivalent
        // for; letting SPIRV-Cross emit its compatibility path keeps the
        // translation from failing outright on point primitives.
        hlsl_options.point_size_compat = true;
        hlsl_options.point_coord_compat = true;
        compiler.set_hlsl_options(hlsl_options);

        apply_register_mapping(compiler, stage);

        std::string hlsl = compiler.compile();

        // Only the varyings are padded, and only on the side of each stage that
        // carries them.
        //
        // D3D12 matches the two stages by signature register, so a vertex shader
        // writing locations 1,4 and a pixel shader reading only 1 end up
        // disagreeing about which register SV_Position is; giving both the same
        // fixed layout fixes that.
        //
        // The other struct of each stage must be left alone. A vertex shader's
        // SPIRV_Cross_Input is the vertex attributes, and build_input_layout only
        // declares the attributes the vertex program actually has -- padding it
        // makes the shader demand a TEXCOORD the input layout does not provide,
        // and D3D12 rejects the whole pipeline. A fragment shader's
        // SPIRV_Cross_Output is SV_Target and has no varyings at all.
        if (stage == ShaderStage::Vertex)
            pad_stage_io(hlsl, "SPIRV_Cross_Output");
        else
            pad_stage_io(hlsl, "SPIRV_Cross_Input");

        return hlsl;
    } catch (const spirv_cross::CompilerError &error) {
        LOG_ERROR("D3D12: SPIR-V to HLSL translation failed: {}", error.what());
        return {};
    }
}

bool compile_hlsl_to_dxil(const std::string &hlsl, ShaderStage stage, const std::string &shader_name,
    std::vector<uint8_t> &bytecode, std::string &error) {
    bytecode.clear();
    error.clear();

    DxcLoader &loader = DxcLoader::instance();
    if (!loader.available()) {
        error = std::string(loader.unavailable_reason());
        return false;
    }

    if (hlsl.empty()) {
        error = "no HLSL source to compile";
        return false;
    }

    std::lock_guard<std::mutex> guard(dxc_mutex);

    const std::wstring wide_name = string_utils::utf_to_wide(shader_name);
    const wchar_t *profile = stage == ShaderStage::Vertex ? L"vs_6_0" : L"ps_6_0";

    std::vector<const wchar_t *> args = {
        wide_name.c_str(),
        L"-T",
        profile,
        L"-E",
        L"main",
        // Pack matrices the way the uniform blocks are laid out by the
        // recompiler, which follows the GLSL column-major convention.
        L"-Zpc",
        L"-O3",
        // The runtime builds its own root signature, so the shader must not
        // carry one of its own.
        L"-Qstrip_rootsignature",
    };

    const DxcBuffer source{
        .Ptr = hlsl.data(),
        .Size = hlsl.size(),
        // 0 means "no BOM, treat as UTF-8".
        .Encoding = DXC_CP_UTF8,
    };

    ComPtr<IDxcResult> result;
    const HRESULT compile_hr = loader.get_compiler()->Compile(&source, args.data(),
        static_cast<UINT32>(args.size()), nullptr, IID_PPV_ARGS(&result));

    if (FAILED(compile_hr)) {
        error = fmt::format("DXC could not be invoked: {}", hresult_to_string(compile_hr));
        return false;
    }

    ComPtr<IDxcBlobUtf8> errors;
    if (SUCCEEDED(result->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&errors), nullptr)))
        error = blob_to_string(errors.Get());

    HRESULT status = S_OK;
    result->GetStatus(&status);
    if (FAILED(status)) {
        if (error.empty())
            error = fmt::format("DXC failed: {}", hresult_to_string(status));
        return false;
    }

    ComPtr<IDxcBlob> object;
    if (FAILED(result->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&object), nullptr)) || !object) {
        error = "DXC reported success but produced no DXIL";
        return false;
    }

    const uint8_t *data = static_cast<const uint8_t *>(object->GetBufferPointer());
    bytecode.assign(data, data + object->GetBufferSize());

    // Warnings are worth surfacing even when the compile succeeded.
    if (!error.empty())
        LOG_WARN("D3D12: {} compiled with diagnostics: {}", shader_name, error);

    error.clear();
    return true;
}

CompiledShader compile_spirv(const std::vector<uint32_t> &spirv, ShaderStage stage, const std::string &shader_name) {
    CompiledShader compiled;

    compiled.hlsl = spirv_to_hlsl(spirv, stage);
    if (compiled.hlsl.empty())
        return compiled;

    std::string error;
    if (!compile_hlsl_to_dxil(compiled.hlsl, stage, shader_name, compiled.bytecode, error))
        LOG_ERROR("D3D12: failed to compile {}: {}", shader_name, error);

    return compiled;
}

// DXIL disk cache

static fs::path dxil_cache_path(const fs::path &shaders_path, const std::string &shader_name) {
    return shaders_path / fmt::format("{}-v{}.dxil", shader_name, DXIL_CACHE_VERSION);
}

bool load_cached_dxil(const fs::path &shaders_path, const std::string &shader_name, std::vector<uint8_t> &bytecode) {
    const fs::path path = dxil_cache_path(shaders_path, shader_name);

    fs::ifstream file(path, std::ios::in | std::ios::binary | std::ios::ate);
    if (!file.is_open())
        return false;

    const std::streamsize size = file.tellg();
    if (size <= 0)
        return false;

    file.seekg(0, std::ios::beg);
    bytecode.resize(static_cast<size_t>(size));
    if (!file.read(reinterpret_cast<char *>(bytecode.data()), size)) {
        bytecode.clear();
        return false;
    }

    return true;
}

void save_cached_dxil(const fs::path &shaders_path, const std::string &shader_name, const std::vector<uint8_t> &bytecode) {
    if (bytecode.empty())
        return;

    fs::create_directories(shaders_path);

    fs::ofstream file(dxil_cache_path(shaders_path, shader_name), std::ios::out | std::ios::binary);
    if (!file.is_open()) {
        LOG_WARN("D3D12: could not write the DXIL cache entry for {}", shader_name);
        return;
    }

    file.write(reinterpret_cast<const char *>(bytecode.data()), static_cast<std::streamsize>(bytecode.size()));
}

} // namespace renderer::d3d12
