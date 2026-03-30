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

#include <QString>

class ShaderHintOverlay : public GameOverlayBase {
    Q_OBJECT
public:
    explicit ShaderHintOverlay(QWidget *game_widget, DisplayState &display);
    ~ShaderHintOverlay() override = default;

    void set_text(const QString &text);

    void set_vertical_offset(int offset);

protected:
    void on_reposition() override;
    void paintEvent(QPaintEvent *) override;

private:
    QSize overlay_size() const;
    QPoint overlay_pos() const;

    QString m_text;
    int m_vertical_offset = 0;
    static constexpr int k_pad_x = 10;
    static constexpr int k_pad_y = 6;
    static constexpr int k_margin = 8;
    static constexpr int k_radius = 5;
    static constexpr int k_font_px = 13;
};
