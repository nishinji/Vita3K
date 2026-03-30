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

#include <app/functions.h>
#include <app/state.h>
#include <config/state.h>
#include <emuenv/state.h>
#include <io/state.h>
#include <packages/sfo.h>
#include <util/fs.h>
#include <util/log.h>

#include <pugixml.hpp>

#include <algorithm>
#include <cstdint>
#include <ctime>
#include <fstream>
#include <mutex>
#include <vector>

static constexpr uint32_t CACHE_VERSION = 1;

namespace app {

namespace {

static std::string read_str(std::ifstream &s) {
    std::size_t len{};
    s.read(reinterpret_cast<char *>(&len), sizeof(len));
    std::vector<char> buf(len);
    s.read(buf.data(), static_cast<std::streamsize>(len));
    return std::string(buf.begin(), buf.end());
}

static void write_str(std::ofstream &s, const std::string &v) {
    const std::size_t len = v.size();
    s.write(reinterpret_cast<const char *>(&len), sizeof(len));
    s.write(v.c_str(), static_cast<std::streamsize>(len));
}

static void save_games_cache(std::ofstream &f, GameListState &state, uint32_t sys_lang) {
    const std::size_t count = state.games.size();
    f.write(reinterpret_cast<const char *>(&count), sizeof(count));
    f.write(reinterpret_cast<const char *>(&CACHE_VERSION), sizeof(CACHE_VERSION));
    f.write(reinterpret_cast<const char *>(&sys_lang), sizeof(sys_lang));

    for (const Game &g : state.games) {
        write_str(f, g.app_ver);
        write_str(f, g.category);
        write_str(f, g.content_id);
        write_str(f, g.addcont);
        write_str(f, g.savedata);
        write_str(f, g.parental_level);
        write_str(f, g.stitle);
        write_str(f, g.title);
        write_str(f, g.title_id);
        write_str(f, g.path);
        write_str(f, g.icon_path);
    }

    state.cache_lang = sys_lang;
}

static std::string get_icon_path(const EmuEnvState &emuenv, const std::string &title_id) {
    const auto rel = fs::path("ux0/app") / title_id / "sce_sys/icon0.png";
    if (fs::exists(emuenv.pref_path / rel))
        return rel.generic_string();

    const auto default_rel = fs::path("vs0/data/internal/common/default-icon.png");
    if (fs::exists(emuenv.pref_path / default_rel))
        return default_rel.generic_string();

    return {};
}

static std::vector<GameTime>::iterator find_game_time(
    GameListState &state,
    const std::string &user_id,
    const std::string &app_path) {
    auto &v = state.game_times[user_id];
    return std::find_if(v.begin(), v.end(), [&](const GameTime &t) {
        return t.app == app_path;
    });
}

} // namespace

bool init_game_list(EmuEnvState &emuenv) {
    if (!load_cached_games(emuenv))
        return scan_games(emuenv);
    return true;
}

bool scan_games(EmuEnvState &emuenv) {
    const fs::path app_path{ emuenv.pref_path / "ux0/app" };
    if (!fs::exists(app_path)) {
        return false;
    }

    std::vector<Game> scanned;
    for (const auto &entry : fs::directory_iterator(app_path)) {
        if (entry.path().empty() || !fs::is_directory(entry.path()) || entry.path().filename_is_dot() || entry.path().filename_is_dot_dot())
            continue;
        scanned.push_back(read_game_info(emuenv, entry.path().stem().generic_string()));
    }

    auto &state = emuenv.app.game_list;
    const uint32_t sys_lang = static_cast<uint32_t>(emuenv.cfg.sys_lang);

    const auto temp_path{ emuenv.pref_path / "ux0/temp" };
    fs::create_directories(temp_path);
    std::ofstream f((temp_path / "apps.dat").string(), std::ios::out | std::ios::binary);
    if (!f.is_open())
        LOG_ERROR("Failed to open game list cache for writing");

    {
        std::lock_guard<std::mutex> lock(state.mutex);
        state.games = std::move(scanned);
        if (f)
            save_games_cache(f, state, sys_lang);
    }

    if (f && !f)
        LOG_ERROR("Error writing game list cache");

    return true;
}

bool load_cached_games(EmuEnvState &emuenv) {
    const auto cache_path{ emuenv.pref_path / "ux0/temp/apps.dat" };

    std::ifstream f(cache_path.string(), std::ios::in | std::ios::binary);
    if (!f.is_open())
        return false;

    std::size_t count{};
    f.read(reinterpret_cast<char *>(&count), sizeof(count));

    uint32_t version{};
    f.read(reinterpret_cast<char *>(&version), sizeof(version));
    if (version != CACHE_VERSION) {
        LOG_WARN("Game cache version mismatch (found {}, expected {}); rebuilding.", version, CACHE_VERSION);
        return false;
    }

    uint32_t cache_lang{};
    f.read(reinterpret_cast<char *>(&cache_lang), sizeof(cache_lang));
    if (cache_lang != static_cast<uint32_t>(emuenv.cfg.sys_lang)) {
        LOG_WARN("Game cache lang {} differs from config {}; rebuilding.",
            cache_lang, static_cast<uint32_t>(emuenv.cfg.sys_lang));
        return false;
    }

    std::vector<Game> loaded;
    loaded.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        Game g;
        g.app_ver = read_str(f);
        g.category = read_str(f);
        g.content_id = read_str(f);
        g.addcont = read_str(f);
        g.savedata = read_str(f);
        g.parental_level = read_str(f);
        g.stitle = read_str(f);
        g.title = read_str(f);
        g.title_id = read_str(f);
        g.path = read_str(f);
        g.icon_path = read_str(f);
        loaded.push_back(std::move(g));
    }

    if (!f) {
        LOG_ERROR("Game cache read failed or truncated; rebuilding.");
        return false;
    }

    auto &state = emuenv.app.game_list;
    std::lock_guard<std::mutex> lock(state.mutex);
    state.games = std::move(loaded);
    state.cache_lang = cache_lang;

    return !state.games.empty();
}

void save_game_cache(EmuEnvState &emuenv) {
    const auto temp_path{ emuenv.pref_path / "ux0/temp" };
    fs::create_directories(temp_path);

    std::ofstream f((temp_path / "apps.dat").string(), std::ios::out | std::ios::binary);
    if (!f.is_open()) {
        LOG_ERROR("Failed to open {}", (temp_path / "apps.dat").string());
        return;
    }

    {
        auto &state = emuenv.app.game_list;
        std::lock_guard<std::mutex> lock(state.mutex);
        save_games_cache(f, state, static_cast<uint32_t>(emuenv.cfg.sys_lang));
    }

    if (!f)
        LOG_ERROR("Write error during game list cache save.");
}

Game read_game_info(EmuEnvState &emuenv, const std::string &title_id) {
    sfo::SfoAppInfo info;
    vfs::FileBuffer param;
    if (vfs::read_app_file(param, emuenv.pref_path, title_id, "sce_sys/param.sfo")) {
        sfo::get_param_info(info, param, emuenv.cfg.sys_lang);
    } else {
        info.app_title_id = title_id;
        info.app_addcont = title_id;
        info.app_savedata = title_id;
        info.app_short_title = title_id;
        info.app_title = title_id;
        info.app_version = "N/A";
        info.app_category = "N/A";
        info.app_parental_level = "N/A";
    }

    Game g;
    g.app_ver = info.app_version;
    g.category = info.app_category;
    g.content_id = info.app_content_id;
    g.addcont = info.app_addcont;
    g.savedata = info.app_savedata;
    g.parental_level = info.app_parental_level;
    g.stitle = info.app_short_title;
    g.title = info.app_title;
    g.title_id = info.app_title_id;
    g.path = title_id;
    g.icon_path = get_icon_path(emuenv, title_id);
    return g;
}

void load_time_games(EmuEnvState &emuenv) {
    auto &state = emuenv.app.game_list;
    std::lock_guard<std::mutex> lock(state.mutex);

    state.game_times.clear();
    const auto time_path{ emuenv.pref_path / "ux0/user/time.xml" };
    if (!fs::exists(time_path))
        return;

    pugi::xml_document doc;
    if (!doc.load_file(time_path.c_str())) {
        LOG_ERROR("time.xml is corrupted at {}; removing.", time_path.string());
        fs::remove(time_path);
        return;
    }

    const auto time_child = doc.child("time");
    if (time_child.child("user").empty())
        return;

    for (const auto &user : time_child) {
        const std::string user_id = user.attribute("id").as_string();
        for (const auto &app : user) {
            GameTime t;
            t.app = app.text().as_string();
            t.last_time_used = app.attribute("last-time-used").as_llong();
            t.time_used = app.attribute("time-used").as_llong();
            state.game_times[user_id].push_back(t);
        }
    }
}

void save_time_games(EmuEnvState &emuenv) {
    auto &state = emuenv.app.game_list;

    pugi::xml_document doc;
    auto decl = doc.append_child(pugi::node_declaration);
    decl.append_attribute("version") = "1.0";
    decl.append_attribute("encoding") = "utf-8";

    auto time_child = doc.append_child("time");

    for (auto &[user_id, games] : state.game_times) {
        std::sort(games.begin(), games.end(), [](const GameTime &a, const GameTime &b) {
            return a.last_time_used > b.last_time_used;
        });

        auto user_child = time_child.append_child("user");
        user_child.append_attribute("id") = user_id.c_str();

        for (const auto &t : games) {
            auto app_child = user_child.append_child("app");
            app_child.append_attribute("last-time-used") = t.last_time_used;
            app_child.append_attribute("time-used") = t.time_used;
            app_child.append_child(pugi::node_pcdata).set_value(t.app.c_str());
        }
    }

    const auto time_path{ emuenv.pref_path / "ux0/user/time.xml" };
    if (!doc.save_file(time_path.c_str()))
        LOG_ERROR("Failed to write {}", time_path.string());
}

void update_last_time_game_used(EmuEnvState &emuenv, const std::string &app_path) {
    auto &state = emuenv.app.game_list;
    std::lock_guard<std::mutex> lock(state.mutex);

    const std::string &user_id = emuenv.io.user_id;
    auto it = find_game_time(state, user_id, app_path);

    if (it != state.game_times[user_id].end()) {
        it->last_time_used = std::time(nullptr);
    } else {
        state.game_times[user_id].push_back({ app_path, std::time(nullptr), 0 });
    }

    save_time_games(emuenv);
}

void update_game_time_used(EmuEnvState &emuenv, const std::string &app_path) {
    auto &state = emuenv.app.game_list;
    std::lock_guard<std::mutex> lock(state.mutex);

    const std::string &user_id = emuenv.io.user_id;
    auto it = find_game_time(state, user_id, app_path);

    if (it == state.game_times[user_id].end()) {
        LOG_WARN("No time record for '{}'; skipping.", app_path);
        return;
    }

    const auto now = std::time(nullptr);
    it->time_used += now - it->last_time_used;
    it->last_time_used = now;

    save_time_games(emuenv);
}

void reset_last_time_game_used(EmuEnvState &emuenv, const std::string &app_path) {
    auto &state = emuenv.app.game_list;
    std::lock_guard<std::mutex> lock(state.mutex);

    const std::string &user_id = emuenv.io.user_id;
    auto it = find_game_time(state, user_id, app_path);

    if (it != state.game_times[user_id].end())
        it->last_time_used = 0;
    else
        state.game_times[user_id].push_back({ app_path, 0, 0 });

    save_time_games(emuenv);
}

void delete_game(EmuEnvState &emuenv, const std::string &app_path) {
    Game game;
    {
        auto &state = emuenv.app.game_list;
        std::lock_guard<std::mutex> lock(state.mutex);
        const auto it = std::find_if(state.games.begin(), state.games.end(),
            [&](const Game &g) { return g.path == app_path; });
        if (it == state.games.end()) {
            LOG_WARN("'{}' not found in game list.", app_path);
            return;
        }
        game = *it;
    }

    try {
        fs::remove_all(emuenv.pref_path / "ux0/app" / app_path);

        const auto custom_cfg{ emuenv.config_path / "config" / fmt::format("config_{}.xml", app_path) };
        if (fs::exists(custom_cfg))
            fs::remove_all(custom_cfg);

        const auto addcont{ emuenv.pref_path / "ux0/addcont" / game.addcont };
        if (fs::exists(addcont))
            fs::remove_all(addcont);

        const auto license{ emuenv.pref_path / "ux0/license" / game.title_id };
        if (fs::exists(license))
            fs::remove_all(license);

        const auto patch{ emuenv.pref_path / "ux0/patch" / game.title_id };
        if (fs::exists(patch))
            fs::remove_all(patch);

        const auto savedata{ emuenv.pref_path / "ux0/user" / emuenv.io.user_id / "savedata" / game.savedata };
        if (fs::exists(savedata))
            fs::remove_all(savedata);

        const auto shader_cache{ emuenv.cache_path / "shaders" / game.title_id };
        if (fs::exists(shader_cache))
            fs::remove_all(shader_cache);

        const auto shader_log{ emuenv.cache_path / "shaderlog" / game.title_id };
        if (fs::exists(shader_log))
            fs::remove_all(shader_log);

        const auto shader_log2{ emuenv.log_path / "shaderlog" / game.title_id };
        if (fs::exists(shader_log2))
            fs::remove_all(shader_log2);

        const auto export_tex{ emuenv.shared_path / "textures/export" / game.title_id };
        if (fs::exists(export_tex))
            fs::remove_all(export_tex);

        const auto import_tex{ emuenv.shared_path / "textures/import" / game.title_id };
        if (fs::exists(import_tex))
            fs::remove_all(import_tex);

        LOG_INFO("Game successfully deleted '{}' [{}].", game.title_id, game.title);
    } catch (const std::exception &e) {
        LOG_ERROR("Failed to delete '{}' [{}]: {}", game.title_id, game.title, e.what());
    }

    {
        auto &state = emuenv.app.game_list;
        std::lock_guard<std::mutex> lock(state.mutex);

        state.games.erase(
            std::remove_if(state.games.begin(), state.games.end(),
                [&](const Game &g) { return g.path == app_path; }),
            state.games.end());

        for (auto &[user_id, times] : state.game_times) {
            times.erase(
                std::remove_if(times.begin(), times.end(),
                    [&](const GameTime &t) { return t.app == app_path; }),
                times.end());
        }

        save_time_games(emuenv);

        const auto temp_path{ emuenv.pref_path / "ux0/temp" };
        fs::create_directories(temp_path);
        std::ofstream f((temp_path / "apps.dat").string(), std::ios::out | std::ios::binary);
        if (f.is_open())
            save_games_cache(f, state, static_cast<uint32_t>(emuenv.cfg.sys_lang));
        else
            LOG_ERROR("Failed to rewrite game cache");
    }
}

std::vector<Game> get_games(const EmuEnvState &emuenv) {
    const auto &state = emuenv.app.game_list;
    std::lock_guard<std::mutex> lock(state.mutex);
    return state.games;
}

std::map<std::string, GameTime> get_user_game_times(const EmuEnvState &emuenv) {
    const auto &state = emuenv.app.game_list;
    std::lock_guard<std::mutex> lock(state.mutex);
    std::map<std::string, GameTime> result;
    const auto it = state.game_times.find(emuenv.io.user_id);
    if (it != state.game_times.end()) {
        for (const auto &t : it->second)
            result[t.app] = t;
    }
    return result;
}

bool set_app_info(EmuEnvState &emuenv, const std::string &app_path) {
    const auto &state = emuenv.app.game_list;
    std::lock_guard<std::mutex> lock(state.mutex);

    const auto it = std::find_if(state.games.begin(), state.games.end(), [&](const Game &g) { return g.path == app_path; });

    if (it == state.games.end()) {
        LOG_ERROR("{} not found in game list.", app_path);
        return false;
    }

    emuenv.io.app_path = it->path;
    emuenv.io.title_id = it->title_id;
    emuenv.io.addcont = it->addcont;
    emuenv.io.content_id = it->content_id;
    emuenv.io.savedata = it->savedata;
    emuenv.current_app_title = it->title;
    emuenv.app_info.app_version = it->app_ver;
    emuenv.app_info.app_category = it->category;
    emuenv.app_info.app_short_title = it->stitle;

    return true;
}

} // namespace app