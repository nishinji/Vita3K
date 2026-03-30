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

#include "ui_main_window.h"

#include <gui-qt/about_dialog.h>
#include <gui-qt/archive_install_dialog.h>
#include <gui-qt/controls_dialog.h>
#include <gui-qt/ctrl_keyboard_filter.h>
#include <gui-qt/firmware_install_dialog.h>
#include <gui-qt/game_compatibility.h>
#include <gui-qt/game_list.h>
#include <gui-qt/game_list_context_menu.h>
#include <gui-qt/game_overlay.h>
#include <gui-qt/game_window.h>
#include <gui-qt/gui_settings.h>
#include <gui-qt/license_install_dialog.h>
#include <gui-qt/log_widget.h>
#include <gui-qt/main_window.h>
#include <gui-qt/persistent_settings.h>
#include <gui-qt/pkg_install_dialog.h>
#include <gui-qt/qt_utils.h>
#include <gui-qt/settings_dialog.h>
#include <gui-qt/trophy_collection_dialog.h>
#include <gui-qt/trophy_notifier.h>
#include <gui-qt/trophy_toast.h>
#include <gui-qt/user_management_dialog.h>
#include <gui-qt/welcome_dialog.h>

#include <app/functions.h>
#include <archive.h>
#include <audio/state.h>
#include <config/functions.h>
#include <config/settings.h>
#include <config/state.h>
#include <config/version.h>
#include <ctrl/functions.h>
#include <display/state.h>
#include <gxm/state.h>
#include <interface.h>
#include <io/state.h>
#include <kernel/state.h>
#include <motion/event_handler.h>
#include <np/state.h>
#include <packages/functions.h>
#include <packages/license.h>
#include <renderer/functions.h>
#include <renderer/shaders.h>
#include <renderer/state.h>
#include <touch/functions.h>
#include <util/fs.h>
#include <util/log.h>
#include <util/string_utils.h>

#if USE_DISCORD
#include <app/discord.h>
#endif

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_sensor.h>

#include <QCloseEvent>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QOpenGLContext>
#include <QProcess>
#include <QProgressDialog>
#include <QPushButton>
#include <QSlider>
#include <QStatusBar>
#include <QToolBar>
#include <QWidgetAction>
#include <QtResource>

#if defined(HAVE_X11) || defined(HAVE_WAYLAND)
#include <qpa/qplatformnativeinterface.h>
#endif

MainWindow *g_main_window = nullptr;

MainWindow::MainWindow(EmuEnvState &emuenv,
    std::shared_ptr<GuiSettings> gui_settings,
    std::shared_ptr<PersistentSettings> persistent_settings)
    : QMainWindow(nullptr)
    , emuenv{ emuenv }
    , m_ui(std::make_unique<Ui::MainWindow>())
    , m_gui_settings(std::move(gui_settings))
    , m_persistent_settings(std::move(persistent_settings)) {
    assert(!g_main_window);
    g_main_window = this;

    initialize();
}

MainWindow::~MainWindow() {
    shutdown_discord();
    if (g_main_window == this)
        g_main_window = nullptr;
}

void MainWindow::closeEvent(QCloseEvent *event) {
    if (m_game_window) {
        const int result = QMessageBox::question(this,
            tr("Exit?"),
            tr("A game is still running. Do you really want to exit?\n\nAny unsaved progress will be lost!"),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);

        if (result != QMessageBox::Yes) {
            event->ignore();
            return;
        }
        on_game_closed();
    }

    save_window_state();
    QMainWindow::closeEvent(event);
}

void MainWindow::save_window_state() {
    if (!m_gui_settings)
        return;

    m_gui_settings->set_value(gui::mw_geometry, saveGeometry(), false);
    m_gui_settings->set_value(gui::mw_windowState, saveState(), false);

    if (m_game_list_widget)
        m_game_list_widget->save_settings(*m_gui_settings);

    if (m_icon_size_slider)
        m_gui_settings->set_value(gui::gl_iconSize, m_icon_size_slider->value(), false);

    if (m_log_widget)
        m_gui_settings->set_value(gui::mw_loggerVisible, m_log_widget->isVisible(), false);
    if (m_game_list_widget)
        m_gui_settings->set_value(gui::mw_gamelistVisible, m_game_list_widget->isVisible(), false);

    m_gui_settings->set_value(gui::mw_toolBarVisible, m_ui->show_toolbar_action->isChecked(), false);
    m_gui_settings->set_value(gui::mw_titleBarsVisible, m_ui->show_title_bars_action->isChecked(), false);

    m_gui_settings->sync();
}

void MainWindow::restore_window_state() {
    if (!m_gui_settings)
        return;

    if (!restoreGeometry(m_gui_settings->get_value(gui::mw_geometry).toByteArray()))
        resize(1280, 720);

    restoreState(m_gui_settings->get_value(gui::mw_windowState).toByteArray());

    if (m_game_list_widget)
        m_game_list_widget->load_settings(*m_gui_settings);

    if (m_icon_size_slider) {
        const int icon_size = m_gui_settings->get_value(gui::gl_iconSize).toInt();
        m_icon_size_slider->setValue(icon_size);
    }

    if (m_log_widget) {
        const bool log_visible = m_gui_settings->get_value(gui::mw_loggerVisible).toBool();
        m_log_widget->setVisible(log_visible);
        m_ui->show_log_action->setChecked(log_visible);
    }
    if (m_game_list_widget) {
        const bool gamelist_visible = m_gui_settings->get_value(gui::mw_gamelistVisible).toBool();
        m_game_list_widget->setVisible(gamelist_visible);
        m_ui->show_gamelist_action->setChecked(gamelist_visible);
    }

    const bool toolbar_visible = m_gui_settings->get_value(gui::mw_toolBarVisible).toBool();
    m_ui->show_toolbar_action->setChecked(toolbar_visible);
    m_ui->toolBar->setVisible(toolbar_visible);

    const bool title_bars_visible = m_gui_settings->get_value(gui::mw_titleBarsVisible).toBool();
    m_ui->show_title_bars_action->setChecked(title_bars_visible);
    show_title_bars(title_bars_visible);
}

void MainWindow::initialize() {
    Q_INIT_RESOURCE(resources);
    m_ui->setupUi(this);

    this->resize(1280, 720);
    this->setWindowIcon(QIcon(":/Vita3K.png"));
    this->setWindowTitle(QString::fromStdString(window_title));

    emuenv.compat.log_compat_warn = emuenv.cfg.log_compat_warn;
    m_game_compat = new GameCompatibility(emuenv.compat, emuenv.cache_path.string(), this);

    auto *dummy = new QWidget(this);
    dummy->setFixedSize(0, 0);
    setCentralWidget(dummy);

    init_current_user();

    m_game_list_widget = new GameList(emuenv, this);
    m_game_list_widget->refresh();
    addDockWidget(Qt::TopDockWidgetArea, m_game_list_widget);

    m_log_widget = new LogWidget(this);
    m_log_widget->attach();
    addDockWidget(Qt::BottomDockWidgetArea, m_log_widget);
    setDockNestingEnabled(true);

    connect(m_game_list_widget, &GameList::boot_requested,
        this, &MainWindow::on_game_selected);

    connect(m_game_list_widget, &GameList::context_menu_requested,
        this, &MainWindow::on_context_menu_requested);

    m_sdl_pump_timer = new QTimer(this);
    m_sdl_pump_timer->setInterval(4);
    connect(m_sdl_pump_timer, &QTimer::timeout, this, &MainWindow::pump_sdl_events);
    m_sdl_pump_timer->start();

    m_trophy_notifier = new TrophyNotifier(this);

    setup_toolbar();

    connect(m_ui->welcome_dialog_action, &QAction::triggered, this, [this]() {
        auto *welcome = new WelcomeDialog(emuenv, true, this);
        welcome->open();
    });

    connect(m_ui->trophy_collection_action, &QAction::triggered,
        this, &MainWindow::open_trophy_collection);

    connect(m_ui->user_management_action, &QAction::triggered,
        this, &MainWindow::open_user_management);

    connect(m_ui->settings_core_action, &QAction::triggered, this, [this] { open_settings(0); });
    connect(m_ui->settings_cpu_action, &QAction::triggered, this, [this] { open_settings(1); });
    connect(m_ui->settings_gpu_action, &QAction::triggered, this, [this] { open_settings(2); });
    connect(m_ui->settings_audio_action, &QAction::triggered, this, [this] { open_settings(3); });
    connect(m_ui->settings_camera_action, &QAction::triggered, this, [this] { open_settings(4); });
    connect(m_ui->settings_system_action, &QAction::triggered, this, [this] { open_settings(5); });
    connect(m_ui->settings_emulator_action, &QAction::triggered, this, [this] { open_settings(6); });
    connect(m_ui->settings_gui_action, &QAction::triggered, this, [this] { open_settings(7); });
    connect(m_ui->settings_network_action, &QAction::triggered, this, [this] { open_settings(8); });
    connect(m_ui->settings_debug_action, &QAction::triggered, this, [this] { open_settings(9); });

    connect(m_ui->install_firmware_action, &QAction::triggered,
        this, &MainWindow::on_install_firmware_triggered);
    connect(m_ui->install_pkg_action, &QAction::triggered,
        this, &MainWindow::on_install_pkg_triggered);
    connect(m_ui->install_zip_action, &QAction::triggered,
        this, &MainWindow::on_install_zip_triggered);
    connect(m_ui->install_license_action, &QAction::triggered,
        this, &MainWindow::on_install_license_triggered);

    connect(m_ui->pause_action, &QAction::triggered,
        this, &MainWindow::on_toolbar_start);
    connect(m_ui->stop_action, &QAction::triggered,
        this, &MainWindow::on_stop_triggered);

    connect(m_ui->show_gamelist_action, &QAction::triggered, this, [this](bool checked) {
        m_game_list_widget->setVisible(checked);
        m_gui_settings->set_value(gui::mw_gamelistVisible, checked);
    });
    connect(m_game_list_widget, &QDockWidget::visibilityChanged,
        m_ui->show_gamelist_action, &QAction::setChecked);

    connect(m_ui->show_log_action, &QAction::triggered, this, [this](bool checked) {
        m_log_widget->setVisible(checked);
        m_gui_settings->set_value(gui::mw_loggerVisible, checked);
    });
    connect(m_log_widget, &QDockWidget::visibilityChanged,
        m_ui->show_log_action, &QAction::setChecked);

    connect(m_ui->show_toolbar_action, &QAction::triggered, this, [this](bool checked) {
        m_ui->toolBar->setVisible(checked);
        m_gui_settings->set_value(gui::mw_toolBarVisible, checked);
    });

    connect(m_ui->show_title_bars_action, &QAction::triggered, this, [this](bool checked) {
        show_title_bars(checked);
        m_gui_settings->set_value(gui::mw_titleBarsVisible, checked);
    });

    setAcceptDrops(true);

    setup_status_bar();

    setMinimumSize(350, minimumSizeHint().height());
    restore_window_state();

    if (m_log_widget && m_gui_settings) {
        const int buf_size = m_gui_settings->get_value(gui::l_bufferSize).toInt();
        m_log_widget->set_log_buffer_size(buf_size);
    }

    init_discord();

    if (emuenv.cfg.run_app_path.has_value()) {
        QTimer::singleShot(0, this, [this]() {
            const std::string title_id = *emuenv.cfg.run_app_path;
            emuenv.cfg.run_app_path.reset();
            boot_game(title_id);
        });
    }
}

void MainWindow::on_install_firmware_triggered() {
    auto *dlg = new FirmwareInstallDialog(emuenv, this);
    connect(dlg, &FirmwareInstallDialog::install_complete,
        this, &MainWindow::on_install_finished);
    connect(dlg, &QDialog::finished, dlg, &QObject::deleteLater);
    dlg->exec();
}

void MainWindow::on_install_pkg_triggered() {
    auto *dlg = new PkgInstallDialog(emuenv, this);
    connect(dlg, &PkgInstallDialog::install_complete,
        this, [this](const QString &) { on_install_finished(); });
    connect(dlg, &QDialog::finished, dlg, &QObject::deleteLater);
    dlg->exec();
}

void MainWindow::on_install_zip_triggered() {
    auto *dlg = new ArchiveInstallDialog(emuenv, this);
    connect(dlg, &ArchiveInstallDialog::install_complete,
        this, [this](const QList<ArchiveInstallResult> &) { on_install_finished(); });
    connect(dlg, &QDialog::finished, dlg, &QObject::deleteLater);
    dlg->exec();
}

void MainWindow::on_install_license_triggered() {
    auto *dlg = new LicenseInstallDialog(emuenv, this);
    connect(dlg, &QDialog::finished, dlg, &QObject::deleteLater);
    dlg->exec();
}

void MainWindow::on_install_finished() {
    m_game_list_widget->refresh(true);
}

void MainWindow::dragEnterEvent(QDragEnterEvent *event) {
    if (event->mimeData()->hasUrls())
        event->acceptProposedAction();
}

void MainWindow::dropEvent(QDropEvent *event) {
    for (const QUrl &url : event->mimeData()->urls()) {
        if (url.isLocalFile())
            handle_drop(url.toLocalFile());
    }
}

void MainWindow::handle_drop(const QString &path) {
    const fs::path drop_path(path.toStdString());
    const std::string ext = string_utils::tolower(drop_path.extension().string());

    if (ext == ".pup") {
        QProgressDialog prog(tr("Installing firmware\u2026"),
            /*cancel=*/QString(), 0, 100, this);
        prog.setWindowTitle(tr("Install Firmware"));
        prog.setWindowModality(Qt::WindowModal);
        prog.setMinimumDuration(0);
        prog.setValue(0);

        const std::string fw_version = install_pup(
            emuenv.pref_path,
            drop_path,
            [&prog](uint32_t pct) {
                prog.setValue(static_cast<int>(pct));
                QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
            });

        prog.setValue(100);

        if (!fw_version.empty()) {
            LOG_INFO("Firmware {} installed successfully via drop!", fw_version);
            on_install_finished();
        } else {
            QMessageBox::critical(this, tr("Install Failed"),
                tr("Failed to install the dropped firmware file.\n"
                   "Check the log for details."));
        }

    } else if (ext == ".vpk" || ext == ".zip") {
        const ReinstallCallback reinstall_cb = [this]() -> bool {
            return QMessageBox::question(
                       this,
                       tr("Already Installed"),
                       tr("This content is already installed.\n"
                          "Do you want to overwrite it?"),
                       QMessageBox::Yes | QMessageBox::No,
                       QMessageBox::No)
                == QMessageBox::Yes;
        };

        const std::vector<ContentInfo> contents = install_archive(emuenv, drop_path, {}, reinstall_cb);

        if (!contents.empty())
            on_install_finished();

    } else if (ext == ".rif" || drop_path.filename() == "work.bin") {
        if (copy_license(emuenv, drop_path))
            LOG_INFO("License installed via drop: {}", drop_path.string());
        else
            QMessageBox::critical(this, tr("Install Failed"),
                tr("Failed to install the dropped license file.\n"
                   "The file may be corrupted."));

    } else if (fs::is_directory(drop_path)) {
        const uint32_t n = install_contents(emuenv, drop_path);
        if (n > 0)
            on_install_finished();

    } else if (drop_path.filename() == "theme.xml") {
        const uint32_t n = install_contents(emuenv, drop_path.parent_path());
        if (n > 0)
            on_install_finished();

    } else {
        LOG_ERROR("Dropped file '{}' is not a supported install type.",
            drop_path.filename().string());
    }
}

void MainWindow::on_about_action_triggered() {
    AboutDialog about(emuenv, this);
    about.exec();
}

void MainWindow::on_controls_action_triggered() {
    m_sdl_pump_timer->stop();
    ControlsDialog dlg(emuenv, this);
    dlg.exec();
    m_sdl_pump_timer->start();
}

void MainWindow::open_settings(int tab_index) {
    const auto old_pref_path = emuenv.pref_path;
    SettingsDialog dlg(emuenv, m_gui_settings, {}, tab_index, this);
    dlg.exec();

    refresh_status_bar();

    if (m_log_widget && m_gui_settings) {
        const int buf_size = m_gui_settings->get_value(gui::l_bufferSize).toInt();
        m_log_widget->set_log_buffer_size(buf_size);
    }

    if (emuenv.pref_path != old_pref_path)
        m_game_list_widget->refresh(true);
}

void MainWindow::init_current_user() {
    const auto &users = emuenv.app.user_list.users;

    if (users.empty()) {
        const std::string id = app::create_user(emuenv, "Vita3K");
        app::activate_user(emuenv, id);
        emuenv.cfg.user_id = id;
        config::serialize_config(emuenv.cfg, emuenv.cfg.config_path);
        return;
    }

    if (!emuenv.cfg.user_id.empty() && users.contains(emuenv.cfg.user_id)) {
        app::activate_user(emuenv, emuenv.cfg.user_id);
        return;
    }

    const auto &first = users.begin();
    app::activate_user(emuenv, first->first);
    emuenv.cfg.user_id = first->first;
    config::serialize_config(emuenv.cfg, emuenv.cfg.config_path);
}

void MainWindow::on_game_selected(const app::Game &game) {
    boot_game(game.title_id);
}

void MainWindow::boot_game(const std::string &title_id) {
    if (m_game_window) {
        const int result = QMessageBox::question(this, tr("Restart Required"),
            tr("A game is already running.\n"
               "The emulator needs to restart to launch another game.\n\n"
               "Restart now?"),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);

        if (result != QMessageBox::Yes)
            return;

        const QString exe = QCoreApplication::applicationFilePath();
        QStringList args = { QStringLiteral("-r"), QString::fromStdString(title_id) };
        QProcess::startDetached(exe, args);
        QApplication::quit();
        return;
    }

    if (m_game_has_run) {
        const QString exe = QCoreApplication::applicationFilePath();
        QStringList args = { QStringLiteral("-r"), QString::fromStdString(title_id) };
        QProcess::startDetached(exe, args);
        QApplication::quit();
        return;
    }

    if (!app::set_app_info(emuenv, title_id)) {
        QMessageBox::critical(this, tr("Error"),
            tr("Could not find app '%1' in game list.").arg(QString::fromStdString(title_id)));
        return;
    }

    app::set_current_config(emuenv, title_id);
    get_license(emuenv, emuenv.io.title_id, emuenv.io.content_id);
    app::update_last_time_game_used(emuenv, title_id);

    m_game_window = new GameWindow(emuenv, emuenv.backend_renderer, screen());

    m_game_container = QWidget::createWindowContainer(m_game_window);
    m_game_container->setWindowTitle(tr("%1 | %2 (%3) | Loading...")
            .arg(QString::fromStdString(window_title),
                QString::fromStdString(emuenv.current_app_title),
                QString::fromStdString(emuenv.io.title_id)));
    m_game_container->setWindowIcon(windowIcon());
    m_game_container->setMinimumSize(160, 90);
    m_game_container->resize(960, 544);
    m_game_container->installEventFilter(this);
    m_game_container->show();

    if (emuenv.cfg.boot_apps_full_screen)
        m_game_container->showFullScreen();

    m_game_window->set_container(m_game_container);

    connect(m_game_window, &GameWindow::closed,
        this, &MainWindow::on_game_closed);

    const auto abort_boot = [&](const QString &msg) {
        QMessageBox::critical(m_game_container, tr("Error"), msg);
        if (m_loading_overlay) {
            m_loading_overlay->hide_overlay();
            m_loading_overlay = nullptr;
        }
        m_game_window = nullptr;
        delete m_game_container;
        m_game_container = nullptr;
    };

    renderer::WindowCallbacks callbacks;
    callbacks.native_handle = reinterpret_cast<void *>(m_game_window->winId());
#ifdef _WIN32
    callbacks.display_protocol = renderer::DisplayProtocol::Win32;
#elif defined(__APPLE__)
    callbacks.display_protocol = renderer::DisplayProtocol::MacOS;
#elif defined(HAVE_X11) || defined(HAVE_WAYLAND)
    {
        QPlatformNativeInterface *native = QGuiApplication::platformNativeInterface();
        if (native) {
#if defined(HAVE_WAYLAND)
            auto *wl_dpy = native->nativeResourceForWindow("display", nullptr);
            auto *wl_surf = native->nativeResourceForWindow("surface",
                const_cast<QWindow *>(static_cast<const QWindow *>(m_game_window)));
            if (wl_dpy && wl_surf) {
                callbacks.native_handle = wl_surf;
                callbacks.native_display = wl_dpy;
                callbacks.display_protocol = renderer::DisplayProtocol::Wayland;
                LOG_INFO("Using Wayland display protocol for Vulkan surface");
            } else
#endif
#if defined(HAVE_X11)
            {
                void *x11_display = native->nativeResourceForIntegration("display");
                callbacks.native_handle = reinterpret_cast<void *>(m_game_window->winId());
                callbacks.native_display = x11_display;
                callbacks.display_protocol = renderer::DisplayProtocol::X11;
                LOG_INFO("Using X11 display protocol for Vulkan surface");
            }
#endif
        } else {
            LOG_ERROR("Could not get QPlatformNativeInterface, display protocol unknown");
        }
    }
#endif
    callbacks.client_width = [win = m_game_window]() -> int {
        return win->client_width_px();
    };
    callbacks.client_height = [win = m_game_window]() -> int {
        return win->client_height_px();
    };

    bool renderer_ok = false;
    if (emuenv.backend_renderer == renderer::Backend::Vulkan) {
        renderer_ok = renderer::init(callbacks, emuenv.renderer,
            emuenv.backend_renderer, emuenv.cfg, emuenv.get_root_paths());
    } else {
        if (!m_game_window->create_gl_context()) {
            abort_boot(tr("Could not create OpenGL context.\nDoes your GPU support at least OpenGL 4.4?"));
            return;
        }
        if (!m_game_window->make_current()) {
            abort_boot(tr("Could not make OpenGL context current."));
            return;
        }
        auto *gl_ctx = m_game_window->gl_context();
        callbacks.get_proc_address = [gl_ctx](const char *name) -> void * {
            return reinterpret_cast<void *>(
                reinterpret_cast<uintptr_t>(gl_ctx->getProcAddress(name)));
        };
        callbacks.default_fbo = [gl_ctx]() -> unsigned int {
            return gl_ctx->defaultFramebufferObject();
        };
        callbacks.swap = [win = m_game_window]() {
            win->swap_buffers();
        };
        callbacks.set_current = [win = m_game_window]() -> bool {
            return win->make_current();
        };
        callbacks.done_current = [win = m_game_window]() {
            win->done_current();
        };
        renderer_ok = renderer::init(callbacks, emuenv.renderer,
            emuenv.backend_renderer, emuenv.cfg, emuenv.get_root_paths());
    }

    if (!renderer_ok) {
        abort_boot(tr("Failed to initialise the renderer."));
        return;
    }

    app::apply_renderer_config(emuenv);

    m_game_container->setWindowTitle(tr("%1 | %2 (%3) | Initializing...")
            .arg(QString::fromStdString(window_title),
                QString::fromStdString(emuenv.current_app_title),
                QString::fromStdString(emuenv.io.title_id)));

    if (!app::late_init(emuenv)) {
        abort_boot(tr("Failed to initialize emulator state."));
        return;
    }

    m_trophy_notifier->install(emuenv);

    m_game_container->setWindowTitle(tr("%1 | %2 (%3) | Loading modules...")
            .arg(QString::fromStdString(window_title),
                QString::fromStdString(emuenv.current_app_title),
                QString::fromStdString(emuenv.io.title_id)));

    int32_t main_module_id = 0;
    if (load_app(main_module_id, emuenv) != Success) {
        abort_boot(tr("Failed to load app modules."));
        return;
    }

    emuenv.renderer->set_app(emuenv.io.title_id.c_str(), emuenv.self_name.c_str());

    if (renderer::get_shaders_cache_hashs(*emuenv.renderer) && emuenv.cfg.shader_cache) {
        const auto &hashes = emuenv.renderer->shaders_cache_hashs;
        const int total = static_cast<int>(hashes.size());

        m_loading_overlay = new GameOverlay(m_game_container, emuenv.display);
        m_loading_overlay->set_background(
            load_app_background(emuenv.io.app_path, emuenv.pref_path.string()));
        m_loading_overlay->set_progress(0, total, tr("Please wait, compiling shaders..."));
        m_loading_overlay->show_overlay();

        for (int i = 0; i < total; ++i) {
            m_loading_overlay->set_progress(i, total, tr("Please wait, compiling shaders..."));

            QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);

            if (emuenv.backend_renderer == renderer::Backend::OpenGL)
                m_game_window->make_current();

            emuenv.renderer->precompile_shader(hashes[i]);
        }

        m_loading_overlay->set_progress(total, total, tr("Done"));
        QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
    }

    if (!m_loading_overlay) {
        m_loading_overlay = new GameOverlay(m_game_container, emuenv.display);
        m_loading_overlay->set_background(
            load_app_background(emuenv.io.app_path, emuenv.pref_path.string()));
        m_loading_overlay->show_overlay();
    }

    m_loading_overlay->set_progress(0, 0, {});

    if (run_app(emuenv, main_module_id) != Success) {
        abort_boot(tr("Failed to start game threads."));
        return;
    }

    m_trophy_toast = new TrophyToast(m_game_container, emuenv.display);
    m_trophy_toast->connect_notifer(m_trophy_notifier);

    if (m_trophy_dialog)
        connect(m_trophy_notifier, &TrophyNotifier::trophy_unlocked,
            m_trophy_dialog, &TrophyCollectionDialog::on_trophy_unlocked,
            Qt::QueuedConnection);

    auto *kb_filter = new CtrlKeyboardFilter(emuenv, m_game_container);
    m_game_container->installEventFilter(kb_filter);
    m_game_window->installEventFilter(kb_filter);
    m_kb_filter = kb_filter;

    connect(m_kb_filter, &CtrlKeyboardFilter::ps_button_pressed,
        this, &MainWindow::on_ps_button);

    connect(m_kb_filter, &CtrlKeyboardFilter::fullscreen_toggled,
        this, [this]() { if (m_game_window) m_game_window->toggle_fullscreen(); });

    connect(m_kb_filter, &CtrlKeyboardFilter::texture_replacement_toggled,
        this, [this]() { toggle_texture_replacement(emuenv); });

    connect(m_kb_filter, &CtrlKeyboardFilter::screenshot_requested,
        this, [this]() { take_screenshot(emuenv); });

    m_game_window->set_loading_overlay(m_loading_overlay);
    m_loading_overlay = nullptr;

    if (emuenv.backend_renderer == renderer::Backend::OpenGL) {
        m_game_window->done_current();
        m_game_window->prepare_gl_for_render_thread();
    }

    renderer::start_render_thread(*emuenv.renderer, emuenv.display, emuenv.gxm, emuenv.mem, emuenv.cfg);

    if (emuenv.backend_renderer == renderer::Backend::OpenGL)
        m_game_window->complete_gl_migration();

    m_game_window->start_ui_updates();

    m_game_container->setWindowTitle(tr("%1 | %2 (%3) | Please wait, loading...")
            .arg(QString::fromStdString(window_title),
                QString::fromStdString(emuenv.current_app_title),
                QString::fromStdString(emuenv.io.title_id)));

    LOG_INFO("Game started: {} ({})", emuenv.current_app_title, title_id);
    m_game_has_run = true;

#if USE_DISCORD
    if (emuenv.cfg.discord_rich_presence)
        discordrpc::update_presence(emuenv.io.title_id, emuenv.current_app_title);
#endif

    refresh_emulation_actions();
    refresh_status_bar();
}

QImage MainWindow::load_app_background(const std::string &app_path,
    const std::string &pref_path) {
    const auto path = fs::path(pref_path) / "ux0/app" / app_path / "sce_sys/pic0.png";
    if (fs::exists(path)) {
        QImage img;
        if (img.load(QString::fromStdString(path.string())))
            return img;
    }
    return {};
}

void MainWindow::on_game_closed() {
    if (!m_game_window)
        return;

    LOG_INFO("Game closed: {}", emuenv.current_app_title);

    if (m_trophy_toast) {
        m_trophy_toast->deleteLater();
        m_trophy_toast = nullptr;
    }
    disconnect(m_trophy_notifier, nullptr, nullptr, nullptr);
    emuenv.np.trophy_state.trophy_unlock_callback = nullptr;

    app::update_game_time_used(emuenv, emuenv.io.app_path);

    emuenv.kernel.exit_delete_all_threads();
    emuenv.gxm.display_queue.abort();
    emuenv.display.abort = true;

    if (emuenv.audio.adapter)
        emuenv.audio.switch_state(true);

    if (emuenv.display.vblank_thread && emuenv.display.vblank_thread->joinable())
        emuenv.display.vblank_thread->join();

    renderer::stop_render_thread(*emuenv.renderer);
    m_game_window->stop_ui_updates();

    if (emuenv.backend_renderer == renderer::Backend::OpenGL)
        m_game_window->make_current();

    emuenv.renderer->preclose_action();
    app::destroy(emuenv);

    if (emuenv.backend_renderer == renderer::Backend::OpenGL)
        m_game_window->done_current();

    m_game_window = nullptr;

    if (m_game_container) {
        m_game_container->deleteLater();
        m_game_container = nullptr;
    }

    emuenv.io.app_path.clear();
    emuenv.io.title_id.clear();
    emuenv.current_app_title.clear();
    emuenv.display.abort = false;

    m_kb_filter = nullptr;

    m_game_list_widget->refresh();

#if USE_DISCORD
    if (emuenv.cfg.discord_rich_presence)
        discordrpc::update_presence();
#endif

    refresh_emulation_actions();

    config::set_current_config(emuenv.cfg, emuenv.config_path, {});
    refresh_status_bar();

    raise();
    activateWindow();
}

bool MainWindow::eventFilter(QObject *watched, QEvent *event) {
    if (watched == m_game_container) {
        if (event->type() == QEvent::Close) {
            event->ignore();

            const int result = QMessageBox::question(m_game_container,
                tr("Exit Game?"),
                tr("Do you really want to exit the game?\n\nAny unsaved progress will be lost!"),
                QMessageBox::Yes | QMessageBox::No, QMessageBox::No);

            if (result == QMessageBox::Yes) {
                on_game_closed();
            } else {
                m_game_container->raise();
                m_game_container->activateWindow();
            }

            return true;
        }
    }

    return QMainWindow::eventFilter(watched, event);
}

void MainWindow::pump_sdl_events() {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        switch (event.type) {
        case SDL_EVENT_GAMEPAD_ADDED:
        case SDL_EVENT_GAMEPAD_REMOVED:
            refresh_controllers(emuenv.ctrl, emuenv);
            break;

        case SDL_EVENT_GAMEPAD_TOUCHPAD_DOWN:
        case SDL_EVENT_GAMEPAD_TOUCHPAD_MOTION:
        case SDL_EVENT_GAMEPAD_TOUCHPAD_UP:
            handle_touchpad_event(event.gtouchpad);
            break;

        case SDL_EVENT_GAMEPAD_SENSOR_UPDATE:
            handle_motion_event(emuenv, event.gsensor.sensor, event.gsensor);
            break;

        case SDL_EVENT_SENSOR_UPDATE:
            handle_motion_event(emuenv, SDL_GetSensorTypeForID(event.sensor.which), event.sensor);
            break;

        case SDL_EVENT_FINGER_DOWN:
        case SDL_EVENT_FINGER_MOTION:
        case SDL_EVENT_FINGER_UP:
            handle_touch_event(event.tfinger);
            break;

        case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
            if (event.gbutton.button == SDL_GAMEPAD_BUTTON_GUIDE)
                on_ps_button();
            if (m_game_window && !emuenv.kernel.is_threads_paused()
                && event.gbutton.button == SDL_GAMEPAD_BUTTON_TOUCHPAD)
                toggle_touchscreen();
            break;

        default:
            break;
        }
    }

#if USE_DISCORD
    discordrpc::update_init_status(emuenv.cfg.discord_rich_presence, &m_discord_rich_presence_old);
#endif
}

void MainWindow::on_pause_triggered() {
    if (!m_game_window)
        return;

    const bool is_paused = emuenv.kernel.is_threads_paused();
    app::switch_state(emuenv, !is_paused);

    m_game_window->set_paused(!is_paused);
    refresh_emulation_actions();
}

void MainWindow::on_stop_triggered() {
    if (!m_game_window)
        return;

    if (emuenv.kernel.is_threads_paused()) {
        app::switch_state(emuenv, false);
        m_game_window->set_paused(false);
    }

    on_game_closed();
}

void MainWindow::on_ps_button() {
    on_pause_triggered();
}

void MainWindow::refresh_emulation_actions() {
    const bool running = m_game_window != nullptr;
    const bool paused = running && emuenv.kernel.is_threads_paused();

    m_ui->stop_action->setEnabled(running);
    m_ui->stop_action->setIcon(m_icon_stop);
    m_ui->toolbar_stop->setEnabled(running);

    if (running) {
        if (paused) {
            m_ui->pause_action->setText(tr("Resume"));
            m_ui->pause_action->setIcon(m_icon_play);
            m_ui->pause_action->setEnabled(true);
            m_ui->toolbar_start->setIcon(m_icon_play);
            m_ui->toolbar_start->setText(tr("Resume"));
            m_ui->toolbar_start->setEnabled(true);
        } else {
            m_ui->pause_action->setText(tr("Pause"));
            m_ui->pause_action->setIcon(m_icon_pause);
            m_ui->pause_action->setEnabled(true);
            m_ui->toolbar_start->setIcon(m_icon_pause);
            m_ui->toolbar_start->setText(tr("Pause"));
            m_ui->toolbar_start->setEnabled(true);
        }
    } else {
        m_ui->pause_action->setText(tr("Play"));
        m_ui->pause_action->setIcon(m_icon_play);
        m_ui->pause_action->setEnabled(m_game_selected);
        m_ui->toolbar_start->setIcon(m_icon_play);
        m_ui->toolbar_start->setText(tr("Play"));
        m_ui->toolbar_start->setEnabled(m_game_selected);
    }
}

void MainWindow::open_trophy_collection() {
    if (!m_trophy_dialog) {
        m_trophy_dialog = new TrophyCollectionDialog(emuenv, this);
        if (m_game_window) {
            connect(m_trophy_notifier, &TrophyNotifier::trophy_unlocked,
                m_trophy_dialog, &TrophyCollectionDialog::on_trophy_unlocked,
                Qt::QueuedConnection);
        }
        connect(m_trophy_dialog, &QObject::destroyed, this, [this] {
            m_trophy_dialog = nullptr;
        });
    }
    m_trophy_dialog->show();
    m_trophy_dialog->raise();
    m_trophy_dialog->activateWindow();
}

void MainWindow::open_user_management() {
    if (m_game_window) {
        return;
    }

    if (!m_user_mgmt_dialog) {
        m_user_mgmt_dialog = new UserManagementDialog(emuenv, this);
        connect(m_user_mgmt_dialog, &UserManagementDialog::user_changed,
            m_game_list_widget, [this] { m_game_list_widget->refresh(); });
        connect(m_user_mgmt_dialog, &UserManagementDialog::user_changed,
            this, [this] {
                if (m_trophy_dialog)
                    m_trophy_dialog->reload();
            });
        connect(m_user_mgmt_dialog, &QObject::destroyed, this, [this] {
            m_user_mgmt_dialog = nullptr;
        });
    }
    m_user_mgmt_dialog->show();
    m_user_mgmt_dialog->raise();
    m_user_mgmt_dialog->activateWindow();
}

void MainWindow::setup_toolbar() {
    auto *tb = m_ui->toolBar;
    tb->setObjectName(QStringLiteral("mw_toolbar"));
    tb->setStyleSheet(QStringLiteral("QToolBar { border: none; border-top: 1px solid palette(dark); }"));

    connect(m_ui->toolbar_open, &QAction::triggered, this, [this] {
        auto *tb = m_ui->toolBar;
        auto *w = tb->widgetForAction(m_ui->toolbar_open);
        const QPoint pos = w ? w->mapToGlobal(QPoint(0, w->height())) : QCursor::pos();

        QMenu menu(this);
        menu.addAction(tr("Open Pref Path"), this, [this] {
            gui::utils::open_dir(emuenv.pref_path.string());
        });

        const auto patch_path = emuenv.shared_path / "patch";
        menu.addAction(tr("Open Patch Path"), this, [patch_path] {
            gui::utils::open_dir(patch_path.string());
        });

        const auto textures_path = emuenv.shared_path / "textures";
        menu.addAction(tr("Open Textures Path"), this, [textures_path] {
            if (!fs::exists(textures_path))
                fs::create_directories(textures_path);
            gui::utils::open_dir(textures_path.string());
        });

        menu.exec(pos);
    });

    connect(m_ui->toolbar_refresh, &QAction::triggered,
        this, &MainWindow::on_toolbar_refresh);

    connect(m_ui->toolbar_fullscreen, &QAction::triggered,
        this, &MainWindow::on_toolbar_fullscreen);

    connect(m_ui->toolbar_stop, &QAction::triggered,
        this, &MainWindow::on_stop_triggered);

    connect(m_ui->toolbar_start, &QAction::triggered,
        this, &MainWindow::on_toolbar_start);

    connect(m_ui->toolbar_config, &QAction::triggered,
        this, [this] { open_settings(0); });

    connect(m_ui->toolbar_controls, &QAction::triggered,
        this, &MainWindow::on_controls_action_triggered);

    m_slider_container = new QWidget(this);
    auto *slider_layout = new QHBoxLayout(m_slider_container);
    slider_layout->setContentsMargins(14, 0, 14, 0);
    slider_layout->setSpacing(0);

    m_icon_size_slider = new QSlider(Qt::Horizontal, m_slider_container);
    m_icon_size_slider->setRange(0, 100);
    m_icon_size_slider->setValue(25);
    m_icon_size_slider->setFocusPolicy(Qt::ClickFocus);
    slider_layout->addWidget(m_icon_size_slider);

    tb->addWidget(m_slider_container);

    connect(m_icon_size_slider, &QSlider::valueChanged,
        this, &MainWindow::resize_icons);

    m_search_bar = new QLineEdit(this);
    m_search_bar->setPlaceholderText(tr("Search..."));
    m_search_bar->setFocusPolicy(Qt::ClickFocus);
    m_search_bar->setClearButtonEnabled(true);
    tb->addWidget(m_search_bar);

    connect(m_search_bar, &QLineEdit::textChanged,
        m_game_list_widget, &GameList::set_search_text);

    connect(m_game_list_widget, &GameList::game_selection_changed,
        this, &MainWindow::on_game_selection_changed);

    repaint_toolbar_icons();

    resize_icons(m_icon_size_slider->value());
    refresh_emulation_actions();
}

void MainWindow::show_title_bars(bool show) const {
    if (m_game_list_widget)
        m_game_list_widget->SetTitleBarVisible(show);
    if (m_log_widget)
        m_log_widget->SetTitleBarVisible(show);
}

void MainWindow::repaint_toolbar_icons() {
    auto *tb = m_ui->toolBar;
    const QColor fg = gui::utils::get_foreground_color(this);

    std::map<QIcon::Mode, QColor> new_colors;
    new_colors[QIcon::Normal] = gui::utils::get_label_color(QStringLiteral("toolbar_icon_color"), fg, fg);
    new_colors[QIcon::Disabled] = gui::utils::get_label_color(QStringLiteral("toolbar_icon_color_disabled"), Qt::gray, Qt::darkGray);
    new_colors[QIcon::Active] = gui::utils::get_label_color(QStringLiteral("toolbar_icon_color_active"), fg, fg);
    new_colors[QIcon::Selected] = gui::utils::get_label_color(QStringLiteral("toolbar_icon_color_selected"), fg, fg);

    const auto icon = [&new_colors](const QString &path) {
        return gui::utils::get_colorized_icon(QIcon(path), Qt::black, new_colors);
    };

    m_icon_play = icon(QStringLiteral(":/icons/play.png"));
    m_icon_pause = icon(QStringLiteral(":/icons/pause.png"));
    m_icon_stop = icon(QStringLiteral(":/icons/stop.png"));
    m_icon_fullscreen_on = icon(QStringLiteral(":/icons/fullscreen.png"));
    m_icon_fullscreen_off = icon(QStringLiteral(":/icons/exit_fullscreen.png"));

    m_ui->toolbar_open->setIcon(icon(QStringLiteral(":/icons/open.png")));
    m_ui->toolbar_refresh->setIcon(icon(QStringLiteral(":/icons/refresh.png")));
    m_ui->toolbar_fullscreen->setIcon(m_fullscreen ? m_icon_fullscreen_off : m_icon_fullscreen_on);
    m_ui->toolbar_stop->setIcon(m_icon_stop);
    m_ui->toolbar_config->setIcon(icon(QStringLiteral(":/icons/configure.png")));
    m_ui->toolbar_controls->setIcon(icon(QStringLiteral(":/icons/controllers.png")));

    const bool running = m_game_window != nullptr;
    const bool paused = running && emuenv.kernel.is_threads_paused();
    if (running && !paused)
        m_ui->toolbar_start->setIcon(m_icon_pause);
    else
        m_ui->toolbar_start->setIcon(m_icon_play);

    const int tool_bar_height = tb->sizeHint().height();

    for (const auto &act : tb->actions()) {
        if (act->isSeparator())
            continue;
        if (auto *w = tb->widgetForAction(act))
            w->setMinimumWidth(tool_bar_height);
    }

    if (m_slider_container)
        m_slider_container->setFixedWidth(tool_bar_height * 4);
    if (m_search_bar)
        m_search_bar->setFixedWidth(tool_bar_height * 5);

    const QColor &handle_color = new_colors[QIcon::Normal];
    if (m_icon_size_slider) {
        m_icon_size_slider->setStyleSheet(
            QStringLiteral("QSlider::handle:horizontal{ background: rgba(%1, %2, %3, %4); }")
                .arg(handle_color.red())
                .arg(handle_color.green())
                .arg(handle_color.blue())
                .arg(handle_color.alpha()));
    }
}

void MainWindow::on_toolbar_refresh() {
    m_game_list_widget->refresh(true);
}

void MainWindow::on_toolbar_fullscreen() {
    if (m_fullscreen) {
        if (m_game_container)
            m_game_container->showNormal();
        else
            showNormal();
        m_fullscreen = false;
        m_ui->toolbar_fullscreen->setIcon(m_icon_fullscreen_on);
        m_ui->toolbar_fullscreen->setText(tr("Fullscreen"));
    } else {
        if (m_game_container)
            m_game_container->showFullScreen();
        else
            showFullScreen();
        m_fullscreen = true;
        m_ui->toolbar_fullscreen->setIcon(m_icon_fullscreen_off);
        m_ui->toolbar_fullscreen->setText(tr("Exit Fullscreen"));
    }
}

void MainWindow::on_toolbar_start() {
    if (m_game_window) {
        on_pause_triggered();
    } else {
        const app::Game *game = m_game_list_widget->selected_game();
        if (game && !game->title_id.empty())
            boot_game(game->title_id);
    }
}

void MainWindow::on_game_selection_changed(const app::Game *game) {
    m_game_selected = game && !game->title_id.empty();
    refresh_emulation_actions();
}

void MainWindow::on_context_menu_requested(const QPoint &global_pos, const std::vector<const app::Game *> &games) {
    GameListContextMenu menu(emuenv, games, m_game_compat, m_game_window != nullptr, this);

    connect(&menu, &GameListContextMenu::boot_requested,
        this, &MainWindow::on_game_selected);
    connect(&menu, &GameListContextMenu::open_settings_requested,
        this, [this](const std::string &app_path, int tab) {
            SettingsDialog dlg(emuenv, m_gui_settings, app_path, tab, this);
            dlg.exec();
            if (!app_path.empty() && !m_game_window) {
                config::set_current_config(emuenv.cfg, emuenv.config_path, {});
            }
            refresh_status_bar();
            m_game_list_widget->refresh();
        });
    connect(&menu, &GameListContextMenu::refresh_requested,
        this, [this] { m_game_list_widget->refresh(); });

    menu.exec(global_pos);
}

void MainWindow::resize_icons(int index) {
    if (m_icon_size_slider->value() != index) {
        m_icon_size_slider->setSliderPosition(index);
        return;
    }
    m_game_list_widget->resize_icons(index);
}

static const QString status_button_base_style = QStringLiteral(
    "QPushButton { border: 1px solid transparent; padding: 2px 8px;"
    " font-weight: bold; font-size: 11px; color: %1; background: transparent; border-radius: 6px; }"
    "QPushButton:hover { border: 1px solid #666666; }"
    "QPushButton:pressed { border: 1px solid #888888; }");

static QString make_button_style(const QString &fg, const QString &hover) {
    return status_button_base_style.arg(fg, hover);
}

void MainWindow::setup_status_bar() {
    auto *sb = statusBar();
    sb->setStyleSheet(QStringLiteral(
        "QStatusBar { background: #2b2b2b; }"
        "QStatusBar::item { border: none; }"));

    m_renderer_button = new QPushButton(this);
    m_renderer_button->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_renderer_button, &QPushButton::clicked, this, [this] {
        auto &cc = emuenv.cfg.current_config;
        cc.backend_renderer = (cc.backend_renderer == "Vulkan") ? "OpenGL" : "Vulkan";
        emuenv.cfg.backend_renderer = cc.backend_renderer;
        save_config();
        update_renderer_button();
        update_accuracy_button();
        update_screen_filter_button();
    });
    connect(m_renderer_button, &QPushButton::customContextMenuRequested, this, [this](const QPoint &pos) {
        QMenu menu(this);
        auto *vulkan_action = menu.addAction(QStringLiteral("Vulkan"));
        auto *opengl_action = menu.addAction(QStringLiteral("OpenGL"));
        QAction *chosen = menu.exec(m_renderer_button->mapToGlobal(pos));
        if (!chosen)
            return;
        auto &cc = emuenv.cfg.current_config;
        if (chosen == vulkan_action) {
            cc.backend_renderer = "Vulkan";
        } else if (chosen == opengl_action) {
            cc.backend_renderer = "OpenGL";
        }
        emuenv.cfg.backend_renderer = cc.backend_renderer;
        save_config();
        update_renderer_button();
        update_accuracy_button();
        update_screen_filter_button();
    });
    sb->addWidget(m_renderer_button);

    m_accuracy_button = new QPushButton(this);
    m_accuracy_button->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_accuracy_button, &QPushButton::clicked, this, [this] {
        auto &cc = emuenv.cfg.current_config;
        cc.high_accuracy = !cc.high_accuracy;
        save_config();
        update_accuracy_button();
    });
    connect(m_accuracy_button, &QPushButton::customContextMenuRequested, this, [this](const QPoint &pos) {
        QMenu menu(this);
        auto *standard_action = menu.addAction(tr("Standard"));
        auto *high_action = menu.addAction(tr("High"));
        QAction *chosen = menu.exec(m_accuracy_button->mapToGlobal(pos));
        if (!chosen)
            return;
        auto &cc = emuenv.cfg.current_config;
        cc.high_accuracy = (chosen == high_action);
        save_config();
        update_accuracy_button();
    });
    sb->addWidget(m_accuracy_button);

    auto get_filter_names = [this]() -> QStringList {
        if (emuenv.cfg.current_config.backend_renderer == "Vulkan")
            return { QStringLiteral("Nearest"), QStringLiteral("Bilinear"),
                QStringLiteral("Bicubic"), QStringLiteral("FXAA"), QStringLiteral("FSR") };
        else
            return { QStringLiteral("Bilinear"), QStringLiteral("FXAA") };
    };

    auto apply_screen_filter = [this](const std::string &filter) {
        emuenv.cfg.current_config.screen_filter = filter;
        save_config();
        if (emuenv.renderer)
            emuenv.renderer->set_screen_filter(filter);
        update_screen_filter_button();
    };

    m_screen_filter_button = new QPushButton(this);
    m_screen_filter_button->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_screen_filter_button, &QPushButton::clicked, this, [this, get_filter_names, apply_screen_filter] {
        const QStringList filters = get_filter_names();
        const QString current = QString::fromStdString(emuenv.cfg.current_config.screen_filter);
        int idx = filters.indexOf(current);
        idx = (idx + 1) % filters.size();
        apply_screen_filter(filters[idx].toStdString());
    });
    connect(m_screen_filter_button, &QPushButton::customContextMenuRequested, this, [this, get_filter_names, apply_screen_filter](const QPoint &pos) {
        const QStringList filters = get_filter_names();
        QMenu menu(this);
        for (const auto &name : filters)
            menu.addAction(name);
        QAction *chosen = menu.exec(m_screen_filter_button->mapToGlobal(pos));
        if (!chosen)
            return;
        apply_screen_filter(chosen->text().toStdString());
    });
    sb->addWidget(m_screen_filter_button);

    m_audio_backend_button = new QPushButton(this);
    m_audio_backend_button->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_audio_backend_button, &QPushButton::clicked, this, [this] {
        auto &cc = emuenv.cfg.current_config;
        cc.audio_backend = (cc.audio_backend == "SDL") ? "Cubeb" : "SDL";
        save_config();
        update_audio_backend_button();
    });
    connect(m_audio_backend_button, &QPushButton::customContextMenuRequested, this, [this](const QPoint &pos) {
        QMenu menu(this);
        auto *sdl_action = menu.addAction(QStringLiteral("SDL"));
        auto *cubeb_action = menu.addAction(QStringLiteral("Cubeb"));
        QAction *chosen = menu.exec(m_audio_backend_button->mapToGlobal(pos));
        if (!chosen)
            return;
        auto &cc = emuenv.cfg.current_config;
        cc.audio_backend = (chosen == cubeb_action) ? "Cubeb" : "SDL";
        save_config();
        update_audio_backend_button();
    });
    sb->addWidget(m_audio_backend_button);

    m_ngs_button = new QPushButton(this);
    m_ngs_button->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_ngs_button, &QPushButton::clicked, this, [this] {
        auto &cc = emuenv.cfg.current_config;
        cc.ngs_enable = !cc.ngs_enable;
        save_config();
        update_ngs_button();
    });
    connect(m_ngs_button, &QPushButton::customContextMenuRequested, this, [this](const QPoint &pos) {
        QMenu menu(this);
        auto *on_action = menu.addAction(tr("NGS: ON"));
        auto *off_action = menu.addAction(tr("NGS: OFF"));
        QAction *chosen = menu.exec(m_ngs_button->mapToGlobal(pos));
        if (!chosen)
            return;
        emuenv.cfg.current_config.ngs_enable = (chosen == on_action);
        save_config();
        update_ngs_button();
    });
    sb->addWidget(m_ngs_button);

    m_volume_button = new QPushButton(this);
    m_volume_button->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_volume_button, &QPushButton::clicked, this, [this] {
        QMenu menu(this);
        auto *container = new QWidget(&menu);
        auto *layout = new QHBoxLayout(container);
        layout->setContentsMargins(12, 8, 12, 8);

        auto *slider = new QSlider(Qt::Horizontal, container);
        slider->setRange(0, 100);
        slider->setValue(emuenv.cfg.current_config.audio_volume);
        slider->setMinimumWidth(180);

        layout->addWidget(slider);
        connect(slider, &QSlider::valueChanged, this, [this](int val) {
            emuenv.cfg.current_config.audio_volume = val;
            emuenv.audio.set_global_volume(val / 100.f);
            save_config();
            update_volume_button();
        });
        auto *action = new QWidgetAction(&menu);
        action->setDefaultWidget(container);
        menu.addAction(action);
        menu.exec(m_volume_button->mapToGlobal(QPoint(0, -menu.sizeHint().height())));
    });
    connect(m_volume_button, &QPushButton::customContextMenuRequested, this, [this](const QPoint &pos) {
        QMenu menu(this);
        auto *mute_action = menu.addAction(tr("Mute"));
        auto *reset_action = menu.addAction(tr("Reset to 100%"));
        QAction *chosen = menu.exec(m_volume_button->mapToGlobal(pos));
        if (!chosen)
            return;
        if (chosen == mute_action) {
            emuenv.cfg.current_config.audio_volume = 0;
        } else if (chosen == reset_action) {
            emuenv.cfg.current_config.audio_volume = 100;
        }
        emuenv.audio.set_global_volume(emuenv.cfg.current_config.audio_volume / 100.f);
        save_config();
        update_volume_button();
    });
    sb->addWidget(m_volume_button);

    update_renderer_button();
    update_accuracy_button();
    update_screen_filter_button();
    update_audio_backend_button();
    update_ngs_button();
    update_volume_button();
}

void MainWindow::update_renderer_button() {
    const auto &renderer = emuenv.cfg.current_config.backend_renderer;
    if (renderer == "Vulkan") {
        m_renderer_button->setText(QStringLiteral("VULKAN"));
        m_renderer_button->setStyleSheet(make_button_style(
            QStringLiteral("#ff4444"), QStringLiteral("#4a2a2a")));
    } else {
        m_renderer_button->setText(QStringLiteral("OPENGL"));
        m_renderer_button->setStyleSheet(make_button_style(
            QStringLiteral("#4488ff"), QStringLiteral("#2a3a4a")));
    }
}

void MainWindow::update_accuracy_button() {
    const bool high = emuenv.cfg.current_config.high_accuracy;
    const bool is_vulkan = emuenv.cfg.current_config.backend_renderer == "Vulkan";
    m_accuracy_button->setVisible(is_vulkan);
    if (high) {
        m_accuracy_button->setText(tr("HIGH"));
        m_accuracy_button->setStyleSheet(make_button_style(
            QStringLiteral("#ff8800"), QStringLiteral("#4a3a2a")));
    } else {
        m_accuracy_button->setText(tr("STANDARD"));
        m_accuracy_button->setStyleSheet(make_button_style(
            QStringLiteral("#ff8800"), QStringLiteral("#4a3a2a")));
    }
}

void MainWindow::update_screen_filter_button() {
    auto &cc = emuenv.cfg.current_config;
    const QStringList valid = (cc.backend_renderer == "Vulkan")
        ? QStringList{ QStringLiteral("Nearest"), QStringLiteral("Bilinear"),
              QStringLiteral("Bicubic"), QStringLiteral("FXAA"), QStringLiteral("FSR") }
        : QStringList{ QStringLiteral("Bilinear"), QStringLiteral("FXAA") };

    const QString current = QString::fromStdString(cc.screen_filter);
    if (!valid.contains(current)) {
        cc.screen_filter = valid.first().toStdString();
        save_config();
    }

    m_screen_filter_button->setText(QString::fromStdString(cc.screen_filter).toUpper());
    m_screen_filter_button->setStyleSheet(make_button_style(
        QStringLiteral("#cccccc"), QStringLiteral("#3a3a3a")));
}

void MainWindow::update_audio_backend_button() {
    const auto &backend = emuenv.cfg.current_config.audio_backend;
    if (backend == "SDL") {
        m_audio_backend_button->setText(QStringLiteral("SDL"));
        m_audio_backend_button->setStyleSheet(make_button_style(
            QStringLiteral("#88dd44"), QStringLiteral("#2a3a2a")));
    } else {
        m_audio_backend_button->setText(QStringLiteral("CUBEB"));
        m_audio_backend_button->setStyleSheet(make_button_style(
            QStringLiteral("#88dd44"), QStringLiteral("#2a3a2a")));
    }
}

void MainWindow::update_ngs_button() {
    const bool ngs = emuenv.cfg.current_config.ngs_enable;
    if (ngs) {
        m_ngs_button->setText(tr("NGS: ON"));
        m_ngs_button->setStyleSheet(make_button_style(
            QStringLiteral("#cccccc"), QStringLiteral("#3a3a3a")));
    } else {
        m_ngs_button->setText(tr("NGS: OFF"));
        m_ngs_button->setStyleSheet(make_button_style(
            QStringLiteral("#888888"), QStringLiteral("#3a3a3a")));
    }
}

void MainWindow::update_volume_button() {
    const int vol = emuenv.cfg.current_config.audio_volume;

    if (vol == 0) {
        m_volume_button->setText(tr("VOLUME: MUTED"));
        m_volume_button->setStyleSheet(make_button_style(
            QStringLiteral("#777777"), QStringLiteral("#aaaaaa")));
    } else {
        m_volume_button->setText(tr("VOLUME: %1%").arg(vol));
        m_volume_button->setStyleSheet(make_button_style(
            QStringLiteral("#cccccc"), QStringLiteral("#aaaaaa")));
    }
}

void MainWindow::refresh_status_bar() {
    const bool running = m_game_window != nullptr;

    update_renderer_button();
    update_accuracy_button();
    update_screen_filter_button();
    update_audio_backend_button();
    update_ngs_button();
    update_volume_button();

    m_renderer_button->setEnabled(!running);
    m_accuracy_button->setEnabled(!running);
    m_audio_backend_button->setEnabled(!running);
    m_ngs_button->setEnabled(!running);
}

void MainWindow::save_config() {
    config::copy_current_config_to_global(emuenv.cfg);
    config::serialize_config(emuenv.cfg, emuenv.cfg.config_path);
}

void MainWindow::init_discord() {
#if USE_DISCORD
    if (emuenv.cfg.discord_rich_presence) {
        discordrpc::init();
        discordrpc::update_presence();
        m_discord_rich_presence_old = true;
    }
#endif
}

void MainWindow::shutdown_discord() {
#if USE_DISCORD
    if (m_discord_rich_presence_old) {
        discordrpc::shutdown();
        m_discord_rich_presence_old = false;
    }
#endif
}
