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

#include <renderer/gl/overlay_renderer.h>
#include <renderer/gl/screen_render.h>
#include <renderer/gl/surface_cache.h>
#include <renderer/state.h>
#include <renderer/types.h>

#include "types.h"

#include <chrono>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <string_view>
#include <thread>
#include <vector>

namespace renderer::gl {

// A range of guest memory that sceGxmMapMemory handed us, living inside the guest memory buffer.
struct GLMappedMemory {
    // Byte offset of the region inside the guest memory buffer.
    uint32_t offset;
    uint32_t size;
    // First 4 KiB block and how many were reserved for it, padding included.
    uint32_t block;
    uint32_t block_count;
};

struct GLState : public renderer::State {
    ShaderCache fragment_shader_cache;
    ShaderCache vertex_shader_cache;
    ProgramCache program_cache;

    GLTextureCache texture_cache;
    GLSurfaceCache surface_cache;

    ScreenRenderer screen_renderer;
    OverlayRenderer overlay_renderer;

    bool context_is_current = false;

    // Does the driver expose GL_ARB_parallel_shader_compile (or its KHR variant)?
    bool support_parallel_shader_compile = false;
    // What the user asked for. Kept separate because set_async_compilation may be
    // called before the extension has been probed.
    bool async_compilation_requested = false;
    // Translate shaders on worker threads and drop draws whose program is not ready.
    // Does not need any extension.
    bool use_async_compilation = false;
    // Skip the GL status queries that would block the render thread. Only possible
    // with GL_ARB_parallel_shader_compile.
    bool defer_gl_status_checks = false;

    // Programs whose linking is in flight, polled from the render thread.
    std::vector<PendingProgram> pending_programs;

    // gxp -> GLSL/SPIR-V translations in flight. Guarded by shader_translations_mutex.
    // Entries are shared so that a worker can safely finish writing to one that has
    // already been dropped from the map.
    std::mutex shader_translations_mutex;
    std::map<Sha256Hash, std::shared_ptr<ShaderTranslation>> shader_translations;

    std::vector<std::thread> shader_translate_threads;
    std::mutex shader_translate_queue_mutex;
    std::condition_variable shader_translate_queue_cond;
    std::deque<ShaderTranslateRequest *> shader_translate_queue;
    bool shader_translate_abort = false;

    // Can this driver back guest memory with a persistently mapped buffer? Decided once, at
    // creation. features.enable_memory_mapping follows it unless the user turned mapping off.
    bool support_memory_mapping = false;

    // All the guest memory GXM mapped lives in this single buffer, which is bound as an SSBO so
    // that the shaders can read any of it. One buffer rather than one per region keeps the whole
    // thing addressable with a plain 32-bit offset and costs a single binding point.
    GLObjectArray<1> guest_memory_buffer;
    // Persistent mapping of guest_memory_buffer, rounded up to the first 4 KiB boundary: the guest
    // page table works in 4 KiB pages, so every sub-allocation has to be 4 KiB aligned.
    uint8_t *guest_memory_base = nullptr;
    // Byte offset of guest_memory_base inside guest_memory_buffer.
    uint32_t guest_memory_base_offset = 0;
    // Size the buffer was created with, so that a host pointer can be tested against its mapping.
    uint32_t guest_memory_buffer_size = 0;
    // Free ranges of the buffer, as (first 4 KiB block, block count). Kept coalesced.
    std::map<uint32_t, uint32_t> guest_memory_free_blocks;
    // Mapped regions keyed by guest address. std::greater so that lower_bound() lands on the
    // region containing an address rather than on the one after it.
    std::map<Address, GLMappedMemory, std::greater<Address>> mapped_memories;

    bool init() override;
    void cleanup() override;
    void late_init(const Config &cfg, const std::string_view game_id, MemState &mem) override;

    TextureCache *get_texture_cache() override {
        return &texture_cache;
    }

    bool map_memory(MemState &mem, Ptr<void> address, uint32_t size) override;
    void unmap_memory(MemState &mem, Ptr<void> address) override;
    // Byte offset of a guest address inside guest_memory_buffer. Returns 0 for an address that is
    // not mapped: that reads the wrong data, but it stays in bounds.
    uint32_t get_matching_offset(Address address);
    // Byte offset of a host pointer inside guest_memory_buffer, or -1 if it points elsewhere.
    // A pointer that lands inside must never be handed to GL as client memory: it is a mapping of
    // a buffer the driver already owns, and reading it as if it were ordinary memory is not the
    // same thing as reading the buffer.
    int64_t get_buffer_offset_of(const void *host_pointer) const;

    void render_frame(DisplayState &display, const GxmState &gxm, MemState &mem) override;
    void swap_window() override;
    bool set_current() override;
    void done_current() override;
    std::vector<uint32_t> dump_frame(DisplayState &display, uint32_t &width, uint32_t &height) override;

    uint32_t get_features_mask() override;
    int get_supported_filters() override;
    void set_screen_filter(const std::string_view &filter) override;
    int get_max_anisotropic_filtering() override;
    void set_anisotropic_filtering(int anisotropic_filtering) override;
    int get_max_2d_texture_width() override;
    void set_async_compilation(bool enable) override;

    // Move every pending program that the driver has finished linking to its final state.
    // Must be called from the render thread. Cheap when nothing is pending.
    void poll_pending_programs();

    void start_shader_translate_threads();
    void stop_shader_translate_threads();
    // Entry point of each shader translation worker.
    void shader_translate_thread();
    // Hand a request over to the workers. Takes ownership of the request.
    void enqueue_shader_translation(ShaderTranslateRequest *request);

    std::string_view get_gpu_name() override;

    void precompile_shader(const ShadersHash &hash) override;
    void preclose_action() override;

private:
    // Create and persistently map guest_memory_buffer. Called on the first mapped region.
    bool create_guest_memory_buffer();
    // First-fit allocation of `size` bytes, 4 KiB aligned. Returns the block index, or -1.
    int32_t allocate_guest_memory(uint32_t size);
    void free_guest_memory(uint32_t block, uint32_t block_count);
};

} // namespace renderer::gl
