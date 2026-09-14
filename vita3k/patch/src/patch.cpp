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

#include "patch/patch.h"
#include "patch/instructions.h"
#include "patch/util.h"

#include <util/fs.h>
#include <util/log.h>
#include <util/string_utils.h>

#include <algorithm>
#include <ranges>

Patches get_patches(fs::path &path, const std::string &titleid, const std::string &bin) {
    // Find a file in the path with the titleid
    Patches patches;

    for (auto &entry : fs::directory_iterator(path)) {
        // Just in case users decide to use lowercase filenames
        const auto filename = string_utils::toupper(fs_utils::path_to_utf8(entry.path().filename()));

        bool is_patchlist = filename.contains("PATCHLIST.TXT");

        if ((filename.contains(titleid) && filename.ends_with(".TXT")) || is_patchlist) {
            // Read the file
            std::ifstream file(entry.path().c_str());
            PatchHeader patch_header = PatchHeader{
                "",
                "eboot.bin"
            };

            auto line_number = 0;
            std::string line;
            // Parse the file
            while (std::getline(file, line)) {
                line_number++;

                // If this is a header, remember the binary the next patches are for
                if (!line.empty() && line[0] == '[') {
                    if (const auto header = read_header(line, is_patchlist))
                        patch_header = *header;
                    else
                        LOG_ERROR("Failed to parse patch header: {} [{}]", line_number, line);
                    continue;
                }

                // Ignore comments and patches for other binaries
                // And @ lines for now
                if (line.empty() || line[0] == '#' || line[0] == '@' || !bin.contains(patch_header.bin) || (is_patchlist && patch_header.titleid != titleid))
                    continue;

                try {
                    patches.push_back(parse_patch(line));
                } catch (std::exception &e) {
                    LOG_ERROR("Failed to parse patch line: {} [{}]", line_number, line);
                    LOG_ERROR("Failed with: {}", e.what());
                }
            }
        }
    }

    LOG_INFO("Found {} patches for titleid {}", patches.size(), titleid);

    return patches;
}

Patch parse_patch(const std::string &patch) {
    // FORMAT: <seg>:<offset> <values>
    // Example, equivalent to `t1_mov(0, 1)`:
    // 0:0xA994 0x01 0x20
    // Keep in mind that we are in little endian
    const auto colon = patch.find(':');
    const auto space = patch.find(' ');

    const auto seg = static_cast<uint8_t>(std::stoi(patch.substr(0, colon)));

    // Everything after the first colon, and before the first space, is the offset
    const auto offset = static_cast<uint32_t>(std::stoull(patch.substr(colon + 1, space - colon - 1), nullptr, 16));

    // All following values (separated by spaces) are the values to be written
    std::string values = patch.substr(space + 1);
    // set vblank to 1(60Hz) for now
    string_utils::replace(values, "4 - vblank", "3");
    string_utils::replace(values, "vblank - 1", "0");
    string_utils::replace(values, "vblank", "1");
    std::vector<uint8_t> values_vec;

    // Clean up potential instructions by removing spaces in between brackets
    // Eg. t1_mov(0, 1) becomes t1_mov(0,1)
    strip_arg_spaces(values);

    // Get all additional values separated by spaces
    for (const auto raw_value : std::views::split(std::string_view(values), ' ')) {
        std::string_view val(raw_value);

        // Strip 0x from the value if it exists
        if (val.length() > 2 && val.starts_with("0x"))
            val.remove_prefix(2);

        unsigned long long bytes = 0;
        uint8_t byte_count = 0;
        const std::string inst = strip_args(val);

        if (to_instruction(inst) != Instruction::INVALID) {
            const auto args = get_args(val);
            std::vector<uint32_t> arg_conv;

            arg_conv.reserve(args.size());
            std::ranges::transform(args, std::back_inserter(arg_conv), [](const std::string &s) { return static_cast<uint32_t>(std::stoull(s, nullptr, 16)); });

            bytes = translate(inst, arg_conv);

            LOG_INFO("Translated {} to 0x{:X}", val, bytes);
        } else {
            bytes = std::stoull(std::string(val), nullptr, 16);
            // We need to count this, as patches may have bytes of zeros that we don't want to just ignore by passing 0 to toBytes
            byte_count = static_cast<uint8_t>((val.length() + 1) / 2);
        }

        const auto byte_vec = to_bytes(bytes, byte_count);
        values_vec.insert(values_vec.end(), byte_vec.begin(), byte_vec.end());
    }

    return Patch{ seg, offset, values_vec };
}
