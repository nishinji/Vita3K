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

#include <algorithm>
#include <optional>
#include <ranges>
#include <vector>

namespace vector_utils {

/**
 * \brief Merges and sorts two vectors. Also eliminates duplicates.
 * \param cur The current vector.
 * \param append The new vector to append. Always assume to be smaller than cur.
 * \return A vector of the same type as the inputs.
 */
template <typename T, typename A = std::allocator<T>>
std::vector<T, A> merge_vectors(const std::vector<T, A> &cur, const std::vector<T, A> &append) {
    std::vector<T, A> merged = cur;
    merged.insert(merged.end(), append.begin(), append.end());

    std::ranges::sort(merged);
    const auto duplicates = std::ranges::unique(merged);
    merged.erase(duplicates.begin(), duplicates.end());

    return merged;
}

template <std::ranges::forward_range T, typename V>
std::optional<size_t> find_index(const T &v, const V &value) {
    const auto it = std::ranges::find(v, value);
    if (it == std::ranges::end(v))
        return std::nullopt;

    return static_cast<size_t>(std::ranges::distance(std::ranges::begin(v), it));
}

template <std::ranges::forward_range T, typename V>
bool push_if_not_exists(T &v, const V &value) {
    if (std::ranges::contains(v, value))
        return true;

    v.push_back(value);
    return false;
}

template <std::ranges::forward_range T, typename V>
bool erase_first(T &v, const V &value) {
    const auto it = std::ranges::find(v, value);
    if (it == std::ranges::end(v))
        return false;

    v.erase(it);
    return true;
}

} // namespace vector_utils
