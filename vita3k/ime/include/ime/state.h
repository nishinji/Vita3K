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

#include <ime/types.h>

#include <map>
#include <string>
#include <vector>

struct ImeLangState {
    std::vector<std::pair<SceImeLanguage, std::string>> ime_keyboards = {
        { SCE_IME_LANGUAGE_DANISH, "Danish" }, { SCE_IME_LANGUAGE_GERMAN, "German" },
        { SCE_IME_LANGUAGE_ENGLISH_GB, "English (United Kingdom)" }, { SCE_IME_LANGUAGE_ENGLISH_US, "English (United States)" },
        { SCE_IME_LANGUAGE_SPANISH, "Spanish" }, { SCE_IME_LANGUAGE_FRENCH, "French" },
        { SCE_IME_LANGUAGE_ITALIAN, "Italian" }, { SCE_IME_LANGUAGE_DUTCH, "Dutch" },
        { SCE_IME_LANGUAGE_NORWEGIAN, "Norwegian" }, { SCE_IME_LANGUAGE_POLISH, "Polish" },
        { SCE_IME_LANGUAGE_PORTUGUESE_BR, "Portuguese (Brazil)" }, { SCE_IME_LANGUAGE_PORTUGUESE_PT, "Portuguese (Portugal)" },
        { SCE_IME_LANGUAGE_RUSSIAN, "Russian" }, { SCE_IME_LANGUAGE_FINNISH, "Finnish" },
        { SCE_IME_LANGUAGE_SWEDISH, "Swedish" }, { SCE_IME_LANGUAGE_TURKISH, "Turkish" }
    };
};

struct ImeKeyboardLayout {
    std::map<int, std::vector<std::u16string>> keys;
    std::map<int, std::vector<std::u16string>> shift_keys;
    std::map<int, std::vector<std::u16string>> punct;
    std::map<int, std::vector<std::u16string>> special_keys = {
        { 1, { u"!", u"?", u"\"", u"'", u"#", u"%" } },
        { 2, { u"(", u")", u"()", u"_", u",", u";" } },
        { 3, { u"*", u"+", u"=", u"&", u"<", u">" } },
        { 4, { u"[", u"]", u"[]", u"{", u"}", u"{}" } },
        { 5, { u"\\", u"|", u"^", u"`", u"$", u"\u00A5" } },
        { 6, { u"\u00B4", u"\u2018", u"\u2019", u"\u201A", u"\u201C", u"\u201D" } },
        { 7, { u"\u201E", u"~", u"\u00A1", u"\u00A1!", u"\u00BF", u"\u00BF?" } },
        { 8, { u"\u2039", u"\u203A", u"\u00AB", u"\u00BB", u"\u00B0", u"\u00AA" } },
        { 9, { u"\u00BA", u"\u00D7", u"\u00F7", u"\u00A4", u"\u00A2", u"\u20AC" } },
        { 10, { u"\u00A3", u"\u20A9", u"\u00A7", u"\u00A6", u"\u00B5", u"\u00AC" } },
        { 11, { u"\u00B9", u"\u00B2", u"\u00B3", u"\u00BC", u"\u00BD", u"\u00BE" } },
        { 12, { u"\u2116", u"\u00B7" } }
    };
    std::map<int, std::vector<std::u16string>> numeric_keys = {
        { 1, { u"7", u"8", u"9", u"-", u"/" } },
        { 2, { u"4", u"5", u"6", u":", u"@" } },
        { 3, { u"1", u"2", u"3" } }
    };
    std::map<int, std::string> second_keyboard;
    std::string space_str = "Space";
};

struct Ime {
    ImeLangState lang;
    ImeKeyboardLayout layout;
    bool state = false;
    SceImeEditText edit_text;
    SceImeParam param;
    std::string enter_label;
    std::u16string str;
    uint32_t caps_level = 0;
    uint32_t caretIndex = 0;
    uint32_t event_id = SCE_IME_EVENT_OPEN;
};
