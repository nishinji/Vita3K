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

#include <renderer/gl/state.h>

#include <mem/functions.h>
#include <mem/util.h>
#include <shader/spirv_recompiler.h>
#include <util/align.h>
#include <util/log.h>

#include <bit>
#include <cassert>

namespace renderer::gl {

// The whole guest memory a game maps through sceGxmMapMemory has to fit in here. A PS Vita has
// 512 MiB of RAM in total, so no game can map more than this. The buffer is only created once a
// game actually maps something, and it replaces the two 256 MiB uniform stream ring buffers, which
// are never allocated when memory mapping is on.
static constexpr uint32_t GUEST_MEMORY_BUFFER_SIZE = static_cast<uint32_t>(MiB(512));

bool GLState::create_guest_memory_buffer() {
    if (!guest_memory_buffer.init(glGenBuffers, glDeleteBuffers)) {
        LOG_ERROR("Failed to create the guest memory buffer");
        return false;
    }

    // The guest writes to this mapping directly (its page table is redirected here) and reads back
    // whatever the shaders stored, so it must be readable, writable and coherent.
    constexpr GLbitfield map_flags = GL_MAP_READ_BIT | GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT;
    // This buffer is the guest's ram: the cpu touches it constantly and the gpu only reads from it
    // a few times a frame, so ask for it to live in client memory rather than across the bus. The
    // vulkan side asks for the same thing by preferring a host cached memory type.
    constexpr GLbitfield storage_flags = map_flags | GL_CLIENT_STORAGE_BIT;

    glBindBuffer(GL_SHADER_STORAGE_BUFFER, guest_memory_buffer[0]);
    glBufferStorage(GL_SHADER_STORAGE_BUFFER, GUEST_MEMORY_BUFFER_SIZE, nullptr, storage_flags);
    uint8_t *base = static_cast<uint8_t *>(glMapBufferRange(GL_SHADER_STORAGE_BUFFER, 0, GUEST_MEMORY_BUFFER_SIZE, map_flags));

    // The binding is context state and the buffer lives as long as the renderer does, so binding it
    // once here is enough for every program that reads guest memory.
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, shader::GUEST_MEMORY_SSBO_BINDING, guest_memory_buffer[0]);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);

    if (!base) {
        LOG_ERROR("Failed to persistently map the guest memory buffer");
        guest_memory_buffer.cleanup();
        return false;
    }

    // Drivers hand out page aligned mappings in practice, but nothing in the specification says so.
    const uint64_t base_value = std::bit_cast<uint64_t>(base);
    guest_memory_base_offset = static_cast<uint32_t>(align(base_value, KiB(4)) - base_value);
    guest_memory_base = base + guest_memory_base_offset;
    guest_memory_buffer_size = GUEST_MEMORY_BUFFER_SIZE;

    const uint32_t total_blocks = (GUEST_MEMORY_BUFFER_SIZE - guest_memory_base_offset) / static_cast<uint32_t>(KiB(4));
    guest_memory_free_blocks.emplace(0, total_blocks);

    return true;
}

int32_t GLState::allocate_guest_memory(uint32_t size) {
    const uint32_t wanted = static_cast<uint32_t>(align(size, KiB(4)) / KiB(4));

    for (auto it = guest_memory_free_blocks.begin(); it != guest_memory_free_blocks.end(); it++) {
        if (it->second < wanted)
            continue;

        const uint32_t block = it->first;
        const uint32_t count = it->second;
        guest_memory_free_blocks.erase(it);
        if (count > wanted)
            // What is left of the range starts further along, so its key changes too.
            guest_memory_free_blocks.emplace(block + wanted, count - wanted);

        return static_cast<int32_t>(block);
    }

    return -1;
}

void GLState::free_guest_memory(uint32_t block, uint32_t block_count) {
    auto next = guest_memory_free_blocks.lower_bound(block);

    // Coalesce with the range that follows, if it starts right where this one ends.
    if (next != guest_memory_free_blocks.end() && next->first == block + block_count) {
        block_count += next->second;
        next = guest_memory_free_blocks.erase(next);
    }

    // And with the one that precedes it, if it ends right where this one starts.
    if (next != guest_memory_free_blocks.begin()) {
        auto previous = std::prev(next);
        if (previous->first + previous->second == block) {
            previous->second += block_count;
            return;
        }
    }

    guest_memory_free_blocks.emplace(block, block_count);
}

bool GLState::map_memory(MemState &mem, Ptr<void> address, uint32_t size) {
    assert(features.enable_memory_mapping);
    // sceGxmMapMemory aligns both of these before sending the command
    assert((address.address() & (KiB(4) - 1)) == 0);
    assert((size & (KiB(4) - 1)) == 0);

    if (!guest_memory_base && !create_guest_memory_buffer())
        return false;

    // Reserve one extra page after the region, the way the vulkan page table method does: an access
    // that runs just past the end of a region then lands in padding rather than in the next
    // region's live data.
    const uint32_t block_count = static_cast<uint32_t>(align(size, KiB(4)) / KiB(4)) + 1;

    const int32_t block = allocate_guest_memory(block_count * static_cast<uint32_t>(KiB(4)));
    if (block < 0) {
        LOG_ERROR("Out of room in the guest memory buffer, cannot map {} bytes at {}", size, log_hex(address.address()));
        return false;
    }

    const uint32_t block_offset = static_cast<uint32_t>(block) * static_cast<uint32_t>(KiB(4));

    // From here on the guest page table points at our mapping instead of at the emulated RAM, so
    // everything the guest writes lands straight in the buffer the GPU reads.
    add_external_mapping(mem, address.address(), size, guest_memory_base + block_offset);
    mapped_memories[address.address()] = { guest_memory_base_offset + block_offset, size,
        static_cast<uint32_t>(block), block_count };

    return true;
}

void GLState::unmap_memory(MemState &mem, Ptr<void> address) {
    assert(features.enable_memory_mapping);

    auto ite = mapped_memories.find(address.address());
    if (ite == mapped_memories.end()) {
        LOG_ERROR("Could not find mapped memory to erase at {}", log_hex(address.address()));
        return;
    }

    // The guest may still be reading what the last draws wrote.
    glFinish();

    // This copies the content back into the emulated RAM and restores the page table.
    remove_external_mapping(mem, address.cast<uint8_t>().get(mem), ite->second.size);

    free_guest_memory(ite->second.block, ite->second.block_count);
    mapped_memories.erase(ite);
}

int64_t GLState::get_buffer_offset_of(const void *host_pointer) const {
    if (!guest_memory_base)
        return -1;

    const uint8_t *pointer = static_cast<const uint8_t *>(host_pointer);
    const uint8_t *mapping = guest_memory_base - guest_memory_base_offset;

    if (pointer < mapping || pointer >= mapping + guest_memory_buffer_size)
        return -1;

    return pointer - mapping;
}

uint32_t GLState::get_matching_offset(Address address) {
    // mapped_memories is ordered by std::greater, so this is the last region starting at or before
    // the address.
    auto mapped_memory = mapped_memories.lower_bound(address);
    if (mapped_memory == mapped_memories.end()
        || mapped_memory->first + mapped_memory->second.size < address) {
        LOG_ERROR_ONCE("Could not find the mapped buffer matching {}", log_hex(address));
        return 0;
    }

    return mapped_memory->second.offset + (address - mapped_memory->first);
}

} // namespace renderer::gl
