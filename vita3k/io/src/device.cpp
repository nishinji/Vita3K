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

#include <algorithm>
#include <ranges>

#include <io/device.h>

namespace device {

std::string construct_normalized_path(const VitaIoDevice dev, const std::string &path, const std::string &ext) {
    const auto device_path = get_device_string(dev, true);
    if (path.empty()) // Wants the device only
        return device_path + "/";

    // Normalize the path
    auto normalized = path.front() == '/' ? device_path + path : device_path + '/' + path;

    if (!ext.empty()) {
        if (fs::path(normalized).has_extension())
            normalized.erase(normalized.find_last_of('.'));
        normalized += '.' + ext;
    }

    return normalized;
}

std::string remove_device_from_path(const std::string &path, const VitaIoDevice device, const std::string &mod_path) {
    if (device == VitaIoDevice::_INVALID)
        return {};
    // Trim the path to include only the substring after the device string
    const auto device_length = get_device_string(device, true).length();
    auto out = path.substr(device_length);
    if (!mod_path.empty())
        out = (!out.empty() && out.front() == '/') ? mod_path + out : mod_path + '/' + out;
    if (out.starts_with('/'))
        out.erase(0, 1);

    return out;
}

VitaIoDevice get_device(const std::string &path) {
    if (path.empty())
        return VitaIoDevice::_INVALID;

    const auto colon = path.find_first_of(':');
    if (colon == std::string::npos)
        return VitaIoDevice::_INVALID;

    auto p = path.substr(0, colon);
    std::ranges::transform(p, p.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    VitaIoDevice result;
    if (boost::describe::enum_from_string(p.c_str(), result))
        return result;

    return VitaIoDevice::_INVALID;
}

std::string get_device_string(const VitaIoDevice dev, const bool with_colon) {
    return with_colon ? std::string(boost::describe::enum_to_string(dev, "")).append(":") : boost::describe::enum_to_string(dev, "");
}

std::string remove_duplicate_device(const std::string &path, VitaIoDevice &device) {
    auto cur_path = remove_device_from_path(path, device);
    if (get_device(cur_path) != VitaIoDevice::_INVALID) {
        device = get_device(cur_path);
        if (cur_path.contains(':'))
            cur_path = remove_duplicate_device(cur_path, device);
        return cur_path;
    }

    return path;
}

fs::path construct_emulated_path(const VitaIoDevice dev, const fs::path &path, const fs::path &base_path, const bool redirect_pwd, const std::string &ext) {
    if (redirect_pwd && dev == VitaIoDevice::host0) {
        return fs::current_path() / path;
    }
    return fs_utils::construct_file_name(base_path, get_device_string(dev, false), path, ext);
}

} // namespace device
