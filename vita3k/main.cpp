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

#include "archive.h"
#include "interface.h"

#include <app/functions.h>
#include <config/functions.h>
#include <config/version.h>
#include <emuenv/state.h>
#include <gui-qt/gui_settings.h>
#include <gui-qt/log_widget.h>
#include <gui-qt/main_window.h>
#include <gui-qt/persistent_settings.h>
#include <gui-qt/welcome_dialog.h>
#include <include/cpu.h>
#include <include/environment.h>
#include <io/state.h>
#include <modules/module_parent.h>
#include <packages/functions.h>
#include <packages/license.h>
#include <packages/pkg.h>
#include <packages/sfo.h>
#include <shader/spirv_recompiler.h>
#include <util/log.h>
#include <util/string_utils.h>

#include <QApplication>
#include <QMessageBox>

#if USE_DISCORD
#include <app/discord.h>
#endif

#ifdef _WIN32
#include <combaseapi.h>
#include <process.h>
#define SDL_MAIN_HANDLED
#endif

#ifdef TRACY_ENABLE
#include <tracy/Tracy.hpp>
#endif

#ifdef __ANDROID__
#include <SDL3/SDL_system.h>
#include <jni.h>
#include <unistd.h>
#include <xxh3.h>
#endif

#include <SDL3/SDL_cpuinfo.h>
#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_hints.h>
#include <SDL3/SDL_init.h>
#include <SDL3/SDL_main.h>

#include <cstdlib>
#include <optional>

#ifdef __ANDROID__
static void set_current_game_id(const std::string_view game_id) {
    // retrieve the JNI environment.
    JNIEnv *env = reinterpret_cast<JNIEnv *>(SDL_GetAndroidJNIEnv());

    // retrieve the Java instance of the SDLActivity
    jobject activity = reinterpret_cast<jobject>(SDL_GetAndroidActivity());

    // find the Java class of the activity. It should be SDLActivity or a subclass of it.
    jclass clazz(env->GetObjectClass(activity));

    // find the identifier of the method to call
    jmethodID method_id = env->GetMethodID(clazz, "setCurrentGameId", "(Ljava/lang/String;)V");
    jstring j_game_id = env->NewStringUTF(game_id.data());
    env->CallVoidMethod(activity, method_id, j_game_id);

    // clean up the local references.
    env->DeleteLocalRef(j_game_id);
    env->DeleteLocalRef(activity);
    env->DeleteLocalRef(clazz);
}

static void run_execv(char *argv[], EmuEnvState &emuenv) {
    // retrieve the JNI environment.
    JNIEnv *env = reinterpret_cast<JNIEnv *>(SDL_GetAndroidJNIEnv());

    // retrieve the Java instance of the SDLActivity
    jobject activity = reinterpret_cast<jobject>(SDL_GetAndroidActivity());

    // find the Java class of the activity. It should be SDLActivity or a subclass of it.
    jclass clazz(env->GetObjectClass(activity));

    // find the identifier of the method to call
    jmethodID method_id = env->GetMethodID(clazz, "restartApp", "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)V");

    // create the java string for the different parameters
    jstring app_path = env->NewStringUTF(emuenv.load_app_path.c_str());
    jstring exec_path = env->NewStringUTF(emuenv.load_exec_path.c_str());
    jstring exec_args = env->NewStringUTF(emuenv.load_exec_argv.c_str());

    env->CallVoidMethod(activity, method_id, app_path, exec_path, exec_args);

    // The function call above will exit with some delay
    // Exit now to match the behavior on PC
    exit(0);
};
#else
static void run_execv(char *argv[], EmuEnvState &emuenv) {
    char const *args[10];
    args[0] = argv[0];
    args[1] = "-a";
    args[2] = "true";
    if (!emuenv.load_app_path.empty()) {
        args[3] = "-r";
        args[4] = emuenv.load_app_path.data();
        if (!emuenv.load_exec_path.empty()) {
            args[5] = "--self";
            args[6] = emuenv.load_exec_path.data();
            if (!emuenv.load_exec_argv.empty()) {
                args[7] = "--app-args";
                args[8] = emuenv.load_exec_argv.data();
                args[9] = nullptr;
            } else
                args[7] = nullptr;
        } else
            args[5] = nullptr;
    } else {
        args[3] = nullptr;
    }

    // Execute the emulator again with some arguments
#ifdef _WIN32
    FreeConsole();
    _execv(argv[0], args);
#elif defined(__unix__) || defined(__APPLE__) && defined(__MACH__)
    execv(argv[0], const_cast<char *const *>(args));
#endif
}
#endif

int main(int argc, char *argv[]) {
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("Vita3K"));
    QCoreApplication::setApplicationName(QStringLiteral("Vita3K"));

#ifdef TRACY_ENABLE
    ZoneScoped; // Tracy - Track main function scope
#endif

    Root root_paths;
    app::init_paths(root_paths);

    if (!fs::exists(root_paths.get_pref_path())) {
        fs::create_directories(root_paths.get_pref_path());
    }

    LogWidget::register_callback();
    if (logging::init(root_paths, true) != Success) {
        return InitConfigFailed;
    }

#ifdef __ANDROID__
    setvbuf(stdout, 0, _IOLBF, 0);
    setvbuf(stderr, 0, _IONBF, 0);
    int pfd[2];
    pipe(pfd);
    dup2(pfd[1], 1);
    dup2(pfd[1], 2);
    std::thread cout_thread([&pfd]() {
        ssize_t rdsz;
        char buf[512];
        while ((rdsz = read(pfd[0], buf, sizeof buf - 1)) > 0) {
            if (buf[rdsz - 1] == '\n')
                --rdsz;
            buf[rdsz] = 0; /* add null-terminator */
            LOG_DEBUG("{}", buf);
        }
    });
    cout_thread.detach();
#endif

    // Check admin privs before init starts to avoid creating of file as other user by accident
    bool admin_priv = false;
#ifdef _WIN32
    // https://stackoverflow.com/questions/8046097/how-to-check-if-a-process-has-the-administrative-rights
    HANDLE hToken = NULL;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hToken)) {
        TOKEN_ELEVATION Elevation;
        DWORD cbSize = sizeof(TOKEN_ELEVATION);
        if (GetTokenInformation(hToken, TokenElevation, &Elevation, sizeof(Elevation), &cbSize)) {
            admin_priv = Elevation.TokenIsElevated;
        }
    }
    if (hToken) {
        CloseHandle(hToken);
    }
#else
    auto uid = getuid();
    auto euid = geteuid();

    // if either effective uid or uid is the one of the root user assume running as root.
    // else if euid and uid are different then permissions errors can happen if its running
    // as a completely different user than the uid/euid
    if (uid == 0 || euid == 0 || uid != euid)
        admin_priv = true;
#endif

    if (admin_priv) {
        LOG_CRITICAL("PLEASE. DO NOT RUN VITA3K AS ADMIN OR WITH ADMIN PRIVILEGES.");
    }

    Config cfg{};
    EmuEnvState emuenv;
    const auto config_err = config::init_config(cfg, argc, argv, root_paths);

    fs::create_directories(cfg.get_pref_path());

    if (config_err != Success) {
        if (config_err == QuitRequested) {
            if (cfg.recompile_shader_path.has_value()) {
                LOG_INFO("Recompiling {}", *cfg.recompile_shader_path);
                shader::convert_gxp_to_glsl_from_filepath(*cfg.recompile_shader_path);
            }
            if (cfg.delete_title_id.has_value()) {
                LOG_INFO("Deleting title id {}", *cfg.delete_title_id);
                fs::remove_all(cfg.get_pref_path() / "ux0/app" / *cfg.delete_title_id);
                fs::remove_all(cfg.get_pref_path() / "ux0/addcont" / *cfg.delete_title_id);
                fs::remove_all(cfg.get_pref_path() / "ux0/user/00/savedata" / *cfg.delete_title_id);
                fs::remove_all(root_paths.get_cache_path() / "shaders" / *cfg.delete_title_id);
            }
            if (cfg.pup_path.has_value()) {
                LOG_INFO("Installing firmware file {}", *cfg.pup_path);
                install_pup(cfg.get_pref_path(), *cfg.pup_path, [](uint32_t progress) {
                    LOG_INFO("Firmware installation progress: {}%", progress);
                });
            }
            if (cfg.pkg_path.has_value() && cfg.pkg_zrif.has_value()) {
                LOG_INFO("Installing pkg from {} ", *cfg.pkg_path);
                emuenv.cache_path = root_paths.get_cache_path().generic_path();
                emuenv.pref_path = cfg.get_pref_path();
                auto pkg_path = fs_utils::utf8_to_path(*cfg.pkg_path);
                install_pkg(pkg_path, emuenv, *cfg.pkg_zrif, [](float) {});
            }
            return Success;
        }
        LOG_ERROR("Failed to initialise config");
        return InitConfigFailed;
    }

#ifdef _WIN32
    {
        auto res = CoInitializeEx(NULL, COINIT_MULTITHREADED);
        LOG_ERROR_IF(res == S_FALSE, "Failed to initialize COM Library");
    }
#endif

    if (cfg.console) {
        if (logging::init(root_paths, false) != Success)
            return InitConfigFailed;
    } else {
        std::atexit(SDL_Quit);

        // Joystick events on background thread
        SDL_SetHint(SDL_HINT_JOYSTICK_THREAD, "1");
        // Enable HIDAPI rumble for DS4/DS
        SDL_SetHint(SDL_HINT_JOYSTICK_ENHANCED_REPORTS, "1");
        // Enable Switch controller
        SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_SWITCH, "1");
        SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_JOY_CONS, "1");

#ifdef __ANDROID__
        // The Audio driver (used by default) is really really bad
        SDL_SetHint(SDL_HINT_AUDIO_DRIVER, "openslES");
#endif

        if (!SDL_Init(SDL_INIT_AUDIO | SDL_INIT_GAMEPAD | SDL_INIT_HAPTIC | SDL_INIT_SENSOR | SDL_INIT_CAMERA)) {
            LOG_ERROR("SDL initialisation failed: {}", SDL_GetError());
            QMessageBox::critical(nullptr, "Error", "SDL initialisation failed.");
            return SDLInitFailed;
        }
    }

    LOG_INFO("{}", window_title);
    LOG_INFO("OS: {}", CppCommon::Environment::OSVersion());
    LOG_INFO("CPU: {} | {} Threads | {} GHz", CppCommon::CPU::Architecture(), CppCommon::CPU::LogicalCores(), static_cast<float>(CppCommon::CPU::ClockSpeed()) / 1000.f);
    LOG_INFO("Available ram memory: {} MiB", SDL_GetSystemRAM());

    app::AppRunType run_type = app::AppRunType::Unknown;
    if (cfg.run_app_path)
        run_type = app::AppRunType::Extracted;

    if (!app::init(emuenv, cfg, root_paths)) {
        QMessageBox::critical(nullptr, "Error", "Emulated environment initialization failed.");
        return 1;
    }

    if (emuenv.cfg.controller_binds.empty() || (emuenv.cfg.controller_binds.size() != 15)
        || emuenv.cfg.controller_axis_binds.empty() || (emuenv.cfg.controller_axis_binds.size() != 6))
        app::reset_controller_binding(emuenv);

    init_libraries(emuenv);

    if (!app::init_game_list(emuenv)) {
        LOG_ERROR("Failed to initialize apps list.");
        return 1;
    }

    app::load_users(emuenv);

    if (cfg.content_path.has_value()) {
        const auto extension = string_utils::tolower(cfg.content_path->extension().string());
        const auto is_archive = (extension == ".vpk") || (extension == ".zip");
        const auto is_rif = (extension == ".rif") || (cfg.content_path->filename() == "work.bin");
        const auto is_directory = fs::is_directory(*cfg.content_path);

        std::string boot_title_id;

        if (is_archive) {
            LOG_INFO("Installing archive from CLI: {}", cfg.content_path->string());
            std::vector<ContentInfo> contents_info = install_archive(emuenv, *cfg.content_path);
            const auto content_index = std::find_if(contents_info.begin(), contents_info.end(), [](const ContentInfo &c) {
                return c.category == "gd";
            });
            if (content_index != contents_info.end() && content_index->state)
                boot_title_id = content_index->title_id;
        } else if (is_directory) {
            LOG_INFO("Installing contents from CLI: {}", cfg.content_path->string());
            if (install_contents(emuenv, *cfg.content_path) == 1 && emuenv.app_info.app_category == "gd")
                boot_title_id = emuenv.app_info.app_title_id;
        } else if (is_rif) {
            LOG_INFO("Installing license from CLI: {}", cfg.content_path->string());
            copy_license(emuenv, *cfg.content_path);
        } else {
            LOG_ERROR("File: [{}] is not a supported content type.", cfg.content_path->string());
        }

        cfg.content_path.reset();

        if (!boot_title_id.empty()) {
            cfg.run_app_path = boot_title_id;
            LOG_INFO("Content installed, will auto-boot: {}", boot_title_id);
        }

        if (!app::init_game_list(emuenv)) {
            LOG_ERROR("Failed to refresh apps list after content install.");
        }
    }

    app.setStyle("fusion");

    const QString gui_configs_dir = QString::fromStdString((emuenv.config_path / "gui-configs").string());
    auto gui_settings = std::make_shared<GuiSettings>(gui_configs_dir);
    auto persistent_settings = std::make_shared<PersistentSettings>(gui_configs_dir);

    if (emuenv.cfg.show_welcome) {
        WelcomeDialog welcome(emuenv, false);
        welcome.exec();
    }

    MainWindow mainwindow(emuenv, std::move(gui_settings), std::move(persistent_settings));
    mainwindow.show();
    app.exec();

#ifdef _WIN32
    CoUninitialize();
#endif

    return Success;
}
