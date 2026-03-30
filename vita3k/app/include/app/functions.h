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

#include <string>

struct Config;
struct EmuEnvState;
class Root;

namespace app {

/// Describes the state of the application to be run
enum class AppRunType {
    /// Run type is unknown
    Unknown,
    /// Extracted, files are as they are on console
    Extracted,
};

void init_paths(Root &root_paths);
bool init(EmuEnvState &state, Config &cfg, const Root &root_paths);
bool late_init(EmuEnvState &state);
void apply_renderer_config(EmuEnvState &emuenv);
void set_current_config(EmuEnvState &emuenv, const std::string &app_path);
void destroy(EmuEnvState &emuenv);
void switch_state(EmuEnvState &emuenv, const bool pause);

bool init_game_list(EmuEnvState &emuenv);
bool scan_games(EmuEnvState &emuenv);
bool load_cached_games(EmuEnvState &emuenv);
void save_game_cache(EmuEnvState &emuenv);
Game read_game_info(EmuEnvState &emuenv, const std::string &title_id);
void load_time_games(EmuEnvState &emuenv);
void save_time_games(EmuEnvState &emuenv);
void update_last_time_game_used(EmuEnvState &emuenv, const std::string &app_path);
void update_game_time_used(EmuEnvState &emuenv, const std::string &app_path);
void reset_last_time_game_used(EmuEnvState &emuenv, const std::string &app_path);
void delete_game(EmuEnvState &emuenv, const std::string &app_path);
std::vector<Game> get_games(const EmuEnvState &emuenv);
std::map<std::string, GameTime> get_user_game_times(const EmuEnvState &emuenv);

bool set_app_info(EmuEnvState &emuenv, const std::string &app_path);
void reset_controller_binding(EmuEnvState &emuenv);

void load_users(EmuEnvState &emuenv);
void save_user(EmuEnvState &emuenv, const std::string &user_id);
std::string create_user(EmuEnvState &emuenv, const std::string &name);
void delete_user(EmuEnvState &emuenv, const std::string &user_id);
bool activate_user(EmuEnvState &emuenv, const std::string &user_id);

#ifdef __ANDROID__
void add_custom_driver(EmuEnvState &emuenv);
void remove_custom_driver(EmuEnvState &emuenv, const std::string &driver);
#endif

} // namespace app
