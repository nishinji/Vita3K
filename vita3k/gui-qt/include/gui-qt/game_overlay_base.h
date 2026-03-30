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

#include <QFrame>

class QTimer;
struct DisplayState;

class GameOverlayBase : public QFrame {
    Q_OBJECT

public:
    explicit GameOverlayBase(QWidget *game_widget,
        DisplayState &display,
        bool transparent_for_input = false);
    ~GameOverlayBase() override;

    void show_overlay();
    void hide_overlay();
    bool is_overlay_active() const { return m_overlay_active; }

    QRect viewport_rect() const;
    qreal scale_x() const;
    qreal scale_y() const;

    QWidget *game_widget() const { return m_game_widget; }
    QWidget *host_widget() const { return m_host; }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

    virtual void on_reposition() {}

    DisplayState &m_display;

private:
    void reposition();
    void check_visibility();
    void schedule_visibility_check();

    QWidget *m_host = nullptr;
    QWidget *m_game_widget = nullptr;
    QTimer *m_visibility_timer = nullptr;
    bool m_overlay_active = false;
    bool m_transparent_for_input = false;
};
