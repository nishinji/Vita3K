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

#include <app/state.h>

#include <QMenu>

#include <vector>

struct EmuEnvState;
class GameCompatibility;

class GameListContextMenu : public QMenu {
    Q_OBJECT

public:
    explicit GameListContextMenu(EmuEnvState &emuenv,
        const std::vector<const app::Game *> &games,
        GameCompatibility *compat,
        bool game_running,
        QWidget *parent = nullptr);

Q_SIGNALS:
    void boot_requested(const app::Game &game);
    void open_settings_requested(const std::string &app_path, int tab);
    void refresh_requested();

private:
    void build_single(const app::Game &game);
    void build_multi(const std::vector<const app::Game *> &games);

    void add_boot_actions(const app::Game &game);
    void add_compat_actions(const app::Game &game);
    void add_copy_info_actions(const app::Game &game);
    void add_custom_config_actions(const app::Game &game);
    void add_open_folder_actions(const app::Game &game);
    void add_delete_actions(const app::Game &game);
    void add_misc_actions(const app::Game &game);
    void add_information_actions(const app::Game &game);

    EmuEnvState &m_emuenv;
    GameCompatibility *m_compat;
    bool m_game_running;
};
