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

class QString;
class QImage;

enum class OverlayMode {
    ShaderCompile,
    Paused,
};

class GameOverlay : public GameOverlayBase {
    Q_OBJECT
public:
    explicit GameOverlay(QWidget *game_widget, DisplayState &display,
        OverlayMode mode = OverlayMode::ShaderCompile);
    ~GameOverlay() override = default;

    void set_mode(OverlayMode mode);
    OverlayMode mode() const { return m_mode; }

    void set_background(const QImage &img);
    void set_progress(int done, int total, const QString &label);

protected:
    void on_reposition() override;
    void paintEvent(QPaintEvent *) override;

private:
    void paint_shader_compile(QPainter &p);
    void paint_paused(QPainter &p);

    OverlayMode m_mode = OverlayMode::ShaderCompile;
    QImage m_bg;
    int m_done = 0;
    int m_total = 0;
    QString m_label;
};
