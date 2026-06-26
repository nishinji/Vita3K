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
//
// CPU/system identification helpers. Structure adapted from RPCS3's
// util/sysinfo.cpp (GPLv2), trimmed to Vita3K's needs and extended with
// Android support.

#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

namespace util {

// CPU brand string, e.g. "AMD Ryzen 7 5800X", "Apple M2", "Snapdragon 8 Gen 2".
std::string get_cpu_brand();

// Compile-time CPU architecture: "x64", "arm64" or "unknown".
std::string_view get_architecture();

// Number of logical processors (>= 1).
uint32_t get_thread_count();

// Best-effort max CPU clock in MHz, 0 if it cannot be determined
// (notably Apple Silicon, which does not expose it).
int64_t get_cpu_max_clock_mhz();

// One-line summary: "<brand> | <N> Threads | <clock> | <features>".
std::string get_system_info();

// 物理 RAM 総量（バイト）。
uint64_t get_total_memory();

// { 総物理メモリ, 使用中メモリ }（バイト）。
// used = total - available のベストエフォート推定。macOS は近似値、
// 判定不能な OS では used が 0 になることがある。
std::pair<uint64_t, uint64_t> get_memory_usage();

// 現在のプロセス（Vita3K 自身）が使用中の物理 RAM 量（バイト）。
// Windows はワーキングセット、Linux/Android は RSS、macOS は resident_size。
uint64_t get_process_memory_usage();

} // namespace util
