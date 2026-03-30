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

#include <gui-qt/game_overlay_base.h>
#include <gui-qt/trophy_notifier.h>

#include <QPropertyAnimation>
#include <QQueue>

class QLabel;

class TrophyToast : public GameOverlayBase {
    Q_OBJECT

    Q_PROPERTY(int slide_x READ slide_x WRITE set_slide_x)

public:
    explicit TrophyToast(QWidget *game_widget, DisplayState &display);
    ~TrophyToast() override = default;

    void connect_notifer(TrophyNotifier *notifier);

public Q_SLOTS:
    void on_trophy_unlocked(TrophyUnlockData data);

protected:
    void on_reposition() override;
    void paintEvent(QPaintEvent *event) override;

private Q_SLOTS:
    void on_animation_finished();

private:
    void show_next();
    void load_current();

    int slide_x() const { return pos().x(); }
    void set_slide_x(int x) { move(x, pos().y()); }

    QLabel *m_icon_label = nullptr;
    QLabel *m_grade_icon_label = nullptr;
    QLabel *m_name_label = nullptr;
    QLabel *m_earned_label = nullptr;

    QPropertyAnimation *m_anim = nullptr;

    enum class Stage { Idle,
        SlideIn,
        Hold,
        SlideOut };
    Stage m_stage = Stage::Idle;

    QQueue<TrophyUnlockData> m_queue;

    static constexpr int k_widget_w = 380;
    static constexpr int k_widget_h = 72;
    static constexpr int k_icon_size = 48;
    static constexpr int k_margin = 12;
    static constexpr int k_top_margin = 20;
    static constexpr int k_hold_ms = 4000;
    static constexpr int k_slide_ms = 220;

    qreal m_applied_scale = 0;
};