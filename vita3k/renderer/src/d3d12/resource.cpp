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

#include <renderer/d3d12/resource.h>

#include <util/align.h>

#include <comdef.h>

namespace renderer::d3d12 {

std::string hresult_to_string(HRESULT hr) {
    // _com_error owns the message buffer, so it has to be copied out before the
    // temporary goes away.
    const _com_error err(hr);
    const TCHAR *msg = err.ErrorMessage();

#ifdef UNICODE
    std::string narrow;
    if (msg) {
        const int needed = WideCharToMultiByte(CP_UTF8, 0, msg, -1, nullptr, 0, nullptr, nullptr);
        if (needed > 1) {
            narrow.resize(static_cast<size_t>(needed) - 1);
            WideCharToMultiByte(CP_UTF8, 0, msg, -1, narrow.data(), needed, nullptr, nullptr);
        }
    }
#else
    const std::string narrow = msg ? msg : "";
#endif

    return fmt::format("0x{:08X} ({})", static_cast<uint32_t>(hr), narrow);
}

// DescriptorHeap

bool DescriptorHeap::create(ID3D12Device *device, D3D12_DESCRIPTOR_HEAP_TYPE type, uint32_t capacity, bool shader_visible, const wchar_t *debug_name) {
    // Only CBV_SRV_UAV and SAMPLER heaps may be shader visible.
    if (shader_visible && type != D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV && type != D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER) {
        LOG_ERROR("D3D12: descriptor heap type {} cannot be shader visible", static_cast<int>(type));
        return false;
    }

    const D3D12_DESCRIPTOR_HEAP_DESC desc{
        .Type = type,
        .NumDescriptors = capacity,
        .Flags = shader_visible ? D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE : D3D12_DESCRIPTOR_HEAP_FLAG_NONE,
        .NodeMask = 0,
    };

    if (!dx_check(device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&heap)), "creating a descriptor heap"))
        return false;

    if (debug_name)
        heap->SetName(debug_name);

    heap_increment = device->GetDescriptorHandleIncrementSize(type);
    heap_capacity = capacity;
    is_shader_visible = shader_visible;
    cpu_start = heap->GetCPUDescriptorHandleForHeapStart();
    gpu_start = shader_visible ? heap->GetGPUDescriptorHandleForHeapStart() : D3D12_GPU_DESCRIPTOR_HANDLE{};

    return true;
}

void DescriptorHeap::destroy() {
    heap.Reset();
    cpu_start = {};
    gpu_start = {};
    heap_increment = 0;
    heap_capacity = 0;
    is_shader_visible = false;
}

DescriptorHandle DescriptorHeap::at(uint32_t index) const {
    if (index >= heap_capacity)
        return {};

    DescriptorHandle handle;
    handle.cpu.ptr = cpu_start.ptr + static_cast<SIZE_T>(index) * heap_increment;
    if (is_shader_visible)
        handle.gpu.ptr = gpu_start.ptr + static_cast<UINT64>(index) * heap_increment;

    return handle;
}

// StagingDescriptorAllocator

bool StagingDescriptorAllocator::create(ID3D12Device *device, D3D12_DESCRIPTOR_HEAP_TYPE type, uint32_t capacity, const wchar_t *debug_name) {
    if (!DescriptorHeap::create(device, type, capacity, false, debug_name))
        return false;

    free_list.clear();
    next_free = 0;
    return true;
}

DescriptorHandle StagingDescriptorAllocator::allocate() {
    if (!free_list.empty()) {
        const uint32_t index = free_list.back();
        free_list.pop_back();
        return at(index);
    }

    if (next_free >= heap_capacity) {
        LOG_ERROR("D3D12: staging descriptor heap exhausted ({} descriptors)", heap_capacity);
        return {};
    }

    return at(next_free++);
}

void StagingDescriptorAllocator::free(DescriptorHandle handle) {
    if (!handle.valid() || heap_increment == 0)
        return;

    if (handle.cpu.ptr < cpu_start.ptr)
        return;

    const uint32_t index = static_cast<uint32_t>((handle.cpu.ptr - cpu_start.ptr) / heap_increment);
    if (index < heap_capacity)
        free_list.push_back(index);
}

// ShaderVisibleDescriptorRing

bool ShaderVisibleDescriptorRing::create(ID3D12Device *device, D3D12_DESCRIPTOR_HEAP_TYPE type, uint32_t capacity, const wchar_t *debug_name) {
    if (!DescriptorHeap::create(device, type, capacity, true, debug_name))
        return false;

    head = 0;
    return true;
}

DescriptorHandle ShaderVisibleDescriptorRing::allocate(uint32_t count) {
    if (count == 0 || head + count > heap_capacity)
        return {};

    const DescriptorHandle handle = at(head);
    head += count;
    return handle;
}

void ShaderVisibleDescriptorRing::reset() {
    head = 0;
}

// UploadRingBuffer

bool UploadRingBuffer::create(ID3D12Device *device, uint64_t size, uint64_t alignment, const wchar_t *debug_name) {
    buffer_alignment = alignment == 0 ? 1 : alignment;
    buffer_size = align(size, buffer_alignment);

    const D3D12_HEAP_PROPERTIES heap_props{
        .Type = D3D12_HEAP_TYPE_UPLOAD,
        .CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN,
        .MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN,
        .CreationNodeMask = 1,
        .VisibleNodeMask = 1,
    };

    const D3D12_RESOURCE_DESC desc{
        .Dimension = D3D12_RESOURCE_DIMENSION_BUFFER,
        .Alignment = 0,
        .Width = buffer_size,
        .Height = 1,
        .DepthOrArraySize = 1,
        .MipLevels = 1,
        .Format = DXGI_FORMAT_UNKNOWN,
        .SampleDesc = { .Count = 1, .Quality = 0 },
        .Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
        .Flags = D3D12_RESOURCE_FLAG_NONE,
    };

    if (!dx_check(device->CreateCommittedResource(&heap_props, D3D12_HEAP_FLAG_NONE, &desc,
                      D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&buffer)),
            "creating an upload ring buffer"))
        return false;

    if (debug_name)
        buffer->SetName(debug_name);

    // Upload heaps stay mapped for their whole life. The empty read range tells
    // the runtime the CPU never reads back through this pointer.
    const D3D12_RANGE no_read{ 0, 0 };
    void *data = nullptr;
    if (!dx_check(buffer->Map(0, &no_read, &data), "mapping an upload ring buffer")) {
        buffer.Reset();
        return false;
    }

    mapped = static_cast<uint8_t *>(data);
    gpu_base = buffer->GetGPUVirtualAddress();
    head = tail = last_retired_head = 0;
    in_flight.clear();

    return true;
}

void UploadRingBuffer::destroy() {
    if (buffer && mapped) {
        const D3D12_RANGE no_write{ 0, 0 };
        buffer->Unmap(0, &no_write);
    }

    buffer.Reset();
    mapped = nullptr;
    gpu_base = 0;
    head = tail = last_retired_head = 0;
    in_flight.clear();
}

UploadRingBuffer::Allocation UploadRingBuffer::allocate(uint64_t size) {
    if (!mapped || size == 0)
        return {};

    size = align(size, buffer_alignment);
    if (size > buffer_size)
        return {};

    // Offsets are monotonic, so the physical position is just a modulo. An
    // allocation may not straddle the wrap point, so pad up to it when needed.
    uint64_t start = head;
    uint64_t physical = start % buffer_size;
    if (physical + size > buffer_size) {
        start += buffer_size - physical;
        physical = 0;
    }

    // Refuse rather than overwrite bytes the GPU has not finished reading.
    if (start + size - tail > buffer_size)
        return {};

    head = start + size;

    Allocation alloc;
    alloc.cpu = mapped + physical;
    alloc.gpu = gpu_base + physical;
    alloc.resource = buffer.Get();
    alloc.offset = physical;
    alloc.size = size;

    return alloc;
}

void UploadRingBuffer::retire(uint64_t fence_value) {
    if (head == last_retired_head)
        return;

    in_flight.push_back({ fence_value, head });
    last_retired_head = head;
}

void UploadRingBuffer::reclaim(uint64_t completed_fence_value) {
    while (!in_flight.empty() && in_flight.front().fence_value <= completed_fence_value) {
        tail = in_flight.front().end_offset;
        in_flight.pop_front();
    }
}

// BarrierBatcher

void BarrierBatcher::transition(Resource &res, D3D12_RESOURCE_STATES new_state) {
    if (!res || res.state == new_state)
        return;

    transition(res.get(), res.state, new_state);
    res.state = new_state;
}

void BarrierBatcher::transition(ID3D12Resource *res, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
    if (!res || before == after)
        return;

    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barrier.Transition.pResource = res;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    barriers.push_back(barrier);
}

void BarrierBatcher::uav(ID3D12Resource *res) {
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barrier.UAV.pResource = res;
    barriers.push_back(barrier);
}

void BarrierBatcher::flush(ID3D12GraphicsCommandList *cmd_list) {
    if (barriers.empty())
        return;

    cmd_list->ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());
    barriers.clear();
}

} // namespace renderer::d3d12
