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

#include <renderer/profile.h>
#include <renderer/shaders.h>
#include <renderer/types.h>

#include <renderer/gl/state.h>
#include <renderer/gl/types.h>

#include <util/fs.h>
#include <util/log.h>

#include <shader/spirv_recompiler.h>

#include <iomanip>
#include <vector>

namespace renderer::gl {
static SharedGLObject compile_glsl(GLenum type, const std::string &source) {
    R_PROFILE(__func__);

    SharedGLObject shader = std::make_shared<GLObject>();
    if (!shader->init(glCreateShader(type), glDeleteShader)) {
        return SharedGLObject();
    }

    const GLchar *source_glchar = source.c_str();
    const GLint length = static_cast<GLint>(source.length());
    glShaderSource(shader->get(), 1, &source_glchar, &length);

    glCompileShader(shader->get());

    GLint is_compiled = GL_FALSE;
    glGetShaderiv(shader->get(), GL_COMPILE_STATUS, &is_compiled);
    if (!is_compiled) {
        GLint log_length = 0;
        glGetShaderiv(shader->get(), GL_INFO_LOG_LENGTH, &log_length);
        if (log_length > 1) {
            std::vector<GLchar> log(log_length);
            glGetShaderInfoLog(shader->get(), log_length, nullptr, log.data());
            LOG_ERROR("{}", log.data());
        }
        assert(is_compiled != GL_FALSE);
        return SharedGLObject();
    }

    return shader;
}

static SharedGLObject compile_spirv(GLenum type, const std::vector<std::uint32_t> &source) {
    R_PROFILE(__func__);

    SharedGLObject shader = std::make_shared<GLObject>();
    if (!shader->init(glCreateShader(type), glDeleteShader)) {
        return SharedGLObject();
    }

    const GLchar *source_glchar = reinterpret_cast<const GLchar *>(source.data());
    const GLint length = static_cast<GLint>(source.size() * sizeof(std::uint32_t));

    GLuint need_compile[1] = { shader->get() };
    const char *shader_entry = (type == GL_VERTEX_SHADER) ? "main_vs" : "main_fs";

    glShaderBinary(1, need_compile, GL_SHADER_BINARY_FORMAT_SPIR_V_ARB, source_glchar, length);
    glSpecializeShaderARB(need_compile[0], shader_entry, 0, nullptr, nullptr);

    GLint is_compiled = GL_FALSE;
    glGetShaderiv(shader->get(), GL_COMPILE_STATUS, &is_compiled);
    if (!is_compiled) {
        GLint log_length = 0;
        glGetShaderiv(shader->get(), GL_INFO_LOG_LENGTH, &log_length);
        if (log_length > 1) {
            std::vector<GLchar> log(log_length);
            glGetShaderInfoLog(shader->get(), log_length, nullptr, log.data());
            LOG_ERROR("{}", log.data());
        }
        assert(is_compiled != GL_FALSE);
        return SharedGLObject();
    }

    return shader;
}

static std::string convert_hash_to_hex(const Sha256Hash &hash) {
    std::string str;
    str.reserve(hash.size() * 2);

    for (size_t i = 0; i < hash.size(); ++i) {
        fmt::format_to(std::back_inserter(str), "{:02x}", hash[i]);
    }

    return str;
}

static SharedGLObject compile_program(ProgramCache &program_cache, const SharedGLObject &frag_shader, const SharedGLObject &vert_shader, const ProgramHashes &hashes) {
    SharedGLObject program = std::make_shared<GLObject>();
    if (!program->init(glCreateProgram(), glDeleteProgram)) {
        return SharedGLObject();
    }

    glAttachShader(program->get(), frag_shader->get());
    glAttachShader(program->get(), vert_shader->get());
    glLinkProgram(program->get());

    GLint is_linked = GL_FALSE;
    glGetProgramiv(program->get(), GL_LINK_STATUS, &is_linked);
    if (is_linked == GL_FALSE) {
        GLint log_length = 0;
        glGetProgramiv(program->get(), GL_INFO_LOG_LENGTH, &log_length);
        if (log_length > 1) {
            std::vector<GLchar> log(log_length);
            glGetProgramInfoLog(program->get(), log_length, nullptr, log.data());
            LOG_ERROR("{}\n", log.data());
        }
        assert(is_linked != GL_FALSE);
        return SharedGLObject();
    }

    glDetachShader(program->get(), frag_shader->get());
    glDetachShader(program->get(), vert_shader->get());

    program_cache.emplace(hashes, program);

    return program;
}

static SharedGLObject compile_shader(const fs::path &shader_cache_path, const std::string &shader_version, const std::string &hash_hex,
    const char *type_str, const GLenum type, ShaderCache &cache, const Sha256Hash &hash) {
    if (const auto cached = cache.find(hash); cached != cache.end())
        return cached->second;

    const auto shader_name = shader_cache_path / fmt::format("{}-{}.{}", shader_version, hash_hex, type_str);
    const std::string shader = pre_load_shader_glsl(shader_name);
    if (shader.empty()) {
        LOG_WARN("{} shader is empty or not found:\n{}", type_str, hash_hex);
        return SharedGLObject();
    }

    SharedGLObject obj = compile_glsl(type, shader);
    if (!obj) {
        LOG_CRITICAL("Error in compile {} shader:\n{}", type_str, hash_hex);
        return SharedGLObject();
    }

    cache.emplace(hash, obj);
    return obj;
}

static std::vector<ShadersHash>::iterator get_shaders_hash_index(std::vector<ShadersHash> &shaders_cache_hashs, const Sha256Hash &frag_hash, const Sha256Hash &vert_hash) {
    const auto shader_hash_index = std::ranges::find_if(shaders_cache_hashs, [&](const ShadersHash &h) {
        return (h.frag == frag_hash) && (h.vert == vert_hash);
    });

    return shader_hash_index;
}

void pre_compile_program(GLState &renderer, const ShadersHash &hash) {
    if (!fs::exists(renderer.shaders_path) || fs::is_empty(renderer.shaders_path))
        return;

    const ProgramHashes hashes(hash.frag, hash.vert);
    if (renderer.program_cache.contains(hashes))
        return;

    const auto frag_hash_hex = convert_hash_to_hex(hash.frag);
    const SharedGLObject frag_shader = compile_shader(renderer.shaders_path, renderer.shader_version,
        frag_hash_hex, "frag", GL_FRAGMENT_SHADER, renderer.fragment_shader_cache, hash.frag);
    if (!frag_shader) {
        return;
    }

    const auto vert_hash_hex = convert_hash_to_hex(hash.vert);
    const SharedGLObject vert_shader = compile_shader(renderer.shaders_path, renderer.shader_version,
        vert_hash_hex, "vert", GL_VERTEX_SHADER, renderer.vertex_shader_cache, hash.vert);
    if (!vert_shader) {
        return;
    }

    compile_program(renderer.program_cache, frag_shader, vert_shader, hashes);
    renderer.programs_count_pre_compiled++;
}

void pre_compile_programs(GLState &renderer, const std::vector<ShadersHash> &hashes, const std::function<void(int)> &on_progress) {
    if (!fs::exists(renderer.shaders_path) || fs::is_empty(renderer.shaders_path))
        return;

    int done = 0;
    std::vector<ShadersHash> missing;
    missing.reserve(hashes.size());

    for (const ShadersHash &hash : hashes) {
        if (renderer.render_abort.load(std::memory_order_relaxed))
            return;

        if (renderer.program_cache.contains(ProgramHashes(hash.frag, hash.vert))) {
            if (on_progress)
                on_progress(++done);
            continue;
        }
        missing.push_back(hash);
    }

    if (missing.empty())
        return;

    // Kick off all missing shader compilations before querying status so parallel shader compile threads saturate CPU cores
    struct PendingShader {
        Sha256Hash hash;
        SharedGLObject obj;
        GLenum type;
    };
    std::vector<PendingShader> pending_shaders;
    std::map<Sha256Hash, bool> queued_frag;
    std::map<Sha256Hash, bool> queued_vert;

    auto queue_shader = [&](const Sha256Hash &hash, GLenum type, const char *type_str, ShaderCache &cache, std::map<Sha256Hash, bool> &queued) {
        if (cache.contains(hash) || queued.contains(hash))
            return;
        queued.emplace(hash, true);

        const auto hash_hex = convert_hash_to_hex(hash);
        const auto shader_name = renderer.shaders_path / fmt::format("{}-{}.{}", renderer.shader_version, hash_hex, type_str);
        const std::string source = pre_load_shader_glsl(shader_name);
        if (source.empty()) {
            LOG_WARN("{} shader is empty or not found:\n{}", type_str, hash_hex);
            return;
        }

        SharedGLObject shader = std::make_shared<GLObject>();
        if (!shader->init(glCreateShader(type), glDeleteShader))
            return;

        const GLchar *src_ptr = source.c_str();
        const GLint src_len = static_cast<GLint>(source.length());
        glShaderSource(shader->get(), 1, &src_ptr, &src_len);
        glCompileShader(shader->get());
        pending_shaders.push_back({ hash, std::move(shader), type });
    };

    for (const ShadersHash &hash : missing) {
        if (renderer.render_abort.load(std::memory_order_relaxed))
            return;
        queue_shader(hash.frag, GL_FRAGMENT_SHADER, "frag", renderer.fragment_shader_cache, queued_frag);
        queue_shader(hash.vert, GL_VERTEX_SHADER, "vert", renderer.vertex_shader_cache, queued_vert);
    }

    for (auto &pending : pending_shaders) {
        GLint is_compiled = GL_FALSE;
        glGetShaderiv(pending.obj->get(), GL_COMPILE_STATUS, &is_compiled);
        if (!is_compiled) {
            GLint log_length = 0;
            glGetShaderiv(pending.obj->get(), GL_INFO_LOG_LENGTH, &log_length);
            if (log_length > 1) {
                std::vector<GLchar> log(log_length);
                glGetShaderInfoLog(pending.obj->get(), log_length, nullptr, log.data());
                LOG_ERROR("{}", log.data());
            }
            continue;
        }
        if (pending.type == GL_FRAGMENT_SHADER)
            renderer.fragment_shader_cache.emplace(pending.hash, std::move(pending.obj));
        else
            renderer.vertex_shader_cache.emplace(pending.hash, std::move(pending.obj));
    }

    // Kick off all program links before querying link status
    struct PendingProgram {
        ShadersHash hash;
        SharedGLObject program;
        SharedGLObject frag;
        SharedGLObject vert;
    };
    std::vector<PendingProgram> pending_programs;
    pending_programs.reserve(missing.size());

    for (const ShadersHash &hash : missing) {
        if (renderer.render_abort.load(std::memory_order_relaxed))
            return;

        const auto frag_it = renderer.fragment_shader_cache.find(hash.frag);
        const auto vert_it = renderer.vertex_shader_cache.find(hash.vert);
        if (frag_it == renderer.fragment_shader_cache.end() || vert_it == renderer.vertex_shader_cache.end()) {
            if (on_progress)
                on_progress(++done);
            continue;
        }

        SharedGLObject program = std::make_shared<GLObject>();
        if (!program->init(glCreateProgram(), glDeleteProgram)) {
            if (on_progress)
                on_progress(++done);
            continue;
        }

        glAttachShader(program->get(), frag_it->second->get());
        glAttachShader(program->get(), vert_it->second->get());
        glLinkProgram(program->get());
        pending_programs.push_back({ hash, std::move(program), frag_it->second, vert_it->second });
    }

    for (auto &pending : pending_programs) {
        if (renderer.render_abort.load(std::memory_order_relaxed))
            return;

        GLint is_linked = GL_FALSE;
        glGetProgramiv(pending.program->get(), GL_LINK_STATUS, &is_linked);
        if (is_linked != GL_FALSE) {
            glDetachShader(pending.program->get(), pending.frag->get());
            glDetachShader(pending.program->get(), pending.vert->get());
            renderer.program_cache.emplace(ProgramHashes(pending.hash.frag, pending.hash.vert), std::move(pending.program));
            renderer.programs_count_pre_compiled++;
        } else {
            GLint log_length = 0;
            glGetProgramiv(pending.program->get(), GL_INFO_LOG_LENGTH, &log_length);
            if (log_length > 1) {
                std::vector<GLchar> log(log_length);
                glGetProgramInfoLog(pending.program->get(), log_length, nullptr, log.data());
                LOG_ERROR("{}\n", log.data());
            }
        }
        if (on_progress)
            on_progress(++done);
    }
}

static SharedGLObject get_or_compile_shader(const SceGxmProgram *program, const FeatureState &features, const Sha256Hash &hash,
    ShaderCache &cache, const GLenum type, const shader::Hints &hints, bool shader_cache, bool spirv, bool maskupdate, const fs::path &shader_cache_path, const fs::path &shader_log_path, const std::string &shader_version, uint32_t &shaders_count_compiled) {
    const auto cached = cache.find(hash);
    if (cached == cache.end()) {
        SharedGLObject obj = nullptr;

        if (features.spirv_shader && spirv) {
            obj = compile_spirv(type, load_spirv_shader(*program, features, false, hints, maskupdate, shader_cache_path, shader_log_path, shader_version + "spv", shader_cache));
        } else {
            obj = compile_glsl(type, load_glsl_shader(*program, features, hints, maskupdate, shader_cache_path, shader_log_path, shader_version, shader_cache));
        }

        cache.emplace(hash, obj);

        shaders_count_compiled++;

        return obj;
    }

    return cached->second;
}

SharedGLObject compile_program(GLState &renderer, GLContext &context, const GxmRecordState &state, const FeatureState &features, const MemState &mem,
    bool shader_cache, bool spirv, bool maskupdate) {
    R_PROFILE(__func__);

    assert(state.fragment_program);
    assert(state.vertex_program);

    const SceGxmVertexProgram &vertex_program_gxm = *state.vertex_program.get(mem);
    const SceGxmFragmentProgram &fragment_program_gxm = *state.fragment_program.get(mem);

    const GLFragmentProgram &fragment_program = *reinterpret_cast<GLFragmentProgram *>(
        fragment_program_gxm.renderer_data.get());

    const GLVertexProgram &vertex_program = *reinterpret_cast<GLVertexProgram *>(
        vertex_program_gxm.renderer_data.get());

    const ProgramHashes hashes(fragment_program.hash, vertex_program.hash);

    const ProgramCache::const_iterator cached = renderer.program_cache.find(hashes);
    if (cached != renderer.program_cache.end()) {
        return cached->second;
    }

    context.shader_hints.color_format = state.color_surface.colorFormat;
    context.shader_hints.attributes = &vertex_program_gxm.attributes;

    FeatureState shader_features = features;
    shader_features.use_mask_bit = features.use_mask_bit && renderer.mask_used;
    shader_features.discard_disabled_side = renderer.side_disable_used;

    const SharedGLObject fragment_shader = get_or_compile_shader(fragment_program_gxm.program.get(mem), shader_features, fragment_program.hash, renderer.fragment_shader_cache,
        GL_FRAGMENT_SHADER, context.shader_hints, shader_cache, spirv, maskupdate, renderer.shaders_path, renderer.shaders_log_path, renderer.shader_version, renderer.shaders_count_compiled);

    if (!fragment_shader) {
        LOG_CRITICAL("Error in get/compile fragment vertex shader:\n{}", hex_string(fragment_program.hash));
        return SharedGLObject();
    }

    const SharedGLObject vertex_shader = get_or_compile_shader(vertex_program_gxm.program.get(mem), features, vertex_program.hash, renderer.vertex_shader_cache,
        GL_VERTEX_SHADER, context.shader_hints, shader_cache, spirv, maskupdate, renderer.shaders_path, renderer.shaders_log_path, renderer.shader_version, renderer.shaders_count_compiled);

    if (!vertex_shader) {
        LOG_CRITICAL("Error in get/compiled vertex shader:\n{}", hex_string(vertex_program.hash));
        return SharedGLObject();
    }

    SharedGLObject program = compile_program(renderer.program_cache, fragment_shader, vertex_shader, hashes);

    const auto shader_cache_hash_index = get_shaders_hash_index(renderer.shaders_cache_hashs, fragment_program.hash, vertex_program.hash);
    if (shader_cache_hash_index == renderer.shaders_cache_hashs.end()) {
        renderer.shaders_cache_hashs.push_back({ fragment_program.hash, vertex_program.hash });
        renderer.shaders_cache_hashs_dirty = true;
    }

    return program;
}
} // namespace renderer::gl
