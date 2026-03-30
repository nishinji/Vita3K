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

#include <QObject>
#include <QStringLiteral>

enum class GameListColumn : int {
    Icon = 0,
    Title,
    TitleId,
    Version,
    Category,
    CompatStatus,
    LastPlayed,
    PlayTime,
    Count
};

static constexpr int GAME_LIST_COLUMN_COUNT = static_cast<int>(GameListColumn::Count);

inline QStringList game_list_column_labels() {
    return {
        QStringLiteral("Icon"),
        QObject::tr("Title"),
        QObject::tr("Title ID"),
        QObject::tr("Version"),
        QObject::tr("Category"),
        QObject::tr("Compatibility"),
        QObject::tr("Last Played"),
        QObject::tr("Time Played"),
    };
}