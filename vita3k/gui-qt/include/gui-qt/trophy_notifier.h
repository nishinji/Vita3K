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
#include <QPixmap>
#include <QString>

#include <vector>

struct EmuEnvState;
struct NpTrophyUnlockCallbackData;

struct TrophyUnlockData {
    QString np_com_id;
    int trophy_id = 0;
    QString name;
    QString detail;
    int grade = 0;
    std::vector<uint8_t> icon_buf;
};

Q_DECLARE_METATYPE(TrophyUnlockData)

class TrophyNotifier : public QObject {
    Q_OBJECT

public:
    explicit TrophyNotifier(QObject *parent = nullptr);

    void install(EmuEnvState &emuenv);

Q_SIGNALS:
    void trophy_unlocked(TrophyUnlockData data);

private:
    void on_unlock(NpTrophyUnlockCallbackData &data);
};