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

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <Windows.h>

#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <util/log.h>

#include <cstdint>
#include <string>

namespace renderer::d3d12 {

template <typename T>
using ComPtr = Microsoft::WRL::ComPtr<T>;

// Number of frames the CPU is allowed to run ahead of the GPU. Mirrors
// MAX_FRAMES_RENDERING in the vulkan backend so per-frame resource pools line up.
constexpr uint32_t MAX_FRAMES_RENDERING = 3;

// The Vita renders at 960x544; everything else is derived from res_multiplier.
constexpr uint32_t SWAPCHAIN_BUFFER_COUNT = 3;

std::string hresult_to_string(HRESULT hr);

// Log an error and return false when `hr` failed. Meant to be used as
//   if (!dx_check(hr, "creating the swapchain")) return false;
inline bool dx_check(HRESULT hr, const char *what) {
    if (SUCCEEDED(hr))
        return true;

    LOG_ERROR("D3D12: failed {}: {}", what, hresult_to_string(hr));
    return false;
}

} // namespace renderer::d3d12
