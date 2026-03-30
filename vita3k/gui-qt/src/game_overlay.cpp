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

#include <gui-qt/game_overlay.h>

#include <QPainter>

GameOverlay::GameOverlay(QWidget *game_widget, DisplayState &display, OverlayMode mode)
    : GameOverlayBase(game_widget, display, /*transparent_for_input=*/true)
    , m_mode(mode) {
}

void GameOverlay::set_mode(OverlayMode mode) {
    m_mode = mode;
    update();
}

void GameOverlay::set_background(const QImage &img) {
    m_bg = img;
}

void GameOverlay::on_reposition() {
    setGeometry(viewport_rect());
}

void GameOverlay::set_progress(int done, int total, const QString &label) {
    m_done = done;
    m_total = total;
    m_label = label;
    update();
}

void GameOverlay::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.setRenderHint(QPainter::SmoothPixmapTransform);

    switch (m_mode) {
    case OverlayMode::ShaderCompile:
        paint_shader_compile(p);
        break;
    case OverlayMode::Paused:
        paint_paused(p);
        break;
    }
}

void GameOverlay::paint_shader_compile(QPainter &p) {
    if (!m_bg.isNull()) {
        const QImage scaled = m_bg.scaled(size(),
            Qt::KeepAspectRatioByExpanding,
            Qt::SmoothTransformation);
        const QPoint offset(
            (width() - scaled.width()) / 2,
            (height() - scaled.height()) / 2);
        p.drawImage(offset, scaled);
        p.fillRect(rect(), QColor(0, 0, 0, 90));
    } else {
        p.fillRect(rect(), Qt::black);
    }

    if (m_label.isEmpty() && m_total <= 0)
        return;

    const qreal sx = scale_x();
    const qreal sy = scale_y();

    const int card_w = width() * 5 / 8;
    const int card_h = m_total > 0 ? static_cast<int>(80 * sy) : static_cast<int>(46 * sy);
    const int card_x = (width() - card_w) / 2;
    const int card_y = (height() - card_h) / 2;

    p.setPen(Qt::NoPen);
    p.setBrush(QColor(15, 15, 15, 200));
    p.drawRoundedRect(card_x, card_y, card_w, card_h,
        static_cast<int>(8 * sx), static_cast<int>(8 * sy));

    p.setPen(QColor(220, 220, 220));
    QFont f = p.font();
    f.setPixelSize(static_cast<int>(13 * sx));
    f.setWeight(QFont::Normal);
    p.setFont(f);

    const int text_margin = static_cast<int>(16 * sx);
    const int text_top = static_cast<int>(12 * sy);
    const int text_h = static_cast<int>(22 * sy);

    p.drawText(
        QRect(card_x + text_margin, card_y + text_top, card_w - text_margin * 2, text_h),
        Qt::AlignLeft | Qt::AlignVCenter,
        m_label);

    if (m_total <= 0)
        return;

    p.setPen(QColor(160, 160, 160));
    f.setPixelSize(static_cast<int>(12 * sx));
    p.setFont(f);
    p.drawText(
        QRect(card_x + text_margin, card_y + text_top, card_w - text_margin * 2, text_h),
        Qt::AlignRight | Qt::AlignVCenter,
        QStringLiteral("%1 / %2").arg(m_done).arg(m_total));

    const int bar_margin = text_margin;
    const int bar_x = card_x + bar_margin;
    const int bar_y = card_y + static_cast<int>(46 * sy);
    const int bar_w = card_w - bar_margin * 2;
    const int bar_h = static_cast<int>(4 * sy);

    p.setBrush(QColor(60, 60, 60));
    p.drawRoundedRect(bar_x, bar_y, bar_w, bar_h, 2, 2);

    if (m_done > 0) {
        const int filled = bar_w * m_done / m_total;
        p.setBrush(QColor(255, 255, 255, 220));
        p.drawRoundedRect(bar_x, bar_y, filled, bar_h, 2, 2);
    }

    p.setPen(QColor(140, 140, 140));
    f.setPixelSize(static_cast<int>(11 * sx));
    p.setFont(f);
    const int pct = m_total > 0 ? (m_done * 100 / m_total) : 0;
    p.drawText(
        QRect(bar_x, bar_y + static_cast<int>(10 * sy), bar_w, static_cast<int>(18 * sy)),
        Qt::AlignCenter,
        QStringLiteral("%1%").arg(pct));
}

void GameOverlay::paint_paused(QPainter &p) {
    p.fillRect(rect(), QColor(0, 0, 0, 30));

    const qreal sx = scale_x();
    const qreal sy = scale_y();

    const int card_w = static_cast<int>(280 * sx);
    const int card_h = static_cast<int>(72 * sy);
    const int card_x = (width() - card_w) / 2;
    const int card_y = (height() - card_h) / 2;

    p.setPen(Qt::NoPen);
    p.setBrush(QColor(15, 15, 15, 170));
    p.drawRoundedRect(card_x, card_y, card_w, card_h,
        static_cast<int>(8 * sx), static_cast<int>(8 * sy));

    p.setPen(QColor(220, 220, 220));
    QFont f = p.font();
    f.setPixelSize(static_cast<int>(16 * sx));
    f.setWeight(QFont::DemiBold);
    p.setFont(f);

    p.drawText(
        QRect(card_x, card_y + static_cast<int>(10 * sy), card_w, static_cast<int>(26 * sy)),
        Qt::AlignCenter,
        QStringLiteral("Emulation Paused"));

    p.setPen(QColor(160, 160, 160));
    f.setPixelSize(static_cast<int>(12 * sx));
    f.setWeight(QFont::Normal);
    p.setFont(f);

    p.drawText(
        QRect(card_x, card_y + static_cast<int>(38 * sy), card_w, static_cast<int>(22 * sy)),
        Qt::AlignCenter,
        QStringLiteral("Press PS BUTTON to continue"));
}
