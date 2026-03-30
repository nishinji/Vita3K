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

#include <config/settings.h>

#include <config/functions.h>

#include <util/log.h>
#include <util/vector_utils.h>

#include <pugixml.hpp>

#include <algorithm>

namespace config {

static fs::path get_custom_config_path(const fs::path &config_path, const std::string &app_path) {
    return config_path / "config" / fmt::format("config_{}.xml", app_path);
}

bool load_custom_config(Config::CurrentConfig &out, const fs::path &config_path, const std::string &app_path) {
    if (app_path.empty())
        return false;

    const auto custom_cfg_path = get_custom_config_path(config_path, app_path);
    if (!fs::exists(custom_cfg_path))
        return false;

    pugi::xml_document doc;
    if (!doc.load_file(custom_cfg_path.c_str()) || doc.child("config").empty()) {
        LOG_ERROR("Custom config at {} is corrupted or invalid.", custom_cfg_path.string());
        fs::remove(custom_cfg_path);
        return false;
    }

    out = {};
    const auto config_child = doc.child("config");

    if (!config_child.child("core").empty()) {
        const auto core = config_child.child("core");
        out.modules_mode = core.attribute("modules-mode").as_int();
        out.lle_modules.clear();
        for (const auto &m : core.child("lle-modules"))
            out.lle_modules.emplace_back(m.text().as_string());
    }

    if (!config_child.child("cpu").empty())
        out.cpu_opt = config_child.child("cpu").attribute("cpu-opt").as_bool();

    if (!config_child.child("gpu").empty()) {
        const auto gpu = config_child.child("gpu");
        out.backend_renderer = gpu.attribute("backend-renderer").as_string();
#ifdef __ANDROID__
        out.custom_driver_name = gpu.attribute("custom-driver-name").as_string();
#endif
        out.high_accuracy = gpu.attribute("high-accuracy").as_bool();
        out.resolution_multiplier = gpu.attribute("resolution-multiplier").as_float();
        out.disable_surface_sync = gpu.attribute("disable-surface-sync").as_bool();
        out.screen_filter = gpu.attribute("screen-filter").as_string();
        out.memory_mapping = gpu.attribute("memory-mapping").as_string();
        out.v_sync = gpu.attribute("v-sync").as_bool();
        out.anisotropic_filtering = gpu.attribute("anisotropic-filtering").as_int();
        out.async_pipeline_compilation = gpu.attribute("async-pipeline-compilation").as_bool();
        out.import_textures = gpu.attribute("import-textures").as_bool();
        out.export_textures = gpu.attribute("export-textures").as_bool();
        out.export_as_png = gpu.attribute("export-as-png").as_bool();
        out.fps_hack = gpu.attribute("fps-hack").as_bool();
    }

    if (!config_child.child("audio").empty()) {
        const auto audio = config_child.child("audio");
        out.audio_backend = audio.attribute("audio-backend").as_string();
        out.audio_volume = audio.attribute("audio-volume").as_int();
        out.ngs_enable = audio.attribute("enable-ngs").as_bool();
    }

    if (!config_child.child("system").empty())
        out.pstv_mode = config_child.child("system").attribute("pstv-mode").as_bool();

    if (!config_child.child("emulator").empty()) {
        const auto emu = config_child.child("emulator");
        out.show_touchpad_cursor = emu.attribute("show-touchpad-cursor").as_bool();
        out.file_loading_delay = emu.attribute("file-loading-delay").as_int();
        out.stretch_the_display_area = emu.attribute("stretch-the-display-area").as_bool();
        out.fullscreen_hd_res_pixel_perfect = emu.attribute("fullscreen-hd-res-pixel-perfect").as_bool();
    }

    if (!config_child.child("network").empty())
        out.psn_signed_in = config_child.child("network").attribute("psn-signed-in").as_bool();

    return true;
}

bool save_custom_config(const Config::CurrentConfig &cc, const fs::path &config_path, const std::string &app_path) {
    if (app_path.empty())
        return false;

    const auto dir = config_path / "config";
    fs::create_directories(dir);

    pugi::xml_document doc;
    auto decl = doc.append_child(pugi::node_declaration);
    decl.append_attribute("version") = "1.0";
    decl.append_attribute("encoding") = "utf-8";

    auto config_child = doc.append_child("config");

    auto core_child = config_child.append_child("core");
    core_child.append_attribute("modules-mode") = cc.modules_mode;
    auto lle_child = core_child.append_child("lle-modules");
    for (const auto &m : cc.lle_modules)
        lle_child.append_child("module").append_child(pugi::node_pcdata).set_value(m.c_str());

    auto cpu_child = config_child.append_child("cpu");
    cpu_child.append_attribute("cpu-opt") = cc.cpu_opt;

    auto gpu_child = config_child.append_child("gpu");
    gpu_child.append_attribute("backend-renderer") = cc.backend_renderer.c_str();
#ifdef __ANDROID__
    gpu_child.append_attribute("custom-driver-name") = cc.custom_driver_name.c_str();
#endif
    gpu_child.append_attribute("high-accuracy") = cc.high_accuracy;
    gpu_child.append_attribute("resolution-multiplier") = cc.resolution_multiplier;
    gpu_child.append_attribute("disable-surface-sync") = cc.disable_surface_sync;
    gpu_child.append_attribute("screen-filter") = cc.screen_filter.c_str();
    gpu_child.append_attribute("memory-mapping") = cc.memory_mapping.c_str();
    gpu_child.append_attribute("v-sync") = cc.v_sync;
    gpu_child.append_attribute("anisotropic-filtering") = cc.anisotropic_filtering;
    gpu_child.append_attribute("async-pipeline-compilation") = cc.async_pipeline_compilation;
    gpu_child.append_attribute("import-textures") = cc.import_textures;
    gpu_child.append_attribute("export-textures") = cc.export_textures;
    gpu_child.append_attribute("export-as-png") = cc.export_as_png;
    gpu_child.append_attribute("fps-hack") = cc.fps_hack;

    auto audio_child = config_child.append_child("audio");
    audio_child.append_attribute("audio-backend") = cc.audio_backend.c_str();
    audio_child.append_attribute("audio-volume") = cc.audio_volume;
    audio_child.append_attribute("enable-ngs") = cc.ngs_enable;

    auto system_child = config_child.append_child("system");
    system_child.append_attribute("pstv-mode") = cc.pstv_mode;

    auto emu_child = config_child.append_child("emulator");
    emu_child.append_attribute("show-touchpad-cursor") = cc.show_touchpad_cursor;
    emu_child.append_attribute("file-loading-delay") = cc.file_loading_delay;
    emu_child.append_attribute("stretch-the-display-area") = cc.stretch_the_display_area;
    emu_child.append_attribute("fullscreen-hd-res-pixel-perfect") = cc.fullscreen_hd_res_pixel_perfect;

    auto network_child = config_child.append_child("network");
    network_child.append_attribute("psn-signed-in") = cc.psn_signed_in;

    const auto custom_cfg_path = get_custom_config_path(config_path, app_path);
    if (!doc.save_file(custom_cfg_path.c_str())) {
        LOG_ERROR("Failed to save custom config xml for app path: {}", app_path);
        return false;
    }

    return true;
}

bool delete_custom_config(const fs::path &config_path, const std::string &app_path) {
    if (app_path.empty())
        return false;

    const auto custom_cfg_path = get_custom_config_path(config_path, app_path);
    if (fs::exists(custom_cfg_path)) {
        fs::remove(custom_cfg_path);
        return true;
    }
    return false;
}

bool has_custom_config(const fs::path &config_path, const std::string &app_path) {
    if (app_path.empty())
        return false;
    return fs::exists(get_custom_config_path(config_path, app_path));
}

void set_current_config(Config &cfg, const fs::path &config_path, const std::string &app_path) {
    auto &cc = cfg.current_config;

    if (!app_path.empty() && load_custom_config(cc, config_path, app_path))
        return;

    cc.cpu_opt = cfg.cpu_opt;
    cc.modules_mode = cfg.modules_mode;
    cc.lle_modules = cfg.lle_modules;
    cc.backend_renderer = cfg.backend_renderer;
#ifdef __ANDROID__
    cc.custom_driver_name = cfg.custom_driver_name;
#endif
    cc.high_accuracy = cfg.high_accuracy;
    cc.resolution_multiplier = cfg.resolution_multiplier;
    cc.disable_surface_sync = cfg.disable_surface_sync;
    cc.screen_filter = cfg.screen_filter;
    cc.memory_mapping = cfg.memory_mapping;
    cc.v_sync = cfg.v_sync;
    cc.anisotropic_filtering = cfg.anisotropic_filtering;
    cc.async_pipeline_compilation = cfg.async_pipeline_compilation;
    cc.import_textures = cfg.import_textures;
    cc.export_textures = cfg.export_textures;
    cc.export_as_png = cfg.export_as_png;
    cc.fps_hack = cfg.fps_hack;
    cc.audio_backend = cfg.audio_backend;
    cc.audio_volume = cfg.audio_volume;
    cc.ngs_enable = cfg.ngs_enable;
    cc.pstv_mode = cfg.pstv_mode;
    cc.stretch_the_display_area = cfg.stretch_the_display_area;
    cc.fullscreen_hd_res_pixel_perfect = cfg.fullscreen_hd_res_pixel_perfect;
    cc.file_loading_delay = cfg.file_loading_delay;
    cc.psn_signed_in = cfg.psn_signed_in;
}

void copy_current_config_to_global(Config &cfg) {
    const auto &cc = cfg.current_config;
    cfg.cpu_opt = cc.cpu_opt;
    cfg.modules_mode = cc.modules_mode;
    cfg.lle_modules = cc.lle_modules;
    cfg.backend_renderer = cc.backend_renderer;
#ifdef __ANDROID__
    cfg.custom_driver_name = cc.custom_driver_name;
#endif
    cfg.high_accuracy = cc.high_accuracy;
    cfg.resolution_multiplier = cc.resolution_multiplier;
    cfg.disable_surface_sync = cc.disable_surface_sync;
    cfg.screen_filter = cc.screen_filter;
    cfg.memory_mapping = cc.memory_mapping;
    cfg.v_sync = cc.v_sync;
    cfg.anisotropic_filtering = cc.anisotropic_filtering;
    cfg.async_pipeline_compilation = cc.async_pipeline_compilation;
    cfg.import_textures = cc.import_textures;
    cfg.export_textures = cc.export_textures;
    cfg.export_as_png = cc.export_as_png;
    cfg.fps_hack = cc.fps_hack;
    cfg.audio_backend = cc.audio_backend;
    cfg.audio_volume = cc.audio_volume;
    cfg.ngs_enable = cc.ngs_enable;
    cfg.pstv_mode = cc.pstv_mode;
    cfg.stretch_the_display_area = cc.stretch_the_display_area;
    cfg.fullscreen_hd_res_pixel_perfect = cc.fullscreen_hd_res_pixel_perfect;
    cfg.file_loading_delay = cc.file_loading_delay;
    cfg.psn_signed_in = cc.psn_signed_in;
}

void save_current_config(Config &cfg, const fs::path &config_path, const std::string &app_path) {
    if (!app_path.empty() && has_custom_config(config_path, app_path)) {
        save_custom_config(cfg.current_config, config_path, app_path);
    } else {
        copy_current_config_to_global(cfg);
    }
    serialize_config(cfg, cfg.config_path);
}

std::vector<std::pair<std::string, bool>> get_modules_list(
    const fs::path &pref_path,
    const std::vector<std::string> &lle_modules) {
    std::vector<std::pair<std::string, bool>> modules;

    const auto modules_path = pref_path / "vs0/sys/external/";
    if (fs::exists(modules_path) && !fs::is_empty(modules_path)) {
        for (const auto &entry : fs::directory_iterator(modules_path)) {
            if (entry.path().extension() == ".suprx")
                modules.emplace_back(entry.path().filename().replace_extension().string(), false);
        }

        for (auto &m : modules)
            m.second = vector_utils::contains(lle_modules, m.first);

        std::sort(modules.begin(), modules.end(), [](const auto &a, const auto &b) {
            if (a.second == b.second)
                return a.first < b.first;
            return a.second;
        });
    }

    return modules;
}

} // namespace config
