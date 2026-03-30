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

#include <config/config.h>
#include <gui-qt/game_overlay_base.h>

#include <array>

class PerfOverlay : public GameOverlayBase {
    Q_OBJECT
public:
    explicit PerfOverlay(QWidget *game_widget, DisplayState &display);
    ~PerfOverlay() override = default;

    struct Stats {
        uint32_t fps = 0;
        uint32_t avg_fps = 0;
        uint32_t min_fps = 0;
        uint32_t max_fps = 0;
        uint32_t ms_per_frame = 0;
        const float *fps_values = nullptr;
        uint32_t fps_values_count = 0;
        uint32_t fps_offset = 0;
    };

    void set_stats(const Stats &stats);
    void set_position(PerformanceOverlayPosition pos);
    void set_detail(PerformanceOverlayDetail detail);

    int overlay_height() const;

protected:
    void on_reposition() override;
    void paintEvent(QPaintEvent *) override;

private:
    QSize overlay_size() const;
    QPoint overlay_pos() const;

    Stats m_stats{};
    PerformanceOverlayPosition m_position = TOP_LEFT;
    PerformanceOverlayDetail m_detail = MINIMUM;

    static constexpr int k_pad_x = 10;
    static constexpr int k_pad_y = 6;
    static constexpr int k_margin = 12;
    static constexpr int k_radius = 5;
    static constexpr int k_font_px = 13;
    static constexpr int k_line_spacing = 2;
    static constexpr int k_graph_h = 40;
    static constexpr int k_graph_top_pad = 4;
};
