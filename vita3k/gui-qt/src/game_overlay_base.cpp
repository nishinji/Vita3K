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

#include <gui-qt/game_overlay_base.h>

#include <display/state.h>

#include <QApplication>
#include <QEvent>
#include <QTimer>

GameOverlayBase::GameOverlayBase(QWidget *game_widget,
    DisplayState &display,
    bool transparent_for_input)
    : QFrame()
    , m_display(display)
    , m_game_widget(game_widget)
    , m_transparent_for_input(transparent_for_input) {
    Qt::WindowFlags host_flags = Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint;
    if (transparent_for_input)
        host_flags |= Qt::WindowTransparentForInput;

    m_host = new QWidget(nullptr, host_flags);
    m_host->setAttribute(Qt::WA_TranslucentBackground);
    m_host->setAttribute(Qt::WA_ShowWithoutActivating);
    m_host->setProperty("vita3k_overlay_host", true);
    m_host->hide();

    setParent(m_host);
    setAttribute(Qt::WA_TranslucentBackground);

    m_visibility_timer = new QTimer(this);
    m_visibility_timer->setSingleShot(true);
    m_visibility_timer->setInterval(50);
    connect(m_visibility_timer, &QTimer::timeout, this, &GameOverlayBase::check_visibility);

    if (m_game_widget)
        m_game_widget->installEventFilter(this);
    m_host->installEventFilter(this);

    hide();
}

GameOverlayBase::~GameOverlayBase() {
    setParent(nullptr);
    delete m_host;
}

void GameOverlayBase::show_overlay() {
    m_overlay_active = true;
    reposition();
    check_visibility();
}

void GameOverlayBase::hide_overlay() {
    m_overlay_active = false;
    m_visibility_timer->stop();
    hide();
    m_host->hide();
}

QRect GameOverlayBase::viewport_rect() const {
    if (!m_game_widget)
        return QRect();

    const QSize widget_size = m_game_widget->size();

    if (m_display.viewport_w > 0 && m_display.viewport_h > 0
        && m_display.viewport_drawable_w > 0 && m_display.viewport_drawable_h > 0) {
        const qreal dw = static_cast<qreal>(m_display.viewport_drawable_w);
        const qreal dh = static_cast<qreal>(m_display.viewport_drawable_h);
        const qreal ww = static_cast<qreal>(widget_size.width());
        const qreal wh = static_cast<qreal>(widget_size.height());

        return QRect(
            static_cast<int>(m_display.viewport_x / dw * ww),
            static_cast<int>(m_display.viewport_y / dh * wh),
            static_cast<int>(m_display.viewport_w / dw * ww),
            static_cast<int>(m_display.viewport_h / dh * wh));
    }

    return QRect(0, 0, widget_size.width(), widget_size.height());
}

qreal GameOverlayBase::scale_x() const {
    return viewport_rect().width() / 960.0;
}

qreal GameOverlayBase::scale_y() const {
    return viewport_rect().height() / 544.0;
}

void GameOverlayBase::reposition() {
    if (!m_game_widget)
        return;

    const QSize game_size = m_game_widget->size();
    if (game_size.isEmpty())
        return;

    const QPoint origin = m_game_widget->mapToGlobal(QPoint(0, 0));
    m_host->setGeometry(QRect(origin, game_size));
    setGeometry(0, 0, game_size.width(), game_size.height());

    on_reposition();
}

void GameOverlayBase::check_visibility() {
    if (!m_overlay_active)
        return;

    if (!m_game_widget || !m_game_widget->isVisible() || m_game_widget->isMinimized()) {
        m_host->hide();
        hide();
        return;
    }

    const QWidget *active = QApplication::activeWindow();
    const bool should_show = m_game_widget->isActiveWindow()
        || (active && active->property("vita3k_overlay_host").toBool());

    if (should_show) {
        m_host->show();
        m_host->raise();
        show();
        raise();
    } else {
        m_host->hide();
        hide();
        m_visibility_timer->start(100);
    }
}

void GameOverlayBase::schedule_visibility_check() {
    if (m_overlay_active)
        m_visibility_timer->start();
}

bool GameOverlayBase::eventFilter(QObject *watched, QEvent *event) {
    if (watched == m_game_widget) {
        switch (event->type()) {
        case QEvent::Move:
        case QEvent::Resize:
            if (m_overlay_active) {
                m_visibility_timer->stop();
                reposition();
                m_host->show();
                m_host->raise();
                show();
                raise();
            }
            break;
        case QEvent::WindowActivate:
        case QEvent::WindowDeactivate:
            schedule_visibility_check();
            break;
        case QEvent::WindowStateChange:
            if (m_game_widget->isMinimized()) {
                m_visibility_timer->stop();
                m_host->hide();
                hide();
            } else {
                schedule_visibility_check();
            }
            break;
        default:
            break;
        }
    } else if (watched == m_host) {
        switch (event->type()) {
        case QEvent::WindowActivate:
        case QEvent::WindowDeactivate:
            schedule_visibility_check();
            break;
        default:
            break;
        }
    }
    return QFrame::eventFilter(watched, event);
}
