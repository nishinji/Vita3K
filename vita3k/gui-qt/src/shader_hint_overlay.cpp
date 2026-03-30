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

#include <gui-qt/shader_hint_overlay.h>

#include <QPainter>

ShaderHintOverlay::ShaderHintOverlay(QWidget *game_widget, DisplayState &display)
    : GameOverlayBase(game_widget, display, /*transparent_for_input=*/true) {
}

void ShaderHintOverlay::set_text(const QString &text) {
    if (m_text == text)
        return;
    m_text = text;
    on_reposition();
    update();
}

void ShaderHintOverlay::set_vertical_offset(int offset) {
    if (m_vertical_offset == offset)
        return;
    m_vertical_offset = offset;
    on_reposition();
}

QSize ShaderHintOverlay::overlay_size() const {
    QFont f;
    f.setPixelSize(k_font_px);
    const QFontMetrics fm(f);
    const int text_w = fm.horizontalAdvance(m_text.isEmpty() ? QStringLiteral("X") : m_text);
    const int text_h = fm.height();
    return QSize(text_w + k_pad_x * 2, text_h + k_pad_y * 2);
}

QPoint ShaderHintOverlay::overlay_pos() const {
    const QRect vp = viewport_rect();
    const QSize sz = overlay_size();
    const int x = vp.x() + k_margin;
    const int y = vp.y() + vp.height() - sz.height() - k_margin - m_vertical_offset;
    return QPoint(x, y);
}

void ShaderHintOverlay::on_reposition() {
    const QSize sz = overlay_size();
    const QPoint pos = overlay_pos();
    setGeometry(pos.x(), pos.y(), sz.width(), sz.height());
}

void ShaderHintOverlay::paintEvent(QPaintEvent *) {
    if (m_text.isEmpty())
        return;

    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    p.setPen(Qt::NoPen);
    p.setBrush(QColor(15, 15, 15, 150));
    p.drawRoundedRect(rect(), k_radius, k_radius);

    p.setPen(QColor(220, 220, 220));
    QFont f = p.font();
    f.setPixelSize(k_font_px);
    f.setWeight(QFont::Normal);
    p.setFont(f);
    p.drawText(rect(), Qt::AlignCenter, m_text);
}
