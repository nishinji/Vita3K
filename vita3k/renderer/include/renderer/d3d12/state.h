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
#include <renderer/d3d12/overlay_renderer.h>
#include <renderer/d3d12/pipeline_cache.h>
#include <renderer/d3d12/resource.h>
#include <renderer/d3d12/screen_renderer.h>
#include <renderer/d3d12/surface_cache.h>
#include <renderer/d3d12/types.h>
#include <renderer/state.h>

#include <array>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct Config;

namespace renderer::d3d12 {

// One swapchain image plus the RTV that addresses it.
struct SwapchainBuffer {
    Resource image;
    DescriptorHandle rtv;
};

struct DXState : public renderer::State {
    MemState *mem = nullptr;

    // 0 = automatic, > 0 = 1-based index into the enumerated adapter list.
    int gpu_idx = 0;

    DXTextureCache texture_cache;
    PipelineCache pipeline_cache;
    DXSurfaceCache surface_cache;
    ScreenRenderer screen_renderer;
    OverlayRenderer overlay_renderer;

    ComPtr<IDXGIFactory4> factory;
    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<ID3D12Device> device;
    D3D_FEATURE_LEVEL feature_level = D3D_FEATURE_LEVEL_11_0;

    // Only set when the validation layer is on. Its messages are pulled into the
    // emulator log rather than left in the debugger's output window, so a report
    // of a validation failure carries the message that caused it.
    ComPtr<ID3D12InfoQueue> info_queue;
    std::mutex info_queue_mutex;

    // Graphics + compute + copy. Copies that can overlap rendering go on the
    // dedicated copy queue instead.
    ComPtr<ID3D12CommandQueue> direct_queue;
    ComPtr<ID3D12CommandQueue> copy_queue;

    ComPtr<IDXGISwapChain3> swapchain;
    std::array<SwapchainBuffer, SWAPCHAIN_BUFFER_COUNT> swapchain_buffers;
    DXGI_FORMAT swapchain_format = DXGI_FORMAT_R8G8B8A8_UNORM;
    uint32_t swapchain_width = 0;
    uint32_t swapchain_height = 0;
    uint32_t current_backbuffer_idx = 0;
    bool swapchain_needs_resize = false;
    // DXGI_PRESENT_ALLOW_TEARING requires both the flag on the swapchain and a
    // present without vsync; only usable when the adapter reports support.
    bool allow_tearing = false;
    HWND hwnd = nullptr;

    // CPU-side descriptor pools. Shader-visible descriptors live per frame in
    // FrameObject instead, because they are consumed while recording.
    StagingDescriptorAllocator rtv_heap;
    StagingDescriptorAllocator dsv_heap;
    StagingDescriptorAllocator srv_staging_heap;

    // Bound into texture slots a shader reads but the game never set, so a
    // stale or null descriptor never reaches the GPU.
    Resource default_image;
    DescriptorHandle default_srv;
    StagingDescriptorAllocator default_sampler_heap;
    DescriptorHandle default_sampler;

    // Frame pacing. next_fence_value is the value the next submission signals.
    ComPtr<ID3D12Fence> frame_fence;
    HANDLE fence_event = nullptr;
    uint64_t next_fence_value = 1;

    std::array<FrameObject, MAX_FRAMES_RENDERING> frames;
    uint32_t current_frame_idx = 0;

    std::string gpu_name;
    std::string driver_version;
    uint32_t vendor_id = 0;
    uint32_t device_id = 0;
    uint64_t dedicated_video_memory = 0;

    Filter screen_filter = Filter::BILINEAR;
    int anisotropic_filtering = 1;

    // Guards command-list creation from the texture/shader worker threads.
    std::mutex device_mutex;

    // A scene's notifications may only be published once the GPU has finished
    // with it. Waiting for that on the render thread serialises the CPU with the
    // GPU once per scene, which costs whole seconds a frame in games that render
    // through several passes; a worker does the waiting instead, mirroring the
    // vulkan backend's request queue.
    struct PendingNotification {
        uint64_t fence_value = 0;
        SceGxmNotification notifications[2]{};
    };

    std::thread notification_thread;
    std::mutex notification_queue_mutex;
    std::condition_variable notification_queue_cv;
    std::deque<PendingNotification> pending_notifications;
    bool notification_thread_stopping = false;
    // The worker cannot share fence_event with the render thread: two waiters on
    // one auto-reset event would steal each other's signal.
    HANDLE notification_event = nullptr;

    explicit DXState(int gpu_idx);
    ~DXState() override;

    // renderer::State

    bool init() override;
    bool create(std::unique_ptr<renderer::State> &state, const Config &config);
    void late_init(const Config &cfg, const std::string_view game_id, MemState &mem) override;
    void cleanup() override;

    TextureCache *get_texture_cache() override {
        return &texture_cache;
    }

    void render_frame(DisplayState &display, const GxmState &gxm, MemState &mem) override;
    void swap_window() override;
    std::vector<uint32_t> dump_frame(DisplayState &display, uint32_t &width, uint32_t &height) override;

    uint32_t get_features_mask() override;
    int get_supported_filters() override;
    void set_screen_filter(const std::string_view &filter) override;
    int get_max_anisotropic_filtering() override;
    void set_anisotropic_filtering(int anisotropic_filtering) override;
    int get_max_2d_texture_width() override;

    std::string_view get_gpu_name() override;
    uint32_t get_gpu_version() override;

    void precompile_shader(const ShadersHash &hash) override;
    void preclose_action() override;

    // D3D12 specifics

    FrameObject &frame() {
        return frames[current_frame_idx];
    }

    // Submit `count` lists on the direct queue and return the fence value that
    // will be signalled once they retire.
    uint64_t submit(ID3D12CommandList *const *lists, uint32_t count);
    // Block until the GPU has passed `fence_value`.
    void wait_for_fence(uint64_t fence_value);
    // Block until every submission so far has retired.
    void wait_idle();
    uint64_t completed_fence_value() const;

    // Log and clear whatever the debug layer has recorded. A no-op when the
    // validation layer is off.
    void drain_debug_messages();

    // Move to the next frame slot, waiting for it to retire and recycling its
    // per-frame pools.
    void begin_frame();

    // Publish `n1` and `n2` to guest memory once the GPU has passed
    // `fence_value`. Returns immediately; the worker does the waiting.
    void queue_notifications(uint64_t fence_value, const SceGxmNotification &n1, const SceGxmNotification &n2);

    bool create_swapchain(uint32_t width, uint32_t height);
    void destroy_swapchain();
    bool resize_swapchain(uint32_t width, uint32_t height);

private:
    // Hardware adapters, best first, with the software adapter filtered out.
    std::vector<ComPtr<IDXGIAdapter1>> enumerate_adapters();
    bool create_device(const Config &config);
    bool create_queues();
    bool create_frame_objects();
    bool create_default_resources();

    bool start_notification_thread();
    void stop_notification_thread();
    void notification_worker();
};

} // namespace renderer::d3d12
