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

#include "patch/util.h"

#include <algorithm>
#include <ranges>
#include <util/log.h>

// This function will return a PatchHeader struct, which contains the titleid and the binary name (if provided)
std::optional<PatchHeader> read_header(std::string &header, bool is_patchlist) {
    PatchHeader patch_header;

    strip_arg_spaces(header, '[', ']');

    auto args = get_args(header, '[', ']');
    if (args.empty())
        return std::nullopt;

    // When this is in a patchlist file, the possible values are [titleid, bin] and [titleid]
    // When this is in a title-specific patch file, the possible values are just [bin] (because the titleid is already known)
    if (is_patchlist) {
        patch_header.titleid = args[0];

        if (args.size() > 1)
            patch_header.bin = args[1];
    } else {
        patch_header.titleid = "";

        // If we have more than one argument, user probably made a mistake, so we can correct it
        if (args.size() > 1) {
            LOG_WARN("Found more than one argument in a title-specific patch file. You only need to specify the binary name because the TitleID is already known.");
            patch_header.bin = args[1];

            return patch_header;
        }

        patch_header.bin = args[0];
    }

    return patch_header;
}

std::vector<uint8_t> to_bytes(unsigned long long value, uint8_t count) {
    std::vector<uint8_t> bytes;

    // If count is 0, go until we see a byte of all 0s
    if (count == 0) {
        for (; value != 0; value >>= 8)
            bytes.push_back(static_cast<uint8_t>(value));

        return bytes;
    }

    // Otherwise, just go as much as count tells us
    bytes.reserve(count);
    for (uint8_t i = count; i > 0; --i)
        bytes.push_back(static_cast<uint8_t>(value >> ((i - 1) * 8)));

    return bytes;
}

void strip_arg_spaces(std::string &line, char open, char close) {
    std::string stripped;
    stripped.reserve(line.size());

    bool in_brackets = false;
    for (const char c : line) {
        if (c == open)
            in_brackets = true;
        else if (c == close)
            in_brackets = false;

        if (!in_brackets || c != ' ')
            stripped += c;
    }

    line = std::move(stripped);
}

void strip_arg_spaces(std::string &line) {
    return strip_arg_spaces(line, '(', ')');
}

static const Op *find_op(std::string_view inst) {
    const auto it = std::ranges::find(instruction_funcs, inst, &Op::name);

    return it == instruction_funcs.end() ? nullptr : &*it;
}

Instruction to_instruction(std::string_view inst) {
    const Op *op = find_op(inst);

    return op ? op->instruction : Instruction::INVALID;
}

bool is_valid_instruction(std::string_view inst) {
    return to_instruction(strip_args(inst)) != Instruction::INVALID;
}

std::string strip_args(std::string_view inst) {
    const auto open = inst.find('(');
    const auto close = inst.find(')');

    if (open == std::string_view::npos || close == std::string_view::npos)
        return std::string(inst);

    std::string stripped(inst);
    stripped.erase(open, close - open + 1);

    return stripped;
}

std::vector<std::string> get_args(std::string_view inst, char open, char close) {
    const auto open_pos = inst.find(open);
    const auto close_pos = inst.find(close);

    if (open_pos == std::string_view::npos || close_pos == std::string_view::npos)
        return {};

    const auto values = inst.substr(open_pos + 1, close_pos - open_pos - 1);

    return values
        | std::views::split(',')
        | std::views::transform([](auto &&arg) { return std::string(std::string_view(arg)); })
        | std::ranges::to<std::vector>();
}

std::vector<std::string> get_args(std::string_view inst) {
    return get_args(inst, '(', ')');
}

uint32_t translate(std::string_view inst, std::vector<uint32_t> &args) {
    if (const Op *op = find_op(inst))
        return op->translate(args);

    LOG_WARN("Instruction {} could not be translated! It will be replaced with NOP", inst);

    return 0;
}
