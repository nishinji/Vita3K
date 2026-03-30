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

#include <config/config.h>
#include <gui-qt/common_dialog_overlay.h>
#include <gui-qt/game_overlay.h>
#include <gui-qt/game_window.h>
#include <gui-qt/ime_overlay.h>
#include <gui-qt/perf_overlay.h>
#include <gui-qt/shader_hint_overlay.h>

#include <config/state.h>
#include <config/version.h>
#include <dialog/state.h>
#include <display/state.h>
#include <ime/state.h>
#include <io/state.h>
#include <renderer/state.h>
#include <touch/state.h>
#include <util/log.h>

#include <QApplication>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QOpenGLContext>
#include <QScreen>
#include <QThread>
#include <QTimer>
#include <QWidget>

#include <algorithm>
#include <chrono>

#ifdef _WIN32
#include <Windows.h>
#endif

GameWindow::GameWindow(EmuEnvState &emuenv, renderer::Backend backend, QScreen *screen)
    : QWindow(screen)
    , m_emuenv(emuenv)
    , m_backend(backend) {
    if (m_backend == renderer::Backend::OpenGL) {
        setSurfaceType(QSurface::OpenGLSurface);

        m_format.setRenderableType(QSurfaceFormat::OpenGL);
        m_format.setMajorVersion(4);
        m_format.setMinorVersion(3);
        m_format.setProfile(QSurfaceFormat::CoreProfile);
        m_format.setDepthBufferSize(0);
        m_format.setSwapBehavior(QSurfaceFormat::DoubleBuffer);
        m_format.setSwapInterval(static_cast<int>(emuenv.cfg.v_sync));
        setFormat(m_format);
    } else {
#ifdef __APPLE__
        setSurfaceType(QSurface::VulkanSurface);
#else
        setSurfaceType(QSurface::RasterSurface);
#endif
    }

    setMinimumSize(QSize(160, 90));
    resize(960, 544);
}

GameWindow::~GameWindow() {
    stop_ui_updates();
    if (m_shader_hint_overlay) {
        m_shader_hint_overlay->hide_overlay();
        delete m_shader_hint_overlay;
        m_shader_hint_overlay = nullptr;
    }
    if (m_perf_overlay) {
        m_perf_overlay->hide_overlay();
        delete m_perf_overlay;
        m_perf_overlay = nullptr;
    }
    if (m_ime_overlay) {
        m_ime_overlay->dismiss();
        delete m_ime_overlay;
        m_ime_overlay = nullptr;
    }
    if (m_dialog_overlay) {
        m_dialog_overlay->dismiss();
        delete m_dialog_overlay;
        m_dialog_overlay = nullptr;
    }
    if (m_gl_context) {
        m_gl_context->doneCurrent();
        delete m_gl_context;
        m_gl_context = nullptr;
    }
}

bool GameWindow::create_gl_context() {
    if (m_gl_context) {
        LOG_WARN("GL context already created");
        return true;
    }

    m_gl_context = new QOpenGLContext();

    QSurfaceFormat fmt = m_format;
#ifndef NDEBUG
    fmt.setOption(QSurfaceFormat::DebugContext);
#endif

    // Try GL versions 4.6 -> 4.5 -> 4.4
    constexpr int accept_minor_versions[] = { 6, 5, 4 };
    bool created = false;
    for (int minor : accept_minor_versions) {
        fmt.setMajorVersion(4);
        fmt.setMinorVersion(minor);
        m_gl_context->setFormat(fmt);
        if (m_gl_context->create()) {
            created = true;
            LOG_INFO("Created OpenGL {}.{} context", 4, minor);
            break;
        }
    }

    if (!created) {
        LOG_ERROR("Failed to create OpenGL context (need at least ver 4.4)");
        delete m_gl_context;
        m_gl_context = nullptr;
        return false;
    }

    return true;
}

bool GameWindow::make_current() {
    if (!m_gl_context) {
        LOG_ERROR("make_current called with no GL context");
        return false;
    }

    if (m_gl_migration_pending && QThread::currentThread() != m_gl_context->thread()) {
        m_render_thread_id.set_value(QThread::currentThread());
        m_gl_migration_done_future.wait();
        m_gl_migration_pending = false;
    }

    if (!m_gl_context->makeCurrent(this)) {
        LOG_WARN("makeCurrent failed, attempting recovery");
        if (!m_gl_context->isValid()) {
            if (!m_gl_context->create()) {
                LOG_ERROR("Failed to recreate OpenGL context");
                return false;
            }
        }
        if (!m_gl_context->makeCurrent(this)) {
            LOG_ERROR("Could not bind OpenGL context after recovery");
            return false;
        }
    }

    return true;
}

void GameWindow::swap_buffers() {
    if (m_gl_context && isExposed()) {
        m_gl_context->swapBuffers(this);
    }
}

void GameWindow::done_current() {
    if (m_gl_context) {
        m_gl_context->doneCurrent();
        if (QThread::currentThread() != QCoreApplication::instance()->thread()) {
            m_gl_context->moveToThread(QCoreApplication::instance()->thread());
        }
    }
}

void GameWindow::prepare_gl_for_render_thread() {
    m_render_thread_id = std::promise<QThread *>();
    m_render_thread_id_future = m_render_thread_id.get_future();
    m_gl_migration_done = std::promise<void>();
    m_gl_migration_done_future = m_gl_migration_done.get_future().share();
    m_gl_migration_pending = true;
}

void GameWindow::complete_gl_migration() {
    QThread *render_qthread = m_render_thread_id_future.get();
    m_gl_context->moveToThread(render_qthread);
    m_gl_migration_done.set_value();
}

void GameWindow::start_ui_updates() {
    if (m_ui_timer)
        return;

    m_ui_timer = new QTimer(this);
    m_ui_timer->setInterval(16);
    connect(m_ui_timer, &QTimer::timeout, this, &GameWindow::ui_tick);
    m_ui_timer->start();
}

void GameWindow::stop_ui_updates() {
    if (m_ui_timer) {
        m_ui_timer->stop();
        delete m_ui_timer;
        m_ui_timer = nullptr;
    }
    m_fps_tracking_started = false;
}

void GameWindow::process_common_dialogs() {
    auto &dialog = m_emuenv.common_dialog;

    if (dialog.status != SCE_COMMON_DIALOG_STATUS_RUNNING) {
        if (m_dialog_overlay) {
            m_dialog_overlay->dismiss();
            m_dialog_overlay->deleteLater();
            m_dialog_overlay = nullptr;
        }
        return;
    }

    if (!m_dialog_overlay && m_container) {
        m_dialog_overlay = new CommonDialogOverlay(m_container, m_emuenv);
    }

    if (m_dialog_overlay) {
        m_dialog_overlay->update_dialog();
    }
}

void GameWindow::process_ime() {
    auto &ime = m_emuenv.ime;

    if (!ime.state) {
        if (m_ime_overlay) {
            m_ime_overlay->dismiss();
            m_ime_overlay->deleteLater();
            m_ime_overlay = nullptr;
        }
        return;
    }

    if (!m_ime_overlay && m_container) {
        m_ime_overlay = new ImeOverlay(m_container, m_emuenv);
    }

    if (m_ime_overlay) {
        m_ime_overlay->update_ime();
    }
}

void GameWindow::ui_tick() {
    process_common_dialogs();
    process_ime();
    process_shader_hint();
    process_perf_overlay();
    update_window_title();

    if (m_loading_overlay) {
        if (m_emuenv.frame_count > 0) {
            m_loading_overlay->hide_overlay();
            m_loading_overlay->deleteLater();
            m_loading_overlay = nullptr;
        }
    }
}

void GameWindow::exposeEvent(QExposeEvent *) {
    // no-op
}

void GameWindow::resizeEvent(QResizeEvent *e) {
    QWindow::resizeEvent(e);
}

bool GameWindow::event(QEvent *e) {
    if (e->type() == QEvent::Close) {
        e->ignore();
        return true;
    }
    return QWindow::event(e);
}

void GameWindow::keyPressEvent(QKeyEvent *e) {
    e->ignore();
}

void GameWindow::keyReleaseEvent(QKeyEvent *e) {
    e->ignore();
}

void GameWindow::mousePressEvent(QMouseEvent *e) {
    auto &touch = m_emuenv.touch;
    const float scale_x = (width() > 0) ? static_cast<float>(client_width_px()) / width() : 1.0f;
    const float scale_y = (height() > 0) ? static_cast<float>(client_height_px()) / height() : 1.0f;
    touch.mouse_x = static_cast<float>(e->position().x()) * scale_x;
    touch.mouse_y = static_cast<float>(e->position().y()) * scale_y;
    if (e->button() == Qt::LeftButton)
        touch.mouse_button_left = true;
    if (e->button() == Qt::RightButton)
        touch.mouse_button_right = true;
}

void GameWindow::mouseReleaseEvent(QMouseEvent *e) {
    auto &touch = m_emuenv.touch;
    const float scale_x = (width() > 0) ? static_cast<float>(client_width_px()) / width() : 1.0f;
    const float scale_y = (height() > 0) ? static_cast<float>(client_height_px()) / height() : 1.0f;
    touch.mouse_x = static_cast<float>(e->position().x()) * scale_x;
    touch.mouse_y = static_cast<float>(e->position().y()) * scale_y;
    if (e->button() == Qt::LeftButton)
        touch.mouse_button_left = false;
    if (e->button() == Qt::RightButton)
        touch.mouse_button_right = false;
}

void GameWindow::mouseMoveEvent(QMouseEvent *e) {
    auto &touch = m_emuenv.touch;
    const float scale_x = (width() > 0) ? static_cast<float>(client_width_px()) / width() : 1.0f;
    const float scale_y = (height() > 0) ? static_cast<float>(client_height_px()) / height() : 1.0f;
    touch.mouse_x = static_cast<float>(e->position().x()) * scale_x;
    touch.mouse_y = static_cast<float>(e->position().y()) * scale_y;
}

void GameWindow::focusInEvent(QFocusEvent *) {
    m_emuenv.touch.renderer_focused = true;
}

void GameWindow::focusOutEvent(QFocusEvent *) {
    m_emuenv.touch.renderer_focused = false;
    m_emuenv.touch.mouse_button_left = false;
    m_emuenv.touch.mouse_button_right = false;
}

void GameWindow::set_container(QWidget *container) {
    m_container = container;
    m_emuenv.touch.native_handle = reinterpret_cast<void *>(winId());
}

int GameWindow::client_width_px() const {
#ifdef _WIN32
    RECT rect;
    if (GetClientRect(reinterpret_cast<HWND>(winId()), &rect))
        return rect.right - rect.left;
#endif
    return static_cast<int>(width() * devicePixelRatio());
}

int GameWindow::client_height_px() const {
#ifdef _WIN32
    RECT rect;
    if (GetClientRect(reinterpret_cast<HWND>(winId()), &rect))
        return rect.bottom - rect.top;
#endif
    return static_cast<int>(height() * devicePixelRatio());
}

void GameWindow::toggle_fullscreen() {
    if (!m_container)
        return;

    if (m_container->isFullScreen()) {
        m_container->showNormal();
    } else {
        m_container->showFullScreen();
    }
}

static constexpr uint32_t frames_size = 20;

void GameWindow::process_shader_hint() {
    if (!m_emuenv.cfg.show_compile_shaders || !m_emuenv.renderer) {
        if (m_shader_hint_overlay) {
            m_shader_hint_overlay->hide_overlay();
            m_shader_hint_overlay->deleteLater();
            m_shader_hint_overlay = nullptr;
            m_shaders_compiled_count = 0;
        }
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    const uint32_t newly_compiled = m_emuenv.renderer->shaders_count_compiled;
    if (newly_compiled > 0) {
        m_shaders_compiled_count += newly_compiled;
        m_emuenv.renderer->shaders_count_compiled = 0;
        m_shaders_compiled_time = now;
    } else if (m_shaders_compiled_count > 0) {
        const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - m_shaders_compiled_time).count();
        if (elapsed >= 3) {
            m_shaders_compiled_count = 0;
            if (m_shader_hint_overlay) {
                m_shader_hint_overlay->hide_overlay();
                m_shader_hint_overlay->deleteLater();
                m_shader_hint_overlay = nullptr;
            }
            return;
        }
    }

    if (m_shaders_compiled_count == 0)
        return;

    if (!m_shader_hint_overlay && m_container) {
        m_shader_hint_overlay = new ShaderHintOverlay(m_container, m_emuenv.display);
        m_shader_hint_overlay->show_overlay();
    }

    if (m_shader_hint_overlay) {
        const bool is_vulkan = (m_emuenv.renderer->current_backend == renderer::Backend::Vulkan);
        const QString label = is_vulkan
            ? QStringLiteral("%1 pipelines compiled").arg(m_shaders_compiled_count)
            : QStringLiteral("%1 shaders compiled").arg(m_shaders_compiled_count);
        m_shader_hint_overlay->set_text(label);

        if (m_perf_overlay && m_perf_overlay->is_overlay_active()
            && m_emuenv.cfg.performance_overlay_position == BOTTOM_LEFT) {
            m_shader_hint_overlay->set_vertical_offset(m_perf_overlay->overlay_height() + 4);
        } else {
            m_shader_hint_overlay->set_vertical_offset(0);
        }
    }
}

void GameWindow::process_perf_overlay() {
    if (!m_emuenv.cfg.performance_overlay || m_emuenv.fps == 0) {
        if (m_perf_overlay) {
            m_perf_overlay->hide_overlay();
            m_perf_overlay->deleteLater();
            m_perf_overlay = nullptr;
        }
        return;
    }

    if (!m_perf_overlay && m_container) {
        m_perf_overlay = new PerfOverlay(m_container, m_emuenv.display);
        m_perf_overlay->show_overlay();
    }

    if (m_perf_overlay) {
        m_perf_overlay->set_position(static_cast<PerformanceOverlayPosition>(m_emuenv.cfg.performance_overlay_position));
        m_perf_overlay->set_detail(static_cast<PerformanceOverlayDetail>(m_emuenv.cfg.performance_overlay_detail));

        PerfOverlay::Stats stats;
        stats.fps = m_emuenv.fps;
        stats.avg_fps = m_emuenv.avg_fps;
        stats.min_fps = m_emuenv.min_fps;
        stats.max_fps = m_emuenv.max_fps;
        stats.ms_per_frame = m_emuenv.ms_per_frame;
        stats.fps_values = m_emuenv.fps_values;
        stats.fps_values_count = frames_size;
        stats.fps_offset = m_emuenv.current_fps_offset;
        m_perf_overlay->set_stats(stats);
    }
}

void GameWindow::update_window_title() {
    if (m_emuenv.frame_count == 0)
        return;

    if (!m_fps_tracking_started) {
        m_fps_tracking_started = true;
        m_last_fps_time = std::chrono::steady_clock::now();
        m_emuenv.frame_count = 0;
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    const uint32_t ms = static_cast<uint32_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(now - m_last_fps_time).count());

    if (ms >= 1000) {
        const uint32_t frame_count = static_cast<uint32_t>(m_emuenv.frame_count);
        m_emuenv.fps = (frame_count * 1000 + ms / 2) / ms;
        m_emuenv.ms_per_frame = (ms + frame_count / 2) / frame_count;
        m_last_fps_time = now;
        m_emuenv.frame_count = 0;

        // FPS statistics
        m_emuenv.fps_values[m_emuenv.current_fps_offset] = static_cast<float>(m_emuenv.fps);
        m_emuenv.current_fps_offset = (m_emuenv.current_fps_offset + 1) % frames_size;
        float avg_fps = 0;
        for (uint32_t i = 0; i < frames_size; i++)
            avg_fps += m_emuenv.fps_values[i];
        m_emuenv.avg_fps = static_cast<uint32_t>(avg_fps) / frames_size;
        m_emuenv.min_fps = static_cast<uint32_t>(*std::min_element(m_emuenv.fps_values, std::next(m_emuenv.fps_values, frames_size)));
        m_emuenv.max_fps = static_cast<uint32_t>(*std::max_element(m_emuenv.fps_values, std::next(m_emuenv.fps_values, frames_size)));

        const auto af = m_emuenv.cfg.current_config.anisotropic_filtering > 1
            ? fmt::format(" | AF {}x", m_emuenv.cfg.current_config.anisotropic_filtering)
            : "";
        const auto x = m_emuenv.display.next_rendered_frame.image_size.x * m_emuenv.cfg.current_config.resolution_multiplier;
        const auto y = m_emuenv.display.next_rendered_frame.image_size.y * m_emuenv.cfg.current_config.resolution_multiplier;
        const std::string title = fmt::format("{} | {} ({}) | {} | {} FPS ({} ms) | {}x{}{} | {}",
            window_title,
            m_emuenv.current_app_title, m_emuenv.io.title_id,
            m_emuenv.cfg.current_config.backend_renderer,
            m_emuenv.fps, m_emuenv.ms_per_frame,
            x, y, af, m_emuenv.cfg.current_config.screen_filter);

        if (m_container)
            m_container->setWindowTitle(QString::fromStdString(title));
    }
}

void GameWindow::set_paused(bool paused) {
    if (paused) {
        if (!m_pause_overlay) {
            m_emuenv.drop_inputs = true;
            m_pause_overlay = new GameOverlay(m_container, m_emuenv.display, OverlayMode::Paused);
            m_pause_overlay->show_overlay();
        }
    } else {
        if (m_pause_overlay) {
            m_emuenv.drop_inputs = false;
            m_pause_overlay->hide_overlay();
            m_pause_overlay->deleteLater();
            m_pause_overlay = nullptr;
        }
    }
}
