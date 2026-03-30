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

#include <ime/functions.h>

#include <algorithm>

static void update_str(Ime &ime, const std::u16string &key) {
    if (ime.str.empty())
        ime.edit_text.preeditIndex = 0;
    ime.edit_text.editIndex = ime.edit_text.caretIndex;
    if (ime.str.length() < ime.param.maxTextLength) {
        ime.str.insert(ime.edit_text.caretIndex, key);
        ime.edit_text.caretIndex++;
    }
    if (ime.caps_level == IME_CAPS_SHIFT)
        ime.caps_level = IME_CAPS_NONE;
    ime.event_id = SCE_IME_EVENT_UPDATE_TEXT;
}

void ime_update_key(Ime &ime, const std::u16string &key) {
    update_str(ime, key);
    ime.edit_text.editLengthChange = ime.edit_text.preeditLength += static_cast<int32_t>(key.length());
}

void ime_reset_preedit(Ime &ime) {
    ime.caretIndex = ime.edit_text.caretIndex;
    ime.edit_text.preeditIndex = ime.edit_text.caretIndex;
    ime.edit_text.preeditLength = ime.edit_text.editLengthChange = 0;
    ime.event_id = SCE_IME_EVENT_UPDATE_TEXT;
}

void ime_update_ponct(Ime &ime, const std::u16string &key) {
    update_str(ime, key);
    ime_reset_preedit(ime);
}

void ime_cursor_left(Ime &ime) {
    ime.edit_text.editIndex = ime.edit_text.caretIndex;
    if (ime.edit_text.caretIndex)
        --ime.edit_text.caretIndex;
    if (ime.edit_text.editIndex == ime.edit_text.preeditIndex)
        ime_reset_preedit(ime);
    else if (ime.caretIndex) {
        --ime.caretIndex;
        ime.event_id = SCE_IME_EVENT_UPDATE_CARET;
    }
    if ((ime.edit_text.caretIndex == 0) && (ime.caps_level == IME_CAPS_NONE))
        ime.caps_level = IME_CAPS_SHIFT;
}

void ime_cursor_right(Ime &ime) {
    ime.edit_text.editIndex = ime.edit_text.caretIndex;
    if (ime.edit_text.caretIndex < ime.str.length())
        ++ime.edit_text.caretIndex;
    if (ime.edit_text.editIndex == (ime.edit_text.preeditIndex + ime.edit_text.preeditLength))
        ime_reset_preedit(ime);
    else if (ime.caretIndex < ime.str.length()) {
        ime.caretIndex++;
        ime.event_id = SCE_IME_EVENT_UPDATE_CARET;
    }
    if (ime.edit_text.caretIndex && ime.caps_level != IME_CAPS_LOCK)
        ime.caps_level = IME_CAPS_NONE;
}

void ime_backspace(Ime &ime) {
    if (!ime.edit_text.caretIndex)
        return;
    ime.str.erase(ime.edit_text.caretIndex - 1, 1);
    ime.edit_text.editIndex = ime.edit_text.caretIndex;
    --ime.edit_text.caretIndex;
    if (ime.edit_text.preeditIndex > ime.edit_text.caretIndex)
        ime.edit_text.preeditIndex = ime.caretIndex = ime.edit_text.caretIndex;
    if (ime.edit_text.preeditLength)
        --ime.edit_text.editLengthChange;
    if (ime.edit_text.preeditLength)
        --ime.edit_text.preeditLength;
    ime.event_id = SCE_IME_EVENT_UPDATE_TEXT;
    if (ime.str.empty() && (ime.caps_level == IME_CAPS_NONE))
        ime.caps_level = IME_CAPS_SHIFT;
}

void ime_toggle_shift(Ime &ime) {
    if (ime.edit_text.caretIndex == 0)
        ime.caps_level = ime.caps_level == IME_CAPS_SHIFT ? IME_CAPS_NONE : ime.caps_level + 1;
    else
        ime.caps_level = ime.caps_level == IME_CAPS_LOCK ? IME_CAPS_NONE : ime.caps_level + 1;
}

std::vector<std::pair<SceImeLanguage, std::string>>::const_iterator
get_ime_lang_index(Ime &ime, SceImeLanguage lang) {
    return std::find_if(ime.lang.ime_keyboards.begin(), ime.lang.ime_keyboards.end(),
        [&](const auto &l) { return l.first == lang; });
}

void set_ime_default_layout(Ime &ime) {
    auto &layout = ime.layout;
    layout.keys = {
        { IME_ROW_FIRST, { u"q", u"w", u"e", u"r", u"t", u"y", u"u", u"i", u"o", u"p" } },
        { IME_ROW_SECOND, { u"a", u"s", u"d", u"f", u"g", u"h", u"j", u"k", u"l" } },
        { IME_ROW_THIRD, { u"z", u"x", u"c", u"v", u"b", u"n", u"m" } }
    };
    layout.shift_keys = {
        { IME_ROW_FIRST, { u"Q", u"W", u"E", u"R", u"T", u"Y", u"U", u"I", u"O", u"P" } },
        { IME_ROW_SECOND, { u"A", u"S", u"D", u"F", u"G", u"H", u"J", u"K", u"L" } },
        { IME_ROW_THIRD, { u"Z", u"X", u"C", u"V", u"B", u"N", u"M" } }
    };
}

void init_ime_lang(Ime &ime, SceImeLanguage lang) {
    auto &layout = ime.layout;
    layout.second_keyboard.clear();

    switch (lang) {
    case SCE_IME_LANGUAGE_SPANISH:
        layout.punct = { { IME_ROW_FIRST, { u",", u"\u00BF?" } }, { IME_ROW_SECOND, { u".", u"\u00A1!" } } };
        break;
    default:
        layout.punct = { { IME_ROW_FIRST, { u",", u"?" } }, { IME_ROW_SECOND, { u".", u"!" } } };
        break;
    }

    switch (lang) {
    case SCE_IME_LANGUAGE_DANISH: layout.space_str = "Mellemrum"; break;
    case SCE_IME_LANGUAGE_GERMAN: layout.space_str = "Leerstelle"; break;
    case SCE_IME_LANGUAGE_SPANISH: layout.space_str = "Espacio"; break;
    case SCE_IME_LANGUAGE_FRENCH: layout.space_str = "Espace"; break;
    case SCE_IME_LANGUAGE_ITALIAN: layout.space_str = "Spazio"; break;
    case SCE_IME_LANGUAGE_DUTCH: layout.space_str = "Spatie"; break;
    case SCE_IME_LANGUAGE_NORWEGIAN: layout.space_str = "Mellomrom"; break;
    case SCE_IME_LANGUAGE_POLISH: layout.space_str = "Spacja"; break;
    case SCE_IME_LANGUAGE_PORTUGUESE_BR:
    case SCE_IME_LANGUAGE_PORTUGUESE_PT:
        layout.space_str = reinterpret_cast<const char *>(u8"Espa\u00E7o");
        break;
    case SCE_IME_LANGUAGE_RUSSIAN:
        layout.space_str = reinterpret_cast<const char *>(u8"\u041F \u0440 \u043E \u0431 \u0435 \u043B");
        break;
    case SCE_IME_LANGUAGE_FINNISH:
        layout.space_str = reinterpret_cast<const char *>(u8"V\u00E4lily\u00F6nti");
        break;
    case SCE_IME_LANGUAGE_SWEDISH: layout.space_str = "Blanksteg"; break;
    case SCE_IME_LANGUAGE_TURKISH:
        layout.space_str = reinterpret_cast<const char *>(u8"Bo\u015Fluk");
        break;
    default:
        layout.space_str = "Space";
        break;
    }

    switch (lang) {
    case SCE_IME_LANGUAGE_GERMAN:
        layout.keys = {
            { IME_ROW_FIRST, { u"q", u"w", u"e", u"r", u"t", u"z", u"u", u"i", u"o", u"p" } },
            { IME_ROW_SECOND, { u"a", u"s", u"d", u"f", u"g", u"h", u"j", u"k", u"l" } },
            { IME_ROW_THIRD, { u"y", u"x", u"c", u"v", u"b", u"n", u"m" } }
        };
        layout.shift_keys = {
            { IME_ROW_FIRST, { u"Q", u"W", u"E", u"R", u"T", u"Z", u"U", u"I", u"O", u"P" } },
            { IME_ROW_SECOND, { u"A", u"S", u"D", u"F", u"G", u"H", u"J", u"K", u"L" } },
            { IME_ROW_THIRD, { u"Y", u"X", u"C", u"V", u"B", u"N", u"M" } }
        };
        break;
    case SCE_IME_LANGUAGE_FRENCH:
        layout.keys = {
            { IME_ROW_FIRST, { u"a", u"z", u"e", u"r", u"t", u"y", u"u", u"i", u"o", u"p" } },
            { IME_ROW_SECOND, { u"q", u"s", u"d", u"f", u"g", u"h", u"j", u"k", u"l", u"m" } },
            { IME_ROW_THIRD, { u"w", u"x", u"c", u"v", u"b", u"n" } }
        };
        layout.shift_keys = {
            { IME_ROW_FIRST, { u"A", u"Z", u"E", u"R", u"T", u"Y", u"U", u"I", u"O", u"P" } },
            { IME_ROW_SECOND, { u"Q", u"S", u"D", u"F", u"G", u"H", u"J", u"K", u"L", u"M" } },
            { IME_ROW_THIRD, { u"W", u"X", u"C", u"V", u"B", u"N" } }
        };
        break;
    case SCE_IME_LANGUAGE_RUSSIAN:
        layout.second_keyboard = {
            { IME_ROW_FIRST, "ABC" },
            { IME_ROW_SECOND, reinterpret_cast<const char *>(u8"\u0420\u0423") }
        };
        layout.keys = {
            { IME_ROW_FIRST, { u"\u0439", u"\u0446", u"\u0443", u"\u043A", u"\u0435", u"\u043D", u"\u0433", u"\u0448", u"\u0449", u"\u0437", u"\u0445", u"\u044A" } },
            { IME_ROW_SECOND, { u"\u0451", u"\u0444", u"\u044B", u"\u0432", u"\u0430", u"\u043F", u"\u0440", u"\u043E", u"\u043B", u"\u0434", u"\u0436", u"\u044D" } },
            { IME_ROW_THIRD, { u"\u044F", u"\u0447", u"\u0441", u"\u043C", u"\u0438", u"\u0442", u"\u044C", u"\u0431", u"\u044E" } }
        };
        layout.shift_keys = {
            { IME_ROW_FIRST, { u"\u0419", u"\u0426", u"\u0423", u"\u041A", u"\u0415", u"\u041D", u"\u0413", u"\u0428", u"\u0429", u"\u0417", u"\u0425", u"\u042A" } },
            { IME_ROW_SECOND, { u"\u0401", u"\u0424", u"\u042B", u"\u0412", u"\u0410", u"\u041F", u"\u0420", u"\u041E", u"\u041B", u"\u0414", u"\u0416", u"\u042D" } },
            { IME_ROW_THIRD, { u"\u042F", u"\u0427", u"\u0421", u"\u041C", u"\u0418", u"\u0422", u"\u042C", u"\u0411", u"\u042E" } }
        };
        break;
    default:
        layout.keys = {
            { IME_ROW_FIRST, { u"q", u"w", u"e", u"r", u"t", u"y", u"u", u"i", u"o", u"p" } },
            { IME_ROW_SECOND, { u"a", u"s", u"d", u"f", u"g", u"h", u"j", u"k", u"l" } },
            { IME_ROW_THIRD, { u"z", u"x", u"c", u"v", u"b", u"n", u"m" } }
        };
        layout.shift_keys = {
            { IME_ROW_FIRST, { u"Q", u"W", u"E", u"R", u"T", u"Y", u"U", u"I", u"O", u"P" } },
            { IME_ROW_SECOND, { u"A", u"S", u"D", u"F", u"G", u"H", u"J", u"K", u"L" } },
            { IME_ROW_THIRD, { u"Z", u"X", u"C", u"V", u"B", u"N", u"M" } }
        };
        break;
    }
}
