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

#include <deque>
#include <vector>

namespace renderer::d3d12 {

struct DescriptorHandle {
    D3D12_CPU_DESCRIPTOR_HANDLE cpu{};
    D3D12_GPU_DESCRIPTOR_HANDLE gpu{};

    bool valid() const {
        return cpu.ptr != 0;
    }
};

// Thin wrapper over an ID3D12DescriptorHeap that knows its own increment size,
// so callers can address descriptors by index instead of doing pointer maths.
class DescriptorHeap {
public:
    bool create(ID3D12Device *device, D3D12_DESCRIPTOR_HEAP_TYPE type, uint32_t capacity, bool shader_visible, const wchar_t *debug_name = nullptr);
    void destroy();

    DescriptorHandle at(uint32_t index) const;

    ID3D12DescriptorHeap *handle() const {
        return heap.Get();
    }
    uint32_t capacity() const {
        return heap_capacity;
    }
    uint32_t increment() const {
        return heap_increment;
    }

protected:
    ComPtr<ID3D12DescriptorHeap> heap;
    D3D12_CPU_DESCRIPTOR_HANDLE cpu_start{};
    D3D12_GPU_DESCRIPTOR_HANDLE gpu_start{};
    uint32_t heap_increment = 0;
    uint32_t heap_capacity = 0;
    bool is_shader_visible = false;
};

// CPU-only heap with a free list. Used for RTVs, DSVs and for the staging SRVs
// that surfaces and textures hold on to for their whole lifetime.
class StagingDescriptorAllocator : public DescriptorHeap {
public:
    bool create(ID3D12Device *device, D3D12_DESCRIPTOR_HEAP_TYPE type, uint32_t capacity, const wchar_t *debug_name = nullptr);

    // Returns an invalid handle when the heap is exhausted.
    DescriptorHandle allocate();
    void free(DescriptorHandle handle);

private:
    std::vector<uint32_t> free_list;
    uint32_t next_free = 0;
};

// Shader-visible heap used as a per-frame linear allocator: descriptors are
// bump-allocated while recording and the whole thing is reset once the frame
// that used it has retired on the GPU.
class ShaderVisibleDescriptorRing : public DescriptorHeap {
public:
    bool create(ID3D12Device *device, D3D12_DESCRIPTOR_HEAP_TYPE type, uint32_t capacity, const wchar_t *debug_name = nullptr);

    // Reserve `count` contiguous descriptors. Returns an invalid handle when the
    // frame has run out; callers must treat that as a recoverable failure.
    DescriptorHandle allocate(uint32_t count);
    void reset();

    uint32_t used() const {
        return head;
    }

private:
    uint32_t head = 0;
};

// Persistently mapped UPLOAD-heap buffer handing out suballocations. Allocation
// is a bump pointer; regions are only reclaimed once `retire`/`reclaim` confirm
// the GPU is done with them, so a caller never overwrites in-flight data.
class UploadRingBuffer {
public:
    struct Allocation {
        uint8_t *cpu = nullptr;
        D3D12_GPU_VIRTUAL_ADDRESS gpu = 0;
        ID3D12Resource *resource = nullptr;
        uint64_t offset = 0;
        uint64_t size = 0;

        bool valid() const {
            return cpu != nullptr;
        }
    };

    bool create(ID3D12Device *device, uint64_t size, uint64_t alignment, const wchar_t *debug_name = nullptr);
    void destroy();

    // Bump-allocate `size` bytes. Returns an invalid allocation when the buffer
    // is full; call reclaim() with the last completed fence value first.
    Allocation allocate(uint64_t size);
    // Tag everything allocated since the previous retire with `fence_value`.
    void retire(uint64_t fence_value);
    // Release every region tagged with a fence value the GPU has passed.
    void reclaim(uint64_t completed_fence_value);

    uint64_t capacity() const {
        return buffer_size;
    }

private:
    struct InFlightRegion {
        uint64_t fence_value;
        uint64_t end_offset;
    };

    ComPtr<ID3D12Resource> buffer;
    uint8_t *mapped = nullptr;
    D3D12_GPU_VIRTUAL_ADDRESS gpu_base = 0;
    uint64_t buffer_size = 0;
    uint64_t buffer_alignment = 256;

    // head chases tail around the ring; head == tail means empty, never full.
    uint64_t head = 0;
    uint64_t tail = 0;
    uint64_t last_retired_head = 0;
    std::deque<InFlightRegion> in_flight;
};

// An ID3D12Resource plus the resource state the recorder believes it is in.
// D3D12 has no automatic layout transitions, so every texture and buffer that
// changes usage has to carry this.
struct Resource {
    ComPtr<ID3D12Resource> resource;
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;

    ID3D12Resource *get() const {
        return resource.Get();
    }
    explicit operator bool() const {
        return resource != nullptr;
    }
    void reset() {
        resource.Reset();
        state = D3D12_RESOURCE_STATE_COMMON;
    }
};

// Collects resource barriers so a run of transitions costs one API call.
class BarrierBatcher {
public:
    // No-op when the resource is already in `new_state`.
    void transition(Resource &res, D3D12_RESOURCE_STATES new_state);
    void transition(ID3D12Resource *res, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after);
    void uav(ID3D12Resource *res);
    void flush(ID3D12GraphicsCommandList *cmd_list);

private:
    std::vector<D3D12_RESOURCE_BARRIER> barriers;
};

} // namespace renderer::d3d12
