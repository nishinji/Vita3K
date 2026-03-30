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

#include <gui-qt/perf_overlay.h>

#include <QColor>
#include <QPainter>

#include <algorithm>

static const QColor k_bg_color(0, 0, 0, 150);

PerfOverlay::PerfOverlay(QWidget *game_widget, DisplayState &display)
    : GameOverlayBase(game_widget, display, /*transparent_for_input=*/true) {
}

void PerfOverlay::set_stats(const Stats &stats) {
    m_stats = stats;
    on_reposition();
    update();
}

void PerfOverlay::set_position(PerformanceOverlayPosition pos) {
    if (m_position == pos)
        return;
    m_position = pos;
    on_reposition();
}

void PerfOverlay::set_detail(PerformanceOverlayDetail detail) {
    if (m_detail == detail)
        return;
    m_detail = detail;
    on_reposition();
    update();
}

int PerfOverlay::overlay_height() const {
    return overlay_size().height();
}

QSize PerfOverlay::overlay_size() const {
    QFont f;
    f.setPixelSize(k_font_px);
    const QFontMetrics fm(f);

    const int line_h = fm.height();
    int text_lines = 1;
    if (m_detail >= MEDIUM)
        text_lines = 2;

    const int text_h = text_lines * line_h + (text_lines - 1) * k_line_spacing;

    const QString fps_text = m_detail == MINIMUM
        ? QStringLiteral("FPS: %1").arg(m_stats.fps)
        : QStringLiteral("FPS: %1  Avg: %2").arg(m_stats.fps).arg(m_stats.avg_fps);
    int text_w = fm.horizontalAdvance(fps_text);

    if (m_detail >= MEDIUM) {
        const QString minmax = QStringLiteral("Min: %1  Max: %2").arg(m_stats.min_fps).arg(m_stats.max_fps);
        text_w = std::max(text_w, fm.horizontalAdvance(minmax));
    }

    int total_h = text_h + k_pad_y * 2;
    if (m_detail == MAXIMUM)
        total_h += k_graph_top_pad + k_graph_h;

    return QSize(text_w + k_pad_x * 2, total_h);
}

QPoint PerfOverlay::overlay_pos() const {
    const QRect vp = viewport_rect();
    const QSize sz = overlay_size();

    const int left = vp.x() + k_margin;
    const int center_x = vp.x() + (vp.width() - sz.width()) / 2;
    const int right = vp.x() + vp.width() - sz.width() - k_margin;
    const int top = vp.y() + k_margin;
    const int bottom = vp.y() + vp.height() - sz.height() - k_margin;

    switch (m_position) {
    case TOP_LEFT: return QPoint(left, top);
    case TOP_CENTER: return QPoint(center_x, top);
    case TOP_RIGHT: return QPoint(right, top);
    case BOTTOM_LEFT: return QPoint(left, bottom);
    case BOTTOM_CENTER: return QPoint(center_x, bottom);
    case BOTTOM_RIGHT: return QPoint(right, bottom);
    default: return QPoint(left, top);
    }
}

void PerfOverlay::on_reposition() {
    const QSize sz = overlay_size();
    const QPoint pos = overlay_pos();
    setGeometry(pos.x(), pos.y(), sz.width(), sz.height());
}

void PerfOverlay::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    p.setPen(Qt::NoPen);
    p.setBrush(k_bg_color);
    p.drawRoundedRect(rect(), k_radius, k_radius);

    p.setPen(QColor(220, 220, 220));
    QFont f = p.font();
    f.setPixelSize(k_font_px);
    f.setWeight(QFont::Normal);
    p.setFont(f);

    const QFontMetrics fm(f);
    const int line_h = fm.height();
    int y = k_pad_y;

    const QString fps_text = m_detail == MINIMUM
        ? QStringLiteral("FPS: %1").arg(m_stats.fps)
        : QStringLiteral("FPS: %1  Avg: %2").arg(m_stats.fps).arg(m_stats.avg_fps);
    p.drawText(k_pad_x, y + fm.ascent(), fps_text);
    y += line_h;

    if (m_detail >= MEDIUM) {
        y += k_line_spacing;
        p.setPen(QColor(180, 180, 180, 100));
        p.drawLine(k_pad_x, y - 1, width() - k_pad_x, y - 1);

        p.setPen(QColor(220, 220, 220));
        const QString minmax = QStringLiteral("Min: %1  Max: %2").arg(m_stats.min_fps).arg(m_stats.max_fps);
        p.drawText(k_pad_x, y + fm.ascent(), minmax);
        y += line_h;
    }

    if (m_detail == MAXIMUM && m_stats.fps_values && m_stats.fps_values_count > 1) {
        y += k_graph_top_pad;
        const int graph_x = k_pad_x;
        const int graph_w = width() - k_pad_x * 2;
        const int graph_y = y;
        const int graph_h = k_graph_h;

        float max_val = 1.0f;
        for (uint32_t i = 0; i < m_stats.fps_values_count; ++i)
            max_val = std::max(max_val, m_stats.fps_values[i]);

        p.setPen(QPen(QColor(220, 220, 220, 180), 1.0));
        const uint32_t count = m_stats.fps_values_count;
        for (uint32_t i = 0; i + 1 < count; ++i) {
            const uint32_t idx0 = (m_stats.fps_offset + i) % count;
            const uint32_t idx1 = (m_stats.fps_offset + i + 1) % count;
            const float v0 = m_stats.fps_values[idx0] / max_val;
            const float v1 = m_stats.fps_values[idx1] / max_val;
            const float x0 = graph_x + static_cast<float>(i) / (count - 1) * graph_w;
            const float x1 = graph_x + static_cast<float>(i + 1) / (count - 1) * graph_w;
            const float y0 = graph_y + graph_h * (1.0f - v0);
            const float y1 = graph_y + graph_h * (1.0f - v1);
            p.drawLine(QPointF(x0, y0), QPointF(x1, y1));
        }
    }
}
