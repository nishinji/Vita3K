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

#include <gui-qt/trophy_notifier.h>

#include <emuenv/state.h>
#include <np/state.h>

#include <QMetaObject>
#include <QObject>

TrophyNotifier::TrophyNotifier(QObject *parent)
    : QObject(parent) {
    qRegisterMetaType<TrophyUnlockData>();
}

void TrophyNotifier::install(EmuEnvState &emuenv) {
    emuenv.np.trophy_state.trophy_unlock_callback = [this](NpTrophyUnlockCallbackData &cb) {
        TrophyUnlockData data;
        data.np_com_id = QString::fromStdString(cb.np_com_id);
        data.trophy_id = std::stoi(cb.trophy_id);
        data.name = QString::fromStdString(cb.trophy_name);
        data.detail = QString::fromStdString(cb.trophy_detail);
        data.grade = static_cast<int>(cb.trophy_kind);
        data.icon_buf = cb.icon_buf;

        QMetaObject::invokeMethod(this, [this, d = std::move(data)]() mutable { emit trophy_unlocked(std::move(d)); }, Qt::QueuedConnection);
    };
}