// Vita3K emulator project
// Copyright (C) 2025 Vita3K team
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

#include <array>
#include <cstdint>
#include <string>

using Sha256Hash = std::array<uint8_t, 32>;

Sha256Hash sha256(const void *data, size_t size);
typedef std::array<char, 65> Sha256HashText;

void hex_buf(const std::uint8_t *hash, char *dst, const std::size_t source_size);

template <size_t N>
std::string hex_string(const std::array<uint8_t, N> &hash) {
    std::string dst(2 * N, 0);
    hex_buf(hash.data(), dst.data(), N);

    return dst;
}
