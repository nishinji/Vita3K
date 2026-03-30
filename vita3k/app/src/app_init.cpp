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

#include <audio/state.h>
#include <config/functions.h>
#include <config/settings.h>
#include <config/state.h>
#include <config/version.h>
#include <display/state.h>
#include <emuenv/state.h>
#include <io/functions.h>
#include <kernel/state.h>
#include <motion/state.h>
#include <ngs/state.h>
#include <renderer/state.h>

#include <util/fs.h>
#include <util/log.h>
#include <util/string_utils.h>

#if USE_DISCORD
#include <app/discord.h>
#endif

#include <gdbstub/functions.h>

#include <SDL3/SDL_filesystem.h>

#ifdef _WIN32
#include <dwmapi.h>
#endif

#ifdef __ANDROID__
#include <SDL3/SDL_hints.h>
#include <SDL3/SDL_system.h>
#endif

namespace app {
static void set_backend_renderer(EmuEnvState &emuenv, const std::string &backend_renderer) {
#ifndef __APPLE__
    emuenv.backend_renderer = (string_utils::toupper(backend_renderer) == "OPENGL")
        ? renderer::Backend::OpenGL
        : renderer::Backend::Vulkan;
#else
    emuenv.backend_renderer = renderer::Backend::Vulkan;
#endif
}

void set_current_config(EmuEnvState &emuenv, const std::string &app_path) {
    config::set_current_config(emuenv.cfg, emuenv.config_path, app_path);
    set_backend_renderer(emuenv, emuenv.cfg.current_config.backend_renderer);
    emuenv.audio.set_global_volume(emuenv.cfg.current_config.audio_volume / 100.f);
}

void init_paths(Root &root_paths) {
#ifdef __ANDROID__
    fs::path storage_path = fs::path(SDL_GetAndroidExternalStoragePath()) / "";
    fs::path vita_storage_path = storage_path / "vita/";

    root_paths.set_base_path(storage_path);

    // On Android, static assets are bundled inside the APK and accessed via SDL_IOFromFile.
    root_paths.set_static_assets_path({});

    root_paths.set_pref_path(vita_storage_path);
    root_paths.set_log_path(storage_path);
    root_paths.set_config_path(storage_path);
    root_paths.set_shared_path(storage_path);
    root_paths.set_cache_path(storage_path / "cache" / "");
    root_paths.set_patch_path(storage_path / "patch" / "");
#else
    auto sdl_base_path = SDL_GetBasePath();
    auto base_path = fs_utils::utf8_to_path(sdl_base_path);

    root_paths.set_base_path(base_path);
    root_paths.set_static_assets_path(base_path);

#if defined(__APPLE__)
    // On Apple platforms, base_path is "Contents/Resources/" inside the app bundle.
    // An extra parent_path is apparently needed because of the trailing slash.
    auto portable_path = base_path.parent_path().parent_path().parent_path().parent_path() / "portable" / "";
#else
    auto portable_path = base_path / "portable" / "";
#endif

    if (fs::is_directory(portable_path)) {
        // If a portable directory exists, use it for everything else.
        // Note that pref_path should not be the same as the other paths.
        root_paths.set_pref_path(portable_path / "fs" / "");
        root_paths.set_log_path(portable_path);
        root_paths.set_config_path(portable_path);
        root_paths.set_shared_path(portable_path);
        root_paths.set_cache_path(portable_path / "cache" / "");
        root_paths.set_patch_path(portable_path / "patch" / "");
    } else {
        // SDL_GetPrefPath is deferred as it creates the directory.
        // When using a portable directory, it is not needed.
        auto sdl_pref_path = SDL_GetPrefPath(org_name, app_name);
        auto pref_path = fs_utils::utf8_to_path(sdl_pref_path);
        SDL_free(sdl_pref_path);

#if defined(__APPLE__)
        // Store other data in the user-wide path. Otherwise we may end up dumping
        // files into the "/Applications/" install directory or the app bundle.
        // This will typically be "~/Library/Application Support/Vita3K/Vita3K/".
        // Check for config.yml first, though, to maintain backwards compatibility,
        // even though storing user data inside the app bundle is not a good idea.
        auto existing_config = base_path / "config.yml";
        if (!fs::exists(existing_config)) {
            base_path = pref_path;
        }

        // pref_path should not be the same as the other paths.
        // For backwards compatibility, though, check if ux0 exists first.
        auto existing_ux0 = pref_path / "ux0";
        if (!fs::is_directory(existing_ux0)) {
            pref_path = pref_path / "fs" / "";
        }
#endif

        root_paths.set_pref_path(pref_path);
        root_paths.set_log_path(base_path);
        root_paths.set_config_path(base_path);
        root_paths.set_shared_path(base_path);
        root_paths.set_cache_path(base_path / "cache" / "");
        root_paths.set_patch_path(base_path / "patch" / "");

#if defined(__linux__)
        // XDG Data Dirs.
        auto env_home = getenv("HOME");
        auto XDG_DATA_DIRS = getenv("XDG_DATA_DIRS");
        auto XDG_DATA_HOME = getenv("XDG_DATA_HOME");
        auto XDG_CACHE_HOME = getenv("XDG_CACHE_HOME");
        auto XDG_CONFIG_HOME = getenv("XDG_CONFIG_HOME");
        auto APPDIR = getenv("APPDIR"); // Used in AppImage

        if (XDG_DATA_HOME != NULL)
            root_paths.set_pref_path(fs::path(XDG_DATA_HOME) / app_name / app_name / "");

        if (XDG_CONFIG_HOME != NULL)
            root_paths.set_config_path(fs::path(XDG_CONFIG_HOME) / app_name / "");
        else if (env_home != NULL)
            root_paths.set_config_path(fs::path(env_home) / ".config" / app_name / "");

        if (XDG_CACHE_HOME != NULL) {
            root_paths.set_cache_path(fs::path(XDG_CACHE_HOME) / app_name / "");
            root_paths.set_log_path(fs::path(XDG_CACHE_HOME) / app_name / "");
        } else if (env_home != NULL) {
            root_paths.set_cache_path(fs::path(env_home) / ".cache" / app_name / "");
            root_paths.set_log_path(fs::path(env_home) / ".cache" / app_name / "");
        }

        // Don't assume that base_path is portable.
        if (fs::exists(root_paths.get_base_path() / "data") && fs::exists(root_paths.get_base_path() / "lang") && fs::exists(root_paths.get_base_path() / "shaders-builtin"))
            root_paths.set_static_assets_path(root_paths.get_base_path());
        else if (env_home != NULL)
            root_paths.set_static_assets_path(fs::path(env_home) / ".local/share" / app_name / "");

        if (XDG_DATA_DIRS != NULL) {
            auto env_paths = string_utils::split_string(XDG_DATA_DIRS, ':');
            for (auto &i : env_paths) {
                if (fs::exists(fs::path(i) / app_name)) {
                    root_paths.set_static_assets_path(fs::path(i) / app_name / "");
                    break;
                }
            }
        } else if (XDG_DATA_HOME != NULL) {
            if (fs::exists(fs::path(XDG_DATA_HOME) / app_name / "data") && fs::exists(fs::path(XDG_DATA_HOME) / app_name / "lang") && fs::exists(fs::path(XDG_DATA_HOME) / app_name / "shaders-builtin"))
                root_paths.set_static_assets_path(fs::path(XDG_DATA_HOME) / app_name / "");
        }

        if (APPDIR != NULL && fs::exists(fs::path(APPDIR) / "usr/share/Vita3K")) {
            root_paths.set_static_assets_path(fs::path(APPDIR) / "usr/share/Vita3K");
        }

        // shared path
        if (env_home != NULL)
            root_paths.set_shared_path(fs::path(env_home) / ".local/share" / app_name / "");

        if (XDG_DATA_HOME != NULL) {
            root_paths.set_shared_path(fs::path(XDG_DATA_HOME) / app_name / "");
        }

        // patch path should be in shared path
        root_paths.set_patch_path(root_paths.get_shared_path() / "patch" / "");
#endif
    }
#endif

    // Create default preference and cache path for safety
    fs::create_directories(root_paths.get_config_path());
    fs::create_directories(root_paths.get_cache_path());
    fs::create_directories(root_paths.get_log_path() / "shaderlog");
    fs::create_directories(root_paths.get_log_path() / "texturelog");
    fs::create_directories(root_paths.get_patch_path());
}

bool init(EmuEnvState &state, Config &cfg, const Root &root_paths) {
    state.cfg = std::move(cfg);

    state.base_path = root_paths.get_base_path();
    state.default_path = root_paths.get_pref_path();
    state.log_path = root_paths.get_log_path();
    state.config_path = root_paths.get_config_path();
    state.cache_path = root_paths.get_cache_path();
    state.shared_path = root_paths.get_shared_path();
    state.static_assets_path = root_paths.get_static_assets_path();
    state.patch_path = root_paths.get_patch_path();

    // If configuration does not provide a preference path, use SDL's default
    if (state.cfg.pref_path == root_paths.get_pref_path() || state.cfg.pref_path.empty()) {
        state.pref_path = root_paths.get_pref_path();
    } else {
        auto last_char = state.cfg.pref_path.back();
        if (last_char != fs::path::preferred_separator && last_char != '/')
            state.cfg.pref_path += fs::path::preferred_separator;
        state.pref_path = state.cfg.get_pref_path();
    }

    set_current_config(state, state.cfg.run_app_path.has_value() ? *state.cfg.run_app_path : "");

    LOG_INFO("backend-renderer: {}", state.cfg.current_config.backend_renderer);

    LOG_INFO("Base path: {}", state.base_path);
#if defined(__linux__) && !defined(__ANDROID__)
    LOG_INFO("Static assets path: {}", state.static_assets_path);
    LOG_INFO("Shared path: {}", state.shared_path);
    LOG_INFO("Log path: {}", state.log_path);
    LOG_INFO("User config path: {}", state.config_path);
    LOG_INFO("User cache path: {}", state.cache_path);
#endif
    LOG_INFO("User pref path: {}", state.pref_path);

    if (!init(state.io, state.cache_path, state.log_path, state.pref_path, state.cfg.console)) {
        LOG_ERROR("Failed to initialize file system for the emulator!");
        return false;
    }

#ifdef __ANDROID__
    state.renderer->current_custom_driver = state.cfg.custom_driver_name;
#endif

#if USE_DISCORD
    if (discordrpc::init() && state.cfg.discord_rich_presence) {
        discordrpc::update_presence();
    }
#endif

    state.motion.init();

    return true;
}

bool late_init(EmuEnvState &state) {
    // note: mem is not initialized yet but that's not an issue
    // the renderer is not using it yet, just storing it for later uses
    state.renderer->late_init(state.cfg, state.app_path, state.mem);

    const bool need_page_table = state.renderer->mapping_method == MappingMethod::PageTable || state.renderer->mapping_method == MappingMethod::NativeBuffer;
    if (!init(state.mem, need_page_table)) {
        LOG_ERROR("Failed to initialize memory for emulator state!");
        return false;
    }

    const ResumeAudioThread resume_thread = [&state](SceUID thread_id) {
        const auto thread = state.kernel.get_thread(thread_id);
        const std::lock_guard<std::mutex> lock(thread->mutex);
        if (thread->status == ThreadStatus::wait) {
            thread->update_status(ThreadStatus::run);
        }
    };
    if (!state.audio.init(resume_thread, state.cfg.audio_backend)) {
        LOG_WARN("Failed to initialize audio! Audio will not work.");
    }

    if (!ngs::init(state.ngs, state.mem)) {
        LOG_ERROR("Failed to initialize ngs.");
        return false;
    }

    return true;
}

void apply_renderer_config(EmuEnvState &emuenv) {
    auto &r = *emuenv.renderer;
    const auto &cc = emuenv.cfg.current_config;

    r.res_multiplier = cc.resolution_multiplier;
    r.set_surface_sync_state(cc.disable_surface_sync);
    r.set_screen_filter(cc.screen_filter);
    r.set_anisotropic_filtering(cc.anisotropic_filtering);
    r.set_stretch_display(emuenv.cfg.stretch_the_display_area);
    r.stretch_hd_pixel_perfect(emuenv.cfg.fullscreen_hd_res_pixel_perfect);
    r.set_async_compilation(cc.async_pipeline_compilation);
    emuenv.display.fps_hack = cc.fps_hack;
}

void destroy(EmuEnvState &emuenv) {
#ifdef USE_DISCORD
    discordrpc::shutdown();
#endif

    if (emuenv.cfg.gdbstub)
        server_close(emuenv);

    // There may be changes that made in the GUI, so we should save, again
    if (emuenv.cfg.overwrite_config)
        config::serialize_config(emuenv.cfg, emuenv.cfg.config_path);
}

void switch_state(EmuEnvState &emuenv, const bool pause) {
    if (pause) {
        emuenv.kernel.pause_threads();
    } else {
        emuenv.kernel.resume_threads();
    }
    emuenv.audio.switch_state(pause);
}

} // namespace app
