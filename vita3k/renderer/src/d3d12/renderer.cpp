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

#include <renderer/d3d12/functions.h>
#include <renderer/d3d12/state.h>
#include <renderer/functions.h>
#include <renderer/types.h>

#include <config/state.h>
#include <config/version.h>
#include <display/state.h>
#include <shader/spirv_recompiler.h>
#include <util/log.h>
#include <util/string_utils.h>

#include <overlay/display_manager.h>

#include <algorithm>
#include <chrono>

namespace renderer::d3d12 {

// Sizes of the per-frame pools. These are starting points sized off what the
// vulkan backend allocates; the draw path will tune them once it is landed.
// The shader-visible view heap can hold up to a million descriptors, so this
// is sized generously. The sampler heap below is capped at 2048 by D3D12.
constexpr uint32_t FRAME_VIEW_DESCRIPTOR_COUNT = 65536;
constexpr uint32_t FRAME_SAMPLER_DESCRIPTOR_COUNT = 2048;
constexpr uint64_t FRAME_UPLOAD_BUFFER_SIZE = 64 * 1024 * 1024;

constexpr uint32_t RTV_HEAP_SIZE = 1024;
constexpr uint32_t DSV_HEAP_SIZE = 512;
constexpr uint32_t SRV_STAGING_HEAP_SIZE = 16384;

bool create(std::unique_ptr<renderer::State> &state, const Config &config) {
    auto &dx_state = dynamic_cast<DXState &>(*state);

    return dx_state.create(state, config);
}

DXState::DXState(int gpu_idx)
    : gpu_idx(gpu_idx)
    , texture_cache(*this)
    , pipeline_cache(*this)
    , surface_cache(*this)
    , screen_renderer(*this) {
}

DXState::~DXState() {
    // Destroying a joinable std::thread terminates the process, so this has to
    // run even on the paths that never reach cleanup(). It is idempotent.
    stop_notification_thread();

    if (fence_event) {
        CloseHandle(fence_event);
        fence_event = nullptr;
    }
}

bool DXState::init() {
    shader_version = fmt::format("v{}", shader::CURRENT_VERSION);
    return true;
}

// Adapter selection

std::vector<ComPtr<IDXGIAdapter1>> DXState::enumerate_adapters() {
    std::vector<ComPtr<IDXGIAdapter1>> adapters;

    ComPtr<IDXGIFactory6> factory6;
    if (SUCCEEDED(factory.As(&factory6))) {
        // Ask DXGI to order by raw performance so index 0 is the discrete GPU.
        ComPtr<IDXGIAdapter1> candidate;
        for (UINT i = 0;
            factory6->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&candidate)) != DXGI_ERROR_NOT_FOUND;
            i++) {
            adapters.push_back(candidate);
            candidate.Reset();
        }
    } else {
        ComPtr<IDXGIAdapter1> candidate;
        for (UINT i = 0; factory->EnumAdapters1(i, &candidate) != DXGI_ERROR_NOT_FOUND; i++) {
            adapters.push_back(candidate);
            candidate.Reset();
        }
    }

    // Drop the software adapter, which would otherwise run the whole emulator on
    // WARP without saying so. Support is not probed here: a throwaway
    // D3D12CreateDevice per adapter costs a full driver load on each one, and
    // create_device already falls through to the next candidate when one fails.
    std::erase_if(adapters, [](const ComPtr<IDXGIAdapter1> &candidate) {
        DXGI_ADAPTER_DESC1 desc{};
        if (FAILED(candidate->GetDesc1(&desc)))
            return true;

        return (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
    });

    return adapters;
}

bool DXState::create_device(const Config &config) {
    UINT factory_flags = 0;

    if (config.validation_layer) {
        ComPtr<ID3D12Debug> debug_controller;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug_controller)))) {
            debug_controller->EnableDebugLayer();
            factory_flags |= DXGI_CREATE_FACTORY_DEBUG;
            LOG_INFO("Enabling the D3D12 debug layer (has a performance impact but allows better error messages)");
        } else {
            LOG_INFO("D3D12 debug layer is not available, install the Graphics Tools optional feature to enable it");
        }
    }

    if (!dx_check(CreateDXGIFactory2(factory_flags, IID_PPV_ARGS(&factory)), "creating the DXGI factory"))
        return false;

    const std::vector<ComPtr<IDXGIAdapter1>> adapters = enumerate_adapters();
    if (adapters.empty()) {
        LOG_ERROR("D3D12: no hardware adapter was found");
        return false;
    }

    for (size_t i = 0; i < adapters.size(); i++) {
        DXGI_ADAPTER_DESC1 desc{};
        adapters[i]->GetDesc1(&desc);
        LOG_INFO("D3D12 adapter {}: {}", i + 1, string_utils::wide_to_utf(desc.Description));
    }

    // gpu_idx is 1-based in the config, with 0 meaning "let the driver decide".
    size_t preferred = 0;
    if (gpu_idx > 0) {
        if (static_cast<size_t>(gpu_idx) <= adapters.size()) {
            preferred = static_cast<size_t>(gpu_idx) - 1;
        } else {
            LOG_WARN("D3D12: requested GPU index {} is out of range ({} adapters), using the first one",
                gpu_idx, adapters.size());
        }
    }

    // Try the preferred adapter first, then the rest as fallbacks.
    std::vector<size_t> order;
    order.reserve(adapters.size());
    order.push_back(preferred);
    for (size_t i = 0; i < adapters.size(); i++) {
        if (i != preferred)
            order.push_back(i);
    }

    // Take the highest feature level the adapter actually supports; 12_0 buys
    // resource binding tier 2+ which the descriptor strategy relies on.
    static constexpr D3D_FEATURE_LEVEL levels[] = {
        D3D_FEATURE_LEVEL_12_1,
        D3D_FEATURE_LEVEL_12_0,
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
    };

    for (const size_t index : order) {
        for (const D3D_FEATURE_LEVEL level : levels) {
            if (SUCCEEDED(D3D12CreateDevice(adapters[index].Get(), level, IID_PPV_ARGS(&device)))) {
                adapter = adapters[index];
                feature_level = level;
                break;
            }
        }

        if (device)
            break;

        DXGI_ADAPTER_DESC1 desc{};
        adapters[index]->GetDesc1(&desc);
        LOG_WARN("D3D12: could not create a device on {}, trying the next adapter",
            string_utils::wide_to_utf(desc.Description));
    }

    if (!device) {
        LOG_ERROR("D3D12: no adapter could create a device at feature level 11_0");
        return false;
    }

    DXGI_ADAPTER_DESC1 desc{};
    adapter->GetDesc1(&desc);
    gpu_name = string_utils::wide_to_utf(desc.Description);
    vendor_id = desc.VendorId;
    device_id = desc.DeviceId;
    dedicated_video_memory = desc.DedicatedVideoMemory;

    LOG_INFO("D3D12: using adapter {} ({} MiB dedicated video memory)", gpu_name, dedicated_video_memory / (1024 * 1024));
    LOG_INFO("D3D12: created a device at feature level {}.{}",
        (static_cast<uint32_t>(feature_level) >> 12) & 0xf,
        (static_cast<uint32_t>(feature_level) >> 8) & 0xf);

    if (config.validation_layer) {
        if (SUCCEEDED(device.As(&info_queue))) {
            // Deliberately no SetBreakOnSeverity: breaking stops the debugger on
            // a stack that does not include the message text, which is the one
            // thing needed to act on it. drain_debug_messages puts the text in
            // the emulator log instead, where it travels with a bug report.
            info_queue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_CORRUPTION, FALSE);
            info_queue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR, FALSE);
            info_queue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_WARNING, FALSE);

            // A miss in the pipeline library is the normal path for every
            // pipeline that has not been compiled before, so it is not worth
            // reporting once per pipeline.
            D3D12_MESSAGE_ID denied[] = {
                D3D12_MESSAGE_ID_LOADPIPELINE_NAMENOTFOUND,
                D3D12_MESSAGE_ID_STOREPIPELINE_DUPLICATENAME,
            };

            D3D12_INFO_QUEUE_FILTER filter{};
            filter.DenyList.NumIDs = static_cast<UINT>(std::size(denied));
            filter.DenyList.pIDList = denied;
            info_queue->AddStorageFilterEntries(&filter);
        }
    }

    // Tearing (needed for uncapped framerate on borderless windows) is a
    // factory-level capability and has to be requested at swapchain creation.
    ComPtr<IDXGIFactory5> factory5;
    if (SUCCEEDED(factory.As(&factory5))) {
        BOOL tearing = FALSE;
        if (SUCCEEDED(factory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tearing, sizeof(tearing))))
            allow_tearing = tearing == TRUE;
    }

    return true;
}

bool DXState::create_queues() {
    const D3D12_COMMAND_QUEUE_DESC direct_desc{
        .Type = D3D12_COMMAND_LIST_TYPE_DIRECT,
        .Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL,
        .Flags = D3D12_COMMAND_QUEUE_FLAG_NONE,
        .NodeMask = 0,
    };

    if (!dx_check(device->CreateCommandQueue(&direct_desc, IID_PPV_ARGS(&direct_queue)), "creating the direct command queue"))
        return false;

    direct_queue->SetName(L"Vita3K direct queue");

    const D3D12_COMMAND_QUEUE_DESC copy_desc{
        .Type = D3D12_COMMAND_LIST_TYPE_COPY,
        .Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL,
        .Flags = D3D12_COMMAND_QUEUE_FLAG_NONE,
        .NodeMask = 0,
    };

    if (!dx_check(device->CreateCommandQueue(&copy_desc, IID_PPV_ARGS(&copy_queue)), "creating the copy command queue"))
        return false;

    copy_queue->SetName(L"Vita3K copy queue");

    if (!dx_check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&frame_fence)), "creating the frame fence"))
        return false;

    fence_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!fence_event) {
        LOG_ERROR("D3D12: failed to create the fence event");
        return false;
    }

    return true;
}

bool DXState::create_frame_objects() {
    for (uint32_t i = 0; i < MAX_FRAMES_RENDERING; i++) {
        FrameObject &frame_object = frames[i];

        if (!dx_check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&frame_object.render_allocator)),
                "creating a render command allocator"))
            return false;

        if (!dx_check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&frame_object.prerender_allocator)),
                "creating a prerender command allocator"))
            return false;

        if (!frame_object.view_descriptors.create(device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
                FRAME_VIEW_DESCRIPTOR_COUNT, L"Vita3K frame view descriptors"))
            return false;

        if (!frame_object.sampler_descriptors.create(device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER,
                FRAME_SAMPLER_DESCRIPTOR_COUNT, L"Vita3K frame sampler descriptors"))
            return false;

        // 512 is D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT, the strictest of the
        // uses this buffer is put to, and a multiple of the 256-byte constant
        // buffer alignment, so every suballocation is valid for any of them.
        if (!frame_object.upload_buffer.create(device.Get(), FRAME_UPLOAD_BUFFER_SIZE,
                D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT, L"Vita3K frame upload buffer"))
            return false;

        // Created in the closed state so the first Reset in render_frame is
        // symmetric with every later one.
        if (!dx_check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, frame_object.render_allocator.Get(),
                          nullptr, IID_PPV_ARGS(&frame_object.render_cmd_list)),
                "creating a frame command list"))
            return false;

        if (!dx_check(frame_object.render_cmd_list->Close(), "closing a freshly created command list"))
            return false;

        frame_object.fence_value = 0;
    }

    return true;
}

bool DXState::create_default_resources() {
    // A 1x1 opaque black texture stands in for slots a shader samples but the
    // game never bound; a null descriptor in a table is undefined behaviour.
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
        .Width = 1,
        .Height = 1,
        .DepthOrArraySize = 1,
        .MipLevels = 1,
        .Format = DXGI_FORMAT_R8G8B8A8_UNORM,
        .SampleDesc = { .Count = 1, .Quality = 0 },
        .Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN,
        .Flags = D3D12_RESOURCE_FLAG_NONE,
    };

    if (!dx_check(device->CreateCommittedResource(&heap_props, D3D12_HEAP_FLAG_NONE, &desc,
                      D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                      nullptr, IID_PPV_ARGS(&default_image.resource)),
            "creating the default texture"))
        return false;

    default_image.state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    default_image.resource->SetName(L"Vita3K default texture");

    default_srv = srv_staging_heap.allocate();
    if (!default_srv.valid())
        return false;

    const D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc{
        .Format = DXGI_FORMAT_R8G8B8A8_UNORM,
        .ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D,
        .Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING,
        .Texture2D = { .MostDetailedMip = 0, .MipLevels = 1, .PlaneSlice = 0, .ResourceMinLODClamp = 0.0f },
    };
    device->CreateShaderResourceView(default_image.get(), &srv_desc, default_srv.cpu);

    if (!default_sampler_heap.create(device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, 1, L"Vita3K default sampler"))
        return false;

    default_sampler = default_sampler_heap.allocate();
    if (!default_sampler.valid())
        return false;

    const D3D12_SAMPLER_DESC sampler_desc{
        .Filter = D3D12_FILTER_MIN_MAG_MIP_POINT,
        .AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
        .AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
        .AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
        .MipLODBias = 0.0f,
        .MaxAnisotropy = 1,
        .ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER,
        .BorderColor = { 0.0f, 0.0f, 0.0f, 0.0f },
        .MinLOD = 0.0f,
        .MaxLOD = D3D12_FLOAT32_MAX,
    };
    device->CreateSampler(&sampler_desc, default_sampler.cpu);

    return true;
}

bool DXState::create(std::unique_ptr<renderer::State> &state, const Config &config) {
    if (!create_device(config))
        return false;

    if (!create_queues())
        return false;

    if (!rtv_heap.create(device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_RTV, RTV_HEAP_SIZE, L"Vita3K RTV heap"))
        return false;

    if (!dsv_heap.create(device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_DSV, DSV_HEAP_SIZE, L"Vita3K DSV heap"))
        return false;

    if (!srv_staging_heap.create(device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, SRV_STAGING_HEAP_SIZE, L"Vita3K SRV staging heap"))
        return false;

    if (!create_frame_objects())
        return false;

    if (!create_default_resources())
        return false;

    if (!start_notification_thread())
        return false;

    // The swapchain needs a window; on Windows the frame host always hands out a
    // Win32 handle, but a headless host would legitimately have none.
    // frame() is the per-frame object accessor, which hides the inherited
    // FrameHost pointer of the same name; reach past it explicitly.
    FrameHost *frame_host = this->renderer::State::frame;
    const DisplayHandle display_handle = frame_host->handle();
    const auto *win32_handle = std::get_if<Win32DisplayHandle>(&display_handle);
    if (!win32_handle || !win32_handle->hwnd) {
        LOG_ERROR("D3D12: the frame host did not provide a Win32 window handle");
        return false;
    }

    hwnd = static_cast<HWND>(win32_handle->hwnd);

    const uint32_t width = static_cast<uint32_t>(std::max(frame_host->drawable_width(), 1));
    const uint32_t height = static_cast<uint32_t>(std::max(frame_host->drawable_height(), 1));
    if (!create_swapchain(width, height))
        return false;

    // The root signature only needs the device; the pipeline library part of it
    // is re-read in late_init once shaders_path points at the running title.
    if (!pipeline_cache.init())
        return false;

    if (!screen_renderer.create())
        return false;

    // A failed overlay is not fatal: the game still renders, only the
    // notifications and the loading screen go missing, so it is logged and
    // stepped over rather than failing initialisation.
    if (!overlay_renderer.init(*this))
        LOG_WARN("D3D12: the overlay renderer could not be created; overlays will not be drawn");

    return true;
}

void DXState::late_init(const Config &cfg, const std::string_view game_id, MemState &mem) {
    this->mem = &mem;

    // Memory mapping relies on importing host memory as GPU-visible buffers.
    // The D3D12 equivalent is not wired up yet, so the backend runs with the
    // copy-based path, exactly as the vulkan backend does when mapping is off.
    mapping_method = MappingMethod::Disabled;
    features.enable_memory_mapping = false;

    // Framebuffer fetch reads a snapshot of the colour target taken before each
    // draw through last_frag_data, which is what direct_fragcolor selects.
    features.direct_fragcolor = true;
    features.support_shader_interlock = false;

    // DXGI has neither scaled vertex formats nor three-component 8/16-bit
    // formats. Turning both off makes the recompiler emit integer attributes it
    // converts itself, and lets the input layout widen 3-component attributes to
    // 4. Both flags feed get_features_mask, so they must be set before any
    // shader is compiled.
    features.support_scaled_attribute_formats = false;
    features.support_rgb_attributes = false;

    texture_cache.init(true, texture_folder(), game_id);

    // shaders_path is only valid once the title is known, so the on-disk
    // pipeline library can only be picked up here.
    pipeline_cache.read_pipeline_cache();
}

void DXState::cleanup() {
    // The worker holds a raw pointer to the fence, so it has to be joined before
    // anything below releases it.
    stop_notification_thread();

    if (device)
        wait_idle();

    pipeline_cache.save_pipeline_cache();
    pipeline_cache.cleanup();
    surface_cache.cleanup();
    overlay_renderer.destroy();
    screen_renderer.destroy();
    texture_cache.cleanup();
    destroy_swapchain();

    for (FrameObject &frame_object : frames) {
        frame_object.upload_buffer.destroy();
        frame_object.view_descriptors.destroy();
        frame_object.sampler_descriptors.destroy();
        frame_object.destroy_queue.clear();
        frame_object.render_allocator.Reset();
        frame_object.prerender_allocator.Reset();
    }

    default_image.reset();
    default_sampler_heap.destroy();

    rtv_heap.destroy();
    dsv_heap.destroy();
    srv_staging_heap.destroy();

    frame_fence.Reset();
    direct_queue.Reset();
    copy_queue.Reset();
    device.Reset();
    adapter.Reset();
    factory.Reset();
}

// Synchronisation

uint64_t DXState::completed_fence_value() const {
    return frame_fence ? frame_fence->GetCompletedValue() : 0;
}

uint64_t DXState::submit(ID3D12CommandList *const *lists, uint32_t count) {
    if (count > 0)
        direct_queue->ExecuteCommandLists(count, lists);

    const uint64_t fence_value = next_fence_value++;
    direct_queue->Signal(frame_fence.Get(), fence_value);

    return fence_value;
}

void DXState::drain_debug_messages() {
    if (!info_queue)
        return;

    std::lock_guard<std::mutex> guard(info_queue_mutex);

    const uint64_t count = info_queue->GetNumStoredMessages();
    if (count == 0)
        return;

    std::vector<uint8_t> storage;
    for (uint64_t i = 0; i < count; i++) {
        SIZE_T length = 0;
        if (FAILED(info_queue->GetMessage(i, nullptr, &length)) || length == 0)
            continue;

        storage.resize(length);
        auto *message = reinterpret_cast<D3D12_MESSAGE *>(storage.data());
        if (FAILED(info_queue->GetMessage(i, message, &length)))
            continue;

        const std::string_view text(message->pDescription, message->DescriptionByteLength ? message->DescriptionByteLength - 1 : 0);

        switch (message->Severity) {
        case D3D12_MESSAGE_SEVERITY_CORRUPTION:
        case D3D12_MESSAGE_SEVERITY_ERROR:
            LOG_ERROR("D3D12 validation [{}]: {}", static_cast<int>(message->ID), text);
            break;
        case D3D12_MESSAGE_SEVERITY_WARNING:
            LOG_WARN("D3D12 validation [{}]: {}", static_cast<int>(message->ID), text);
            break;
        default:
            LOG_INFO("D3D12 validation [{}]: {}", static_cast<int>(message->ID), text);
            break;
        }
    }

    info_queue->ClearStoredMessages();
}

void DXState::wait_for_fence(uint64_t fence_value) {
    if (!frame_fence || frame_fence->GetCompletedValue() >= fence_value)
        return;

    if (!dx_check(frame_fence->SetEventOnCompletion(fence_value, fence_event), "waiting on the frame fence"))
        return;

    WaitForSingleObject(fence_event, INFINITE);
}

void DXState::wait_idle() {
    if (!direct_queue || !frame_fence)
        return;

    const uint64_t fence_value = next_fence_value++;
    direct_queue->Signal(frame_fence.Get(), fence_value);
    wait_for_fence(fence_value);
}

bool DXState::start_notification_thread() {
    notification_event = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (!notification_event) {
        LOG_ERROR("D3D12: could not create the notification fence event");
        return false;
    }

    notification_thread = std::thread(&DXState::notification_worker, this);
    return true;
}

void DXState::stop_notification_thread() {
    if (notification_thread.joinable()) {
        {
            std::lock_guard<std::mutex> guard(notification_queue_mutex);
            notification_thread_stopping = true;
        }
        notification_queue_cv.notify_all();
        notification_thread.join();
    }

    if (notification_event) {
        CloseHandle(notification_event);
        notification_event = nullptr;
    }
}

void DXState::notification_worker() {
    while (true) {
        PendingNotification pending;
        {
            std::unique_lock<std::mutex> lock(notification_queue_mutex);
            notification_queue_cv.wait(lock, [this]() {
                return notification_thread_stopping || !pending_notifications.empty();
            });

            // Drain what is queued before leaving, so a scene the guest is
            // waiting on never goes unpublished during shutdown.
            if (pending_notifications.empty())
                return;

            pending = pending_notifications.front();
            pending_notifications.pop_front();
        }

        if (frame_fence && frame_fence->GetCompletedValue() < pending.fence_value) {
            if (SUCCEEDED(frame_fence->SetEventOnCompletion(pending.fence_value, notification_event)))
                WaitForSingleObject(notification_event, INFINITE);
        }

        // sceGxmNotificationWait sleeps on notification_ready until the value it
        // polls matches, so the write has to happen under the same lock and be
        // followed by a wake-up.
        {
            std::unique_lock<std::mutex> lock(notification_mutex);
            for (const SceGxmNotification &notification : pending.notifications) {
                if (!notification.address)
                    continue;

                uint32_t *value = notification.address.get(*mem);
                if (value)
                    *value = notification.value;
            }
        }

        notification_ready.notify_all();
    }
}

void DXState::queue_notifications(uint64_t fence_value, const SceGxmNotification &n1, const SceGxmNotification &n2) {
    if (!n1.address && !n2.address)
        return;

    if (!notification_thread.joinable()) {
        // No worker to hand this to, so fall back to blocking here rather than
        // leaving the guest waiting on a value that never lands.
        wait_for_fence(fence_value);
        {
            std::unique_lock<std::mutex> lock(notification_mutex);
            if (n1.address)
                *n1.address.get(*mem) = n1.value;
            if (n2.address)
                *n2.address.get(*mem) = n2.value;
        }
        notification_ready.notify_all();
        return;
    }

    {
        std::lock_guard<std::mutex> guard(notification_queue_mutex);
        pending_notifications.push_back({ fence_value, { n1, n2 } });
    }
    notification_queue_cv.notify_one();
}

void DXState::begin_frame() {
    current_frame_idx = (current_frame_idx + 1) % MAX_FRAMES_RENDERING;
    FrameObject &frame_object = frame();

    // This slot was last used MAX_FRAMES_RENDERING frames ago; wait for that
    // work to retire before touching any of its pools.
    wait_for_fence(frame_object.fence_value);

    frame_object.render_allocator->Reset();
    frame_object.prerender_allocator->Reset();
    frame_object.view_descriptors.reset();
    frame_object.sampler_descriptors.reset();
    // The tables live in those heaps, so the cache has to go with them.
    frame_object.view_table_cache.clear();
    frame_object.sampler_table_cache.clear();
    frame_object.upload_buffer.reclaim(completed_fence_value());
    frame_object.destroy_queue.clear();
}

// Swapchain

bool DXState::create_swapchain(uint32_t width, uint32_t height) {
    DXGI_SWAP_CHAIN_DESC1 desc{
        .Width = width,
        .Height = height,
        .Format = swapchain_format,
        .Stereo = FALSE,
        .SampleDesc = { .Count = 1, .Quality = 0 },
        .BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT,
        .BufferCount = SWAPCHAIN_BUFFER_COUNT,
        .Scaling = DXGI_SCALING_STRETCH,
        .SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD,
        .AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED,
        .Flags = allow_tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0u,
    };

    ComPtr<IDXGISwapChain1> swapchain1;
    if (!dx_check(factory->CreateSwapChainForHwnd(direct_queue.Get(), hwnd, &desc, nullptr, nullptr, &swapchain1),
            "creating the swapchain"))
        return false;

    // Vita3K handles fullscreen itself, so DXGI must not hijack alt+enter.
    factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);

    if (!dx_check(swapchain1.As(&swapchain), "querying IDXGISwapChain3"))
        return false;

    swapchain_width = width;
    swapchain_height = height;

    for (uint32_t i = 0; i < SWAPCHAIN_BUFFER_COUNT; i++) {
        SwapchainBuffer &buffer = swapchain_buffers[i];

        if (!dx_check(swapchain->GetBuffer(i, IID_PPV_ARGS(&buffer.image.resource)), "getting a swapchain buffer"))
            return false;

        // A freshly presented buffer is in the PRESENT state.
        buffer.image.state = D3D12_RESOURCE_STATE_PRESENT;

        buffer.rtv = rtv_heap.allocate();
        if (!buffer.rtv.valid()) {
            LOG_ERROR("D3D12: could not allocate an RTV for a swapchain buffer");
            return false;
        }

        device->CreateRenderTargetView(buffer.image.get(), nullptr, buffer.rtv.cpu);
    }

    current_backbuffer_idx = swapchain->GetCurrentBackBufferIndex();
    swapchain_needs_resize = false;

    LOG_INFO("D3D12: created a {}x{} swapchain ({} buffers, tearing {})",
        width, height, SWAPCHAIN_BUFFER_COUNT, allow_tearing ? "allowed" : "unavailable");

    return true;
}

void DXState::destroy_swapchain() {
    for (SwapchainBuffer &buffer : swapchain_buffers) {
        if (buffer.rtv.valid()) {
            rtv_heap.free(buffer.rtv);
            buffer.rtv = {};
        }
        buffer.image.reset();
    }

    swapchain.Reset();
    swapchain_width = 0;
    swapchain_height = 0;
}

bool DXState::resize_swapchain(uint32_t width, uint32_t height) {
    if (width == 0 || height == 0)
        return false;

    if (width == swapchain_width && height == swapchain_height && !swapchain_needs_resize)
        return true;

    // Every reference to a swapchain buffer has to be released before resizing.
    wait_idle();

    for (SwapchainBuffer &buffer : swapchain_buffers) {
        if (buffer.rtv.valid()) {
            rtv_heap.free(buffer.rtv);
            buffer.rtv = {};
        }
        buffer.image.reset();
    }

    const UINT flags = allow_tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0u;
    if (!dx_check(swapchain->ResizeBuffers(SWAPCHAIN_BUFFER_COUNT, width, height, swapchain_format, flags),
            "resizing the swapchain"))
        return false;

    swapchain_width = width;
    swapchain_height = height;

    for (uint32_t i = 0; i < SWAPCHAIN_BUFFER_COUNT; i++) {
        SwapchainBuffer &buffer = swapchain_buffers[i];

        if (!dx_check(swapchain->GetBuffer(i, IID_PPV_ARGS(&buffer.image.resource)), "getting a resized swapchain buffer"))
            return false;

        buffer.image.state = D3D12_RESOURCE_STATE_PRESENT;

        buffer.rtv = rtv_heap.allocate();
        if (!buffer.rtv.valid())
            return false;

        device->CreateRenderTargetView(buffer.image.get(), nullptr, buffer.rtv.cpu);
    }

    current_backbuffer_idx = swapchain->GetCurrentBackBufferIndex();
    swapchain_needs_resize = false;

    return true;
}

// Presentation
//
// Compute where the Vita image sits inside the window. This is shared with the
// touch code through DisplayState, so it has to run whether or not there is
// anything to draw.
static void update_display_viewport(DisplayState &display, const DXState &state, float fb_w, float fb_h) {
    display.viewport_drawable_w = static_cast<int>(fb_w);
    display.viewport_drawable_h = static_cast<int>(fb_h);

    if (fb_h <= 0.0f)
        return;

    const float window_aspect = fb_w / fb_h;
    constexpr float vita_aspect = static_cast<float>(DEFAULT_RES_WIDTH) / DEFAULT_RES_HEIGHT;
    const bool pixel_perfect = state.fullscreen_hd_res_pixel_perfect && state.fullscreen
        && !(state.swapchain_width % DEFAULT_RES_WIDTH)
        && !(state.swapchain_height % (DEFAULT_RES_HEIGHT - 4));

    if (state.stretch_the_display_area && !pixel_perfect) {
        display.viewport_x = 0.0f;
        display.viewport_y = 0.0f;
        display.viewport_w = fb_w;
        display.viewport_h = fb_h;
    } else if ((window_aspect > vita_aspect) && !pixel_perfect) {
        display.viewport_w = fb_h * vita_aspect;
        display.viewport_h = fb_h;
        display.viewport_x = (fb_w - display.viewport_w) / 2.0f;
        display.viewport_y = 0.0f;
    } else {
        display.viewport_w = fb_w;
        display.viewport_h = fb_w / vita_aspect;
        display.viewport_x = 0.0f;
        display.viewport_y = (fb_h - display.viewport_h) / 2.0f;
    }
}

void DXState::render_frame(DisplayState &display, const GxmState &gxm, MemState &mem) {
    // We are displaying this frame, wait for a new one.
    should_display = false;

    DisplayFrameInfo frame_info;
    {
        std::lock_guard<std::mutex> guard(display.display_info_mutex);
        frame_info = display.next_rendered_frame;
    }

    update_overlays();
    bool has_overlays = false;
    if (overlay_manager) {
        overlay_manager->lock_shared();
        has_overlays = overlay_manager->has_visible();
        overlay_manager->unlock_shared();
    }

    if (!frame_info.base && !has_overlays)
        return;

    FrameHost *frame_host = this->renderer::State::frame;
    const uint32_t window_width = static_cast<uint32_t>(std::max(frame_host->drawable_width(), 1));
    const uint32_t window_height = static_cast<uint32_t>(std::max(frame_host->drawable_height(), 1));
    if (window_width != swapchain_width || window_height != swapchain_height || swapchain_needs_resize) {
        if (!resize_swapchain(window_width, window_height))
            return;
    }

    // Presentation is the real frame boundary: this is where the swapchain is
    // waited on, so this is where the per-frame pools are recycled.
    begin_frame();

    current_backbuffer_idx = swapchain->GetCurrentBackBufferIndex();
    SwapchainBuffer &backbuffer = swapchain_buffers[current_backbuffer_idx];

    update_display_viewport(display, *this, static_cast<float>(swapchain_width), static_cast<float>(swapchain_height));

    FrameObject &frame_object = frame();

    ID3D12GraphicsCommandList *cmd_list = frame_object.render_cmd_list.Get();
    if (!dx_check(cmd_list->Reset(frame_object.render_allocator.Get(), nullptr), "resetting the frame command list"))
        return;

    BarrierBatcher batcher;
    batcher.transition(backbuffer.image, D3D12_RESOURCE_STATE_RENDER_TARGET);
    batcher.flush(cmd_list);

    // Compile the overlays and stage their fonts and images before any drawing
    // starts, so the copies are done by the time the draws sample them.
    if (has_overlays)
        overlay_renderer.prepare(cmd_list, batcher, *overlay_manager, display.viewport_w, display.viewport_h);

    cmd_list->OMSetRenderTargets(1, &backbuffer.rtv.cpu, FALSE, nullptr);

    // Clear first so the letterbox bars around the Vita image are black.
    static constexpr float clear_colour[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    cmd_list->ClearRenderTargetView(backbuffer.rtv.cpu, clear_colour, 0, nullptr);

    if (frame_info.base) {
        const D3D12_VIEWPORT screen_viewport{
            .TopLeftX = display.viewport_x,
            .TopLeftY = display.viewport_y,
            .Width = display.viewport_w,
            .Height = display.viewport_h,
            .MinDepth = 0.0f,
            .MaxDepth = 1.0f,
        };

        screen_renderer.use_linear_filter = screen_filter != Filter::NEAREST;

        // Prefer the colour surface the GPU actually rendered into. Its guest
        // memory copy is never written back, so reading that instead would
        // present an empty frame.
        Viewport source;
        source.width = static_cast<uint32_t>(frame_info.image_size.x);
        source.height = static_cast<uint32_t>(frame_info.image_size.y);

        ColorSurfaceCacheInfo *surface = surface_cache.sourcing_color_surface_for_presentation(
            frame_info.base, frame_info.pitch, source);

        if (surface) {
            screen_renderer.render_surface(cmd_list, batcher, surface->image, surface->srv, source, screen_viewport);
        } else {
            // The frame was never drawn through a tracked render target, so the
            // guest framebuffer is the only copy that exists.
            screen_renderer.render(cmd_list, batcher, frame_info, mem, screen_viewport);
        }

        // The blit re-binds descriptor heaps and pipeline state, so the render
        // target has to be re-set for anything drawn after it.
        cmd_list->OMSetRenderTargets(1, &backbuffer.rtv.cpu, FALSE, nullptr);
    }

    if (has_overlays) {
        overlay_renderer.render(cmd_list,
            display.viewport_x, display.viewport_y, display.viewport_w, display.viewport_h);
    }

    batcher.transition(backbuffer.image, D3D12_RESOURCE_STATE_PRESENT);
    batcher.flush(cmd_list);

    if (!dx_check(cmd_list->Close(), "closing the present command list"))
        return;

    ID3D12CommandList *lists[] = { cmd_list };
    frame_object.fence_value = submit(lists, 1);
    frame_object.upload_buffer.retire(frame_object.fence_value);
    overlay_renderer.retire(frame_object.fence_value);

    // Anything the debug layer recorded while this frame was recorded belongs in
    // the log next to the frame it came from.
    drain_debug_messages();
    // destroy_queue is drained in begin_frame, once this fence has been passed.
}

void DXState::swap_window() {
    if (!swapchain)
        return;

    // -1 means the frontend has not pushed a value yet; vsync is on by default.
    const int vsync = pending_vsync.load(std::memory_order_relaxed);
    const bool vsync_enabled = vsync != 0;

    const UINT sync_interval = vsync_enabled ? 1u : 0u;
    // Tearing may only be requested when presenting without vsync.
    const UINT present_flags = (!vsync_enabled && allow_tearing) ? DXGI_PRESENT_ALLOW_TEARING : 0u;

    const HRESULT hr = swapchain->Present(sync_interval, present_flags);
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
        const HRESULT reason = device->GetDeviceRemovedReason();
        LOG_CRITICAL("D3D12: the device was removed while presenting: {}", hresult_to_string(reason));
        return;
    }

    if (FAILED(hr)) {
        // A failed present usually means the window changed underneath us.
        LOG_WARN("D3D12: present failed ({}), recreating the swapchain", hresult_to_string(hr));
        swapchain_needs_resize = true;
        return;
    }

    current_backbuffer_idx = swapchain->GetCurrentBackBufferIndex();

    // Check once a frame whether the pipeline library is due to be written out.
    const auto time_s = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch())
                            .count();
    if (static_cast<uint64_t>(time_s) >= pipeline_cache.next_pipeline_cache_save) {
        pipeline_cache.save_pipeline_cache();
        pipeline_cache.next_pipeline_cache_save = ~0ULL;
    }
}

std::vector<uint32_t> DXState::dump_frame(DisplayState &display, uint32_t &width, uint32_t &height) {
    DisplayFrameInfo frame_info;
    {
        std::lock_guard<std::mutex> guard(display.display_info_mutex);
        frame_info = display.next_rendered_frame;
    }

    width = static_cast<uint32_t>(frame_info.image_size.x * res_multiplier);
    height = static_cast<uint32_t>(frame_info.image_size.y * res_multiplier);

    // Reading the frame back needs a GPU readback of the presented image, which
    // arrives with the surface cache.
    LOG_WARN("D3D12: frame dumping is not implemented yet");
    return {};
}

// Capabilities

uint32_t DXState::get_features_mask() {
    // Must stay layout-compatible with the vulkan backend: the value is folded
    // into the shader cache key, so a mismatch would silently reuse the wrong
    // compiled shaders.
    union {
        struct {
            bool use_shader_interlock : 1;
            bool use_texture_viewport : 1;
            bool use_memory_mapping : 1;
            bool use_rgb_attributes : 1;
            bool use_scaled_attributes : 1;
        };
        uint32_t value;
    } features_mask;
    static_assert(sizeof(features_mask) == sizeof(uint32_t));

    features_mask.value = 0;
    features_mask.use_shader_interlock = features.support_shader_interlock;
    features_mask.use_texture_viewport = features.use_texture_viewport;
    features_mask.use_memory_mapping = features.enable_memory_mapping;
    features_mask.use_rgb_attributes = features.support_rgb_attributes;
    features_mask.use_scaled_attributes = false;

    return features_mask.value;
}

int DXState::get_supported_filters() {
    // Only the filters the screen renderer can actually apply are advertised;
    // bicubic, FXAA and FSR arrive with the post-processing passes.
    return static_cast<int>(Filter::NEAREST) | static_cast<int>(Filter::BILINEAR);
}

void DXState::set_screen_filter(const std::string_view &filter) {
    if (filter == "Nearest")
        screen_filter = Filter::NEAREST;
    else
        screen_filter = Filter::BILINEAR;
}

int DXState::get_max_anisotropic_filtering() {
    return D3D12_REQ_MAXANISOTROPY;
}

void DXState::set_anisotropic_filtering(int anisotropic_filtering) {
    this->anisotropic_filtering = anisotropic_filtering;
    texture_cache.anisotropic_filtering = anisotropic_filtering;
}

int DXState::get_max_2d_texture_width() {
    return D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION;
}

std::string_view DXState::get_gpu_name() {
    return gpu_name;
}

uint32_t DXState::get_gpu_version() {
    return device_id;
}

void DXState::precompile_shader(const ShadersHash &hash) {
    // Precompilation goes through the PSO cache, which is not built yet.
}

void DXState::preclose_action() {
    if (device)
        wait_idle();
}

} // namespace renderer::d3d12
