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

#include <gui-qt/trophy_toast.h>

#include <np/trophy/context.h>

#include <cmath>

#include <QHBoxLayout>
#include <QImage>
#include <QLabel>
#include <QPainter>
#include <QPixmap>
#include <QTimer>
#include <QVBoxLayout>

namespace {

QString grade_icon_path(np::trophy::SceNpTrophyGrade grade) {
    switch (grade) {
    case np::trophy::SceNpTrophyGrade::SCE_NP_TROPHY_GRADE_PLATINUM: return QStringLiteral(":/icons/platinum.png");
    case np::trophy::SceNpTrophyGrade::SCE_NP_TROPHY_GRADE_GOLD: return QStringLiteral(":/icons/gold.png");
    case np::trophy::SceNpTrophyGrade::SCE_NP_TROPHY_GRADE_SILVER: return QStringLiteral(":/icons/silver.png");
    case np::trophy::SceNpTrophyGrade::SCE_NP_TROPHY_GRADE_BRONZE: return QStringLiteral(":/icons/bronze.png");
    default: return {};
    }
}

} // namespace

TrophyToast::TrophyToast(QWidget *game_widget, DisplayState &display)
    : GameOverlayBase(game_widget, display, /*transparent_for_input=*/true) {
    setObjectName(QStringLiteral("TrophyToast"));
    setFixedSize(k_widget_w, k_widget_h);

    setStyleSheet(QStringLiteral(
        "QLabel {"
        "  color: #222;"
        "  background: transparent;"
        "}"
        "#trophy_name {"
        "  font-weight: 600;"
        "  font-size: 13px;"
        "}"
        "#trophy_earned {"
        "  color: #555;"
        "  font-size: 11px;"
        "}"));

    m_icon_label = new QLabel(this);
    m_icon_label->setFixedSize(k_icon_size, k_icon_size);
    m_icon_label->setAlignment(Qt::AlignCenter);
    m_icon_label->setScaledContents(true);

    m_name_label = new QLabel(this);
    m_name_label->setObjectName(QStringLiteral("trophy_name"));
    m_name_label->setWordWrap(true);

    m_grade_icon_label = new QLabel(this);
    m_grade_icon_label->setAlignment(Qt::AlignVCenter | Qt::AlignLeft);
    m_grade_icon_label->hide();

    m_earned_label = new QLabel(tr("You have earned a trophy!"), this);
    m_earned_label->setObjectName(QStringLiteral("trophy_earned"));

    auto *name_row = new QHBoxLayout;
    name_row->setSpacing(6);
    name_row->setContentsMargins(0, 0, 0, 0);
    name_row->addWidget(m_grade_icon_label, 0, Qt::AlignVCenter);
    name_row->addWidget(m_name_label, 1);

    auto *text_col = new QVBoxLayout;
    text_col->setSpacing(1);
    text_col->setContentsMargins(0, 0, 0, 0);
    text_col->addStretch();
    text_col->addLayout(name_row);
    text_col->addWidget(m_earned_label);
    text_col->addStretch();

    auto *root = new QHBoxLayout(this);
    root->setContentsMargins(k_margin, k_margin, k_margin, k_margin);
    root->setSpacing(k_margin);
    root->addWidget(m_icon_label);
    root->addLayout(text_col, 1);

    m_anim = new QPropertyAnimation(this, "slide_x", this);
    m_anim->setEasingCurve(QEasingCurve::OutBack);
    connect(m_anim, &QPropertyAnimation::finished, this, &TrophyToast::on_animation_finished);
}

void TrophyToast::connect_notifer(TrophyNotifier *notifier) {
    connect(notifier, &TrophyNotifier::trophy_unlocked, this, &TrophyToast::on_trophy_unlocked, Qt::QueuedConnection);
}

void TrophyToast::on_trophy_unlocked(TrophyUnlockData data) {
    m_queue.enqueue(std::move(data));
    if (m_stage == Stage::Idle)
        show_next();
}

void TrophyToast::show_next() {
    if (m_queue.isEmpty()) {
        m_stage = Stage::Idle;
        hide_overlay();
        return;
    }

    load_current();
    show_overlay();

    const QRect vp = viewport_rect();
    const qreal sx = scale_x();
    const int sw = static_cast<int>(k_widget_w * sx);
    const int sm = static_cast<int>(k_margin * sx);
    const int stop = static_cast<int>(k_top_margin * sx);
    const int hidden_x = vp.x() + vp.width();
    const int visible_x = vp.x() + vp.width() - sw - sm;
    move(hidden_x, vp.y() + stop);

    m_stage = Stage::SlideIn;
    m_anim->stop();
    m_anim->setDuration(k_slide_ms);
    m_anim->setStartValue(hidden_x);
    m_anim->setEndValue(visible_x);
    m_anim->start();
}

void TrophyToast::load_current() {
    const auto &data = m_queue.head();

    QImage img;
    if (!data.icon_buf.empty())
        img.loadFromData(data.icon_buf.data(), static_cast<int>(data.icon_buf.size()), "PNG");

    if (!img.isNull()) {
        m_icon_label->setPixmap(QPixmap::fromImage(img.scaled(k_icon_size, k_icon_size, Qt::KeepAspectRatio, Qt::SmoothTransformation)));
    } else {
        m_icon_label->clear();
    }

    const auto grade = static_cast<np::trophy::SceNpTrophyGrade>(data.grade);
    const QString icon_path = grade_icon_path(grade);
    if (!icon_path.isEmpty()) {
        QPixmap grade_pixmap(icon_path);
        if (!grade_pixmap.isNull()) {
            const int text_h = m_name_label->fontMetrics().height();
            const QPixmap scaled = grade_pixmap.scaledToHeight(text_h, Qt::SmoothTransformation);
            m_grade_icon_label->setPixmap(scaled);
            m_grade_icon_label->setFixedSize(scaled.size());
            m_grade_icon_label->show();
        } else {
            m_grade_icon_label->clear();
            m_grade_icon_label->hide();
        }
    } else {
        m_grade_icon_label->clear();
        m_grade_icon_label->hide();
    }

    m_name_label->setText(data.name);
}

void TrophyToast::on_animation_finished() {
    switch (m_stage) {
    case Stage::SlideIn:
        m_stage = Stage::Hold;
        QTimer::singleShot(k_hold_ms, this, [this] {
            if (m_stage != Stage::Hold)
                return;
            m_stage = Stage::SlideOut;
            const QRect vp = viewport_rect();
            const qreal sx = scale_x();
            const int sw = static_cast<int>(k_widget_w * sx);
            const int sm = static_cast<int>(k_margin * sx);
            const int visible_x = vp.x() + vp.width() - sw - sm;
            const int hidden_x = vp.x() + vp.width();
            m_anim->stop();
            m_anim->setDuration(k_slide_ms);
            m_anim->setStartValue(visible_x);
            m_anim->setEndValue(hidden_x);
            m_anim->start();
        });
        break;

    case Stage::SlideOut:
        m_queue.dequeue();
        m_stage = Stage::Idle;
        show_next();
        break;

    default:
        break;
    }
}

void TrophyToast::on_reposition() {
    const qreal sx = scale_x();
    if (sx <= 0)
        return;

    if (std::abs(sx - m_applied_scale) > 0.05) {
        m_applied_scale = sx;

        const int sw = static_cast<int>(k_widget_w * sx);
        const int sh = static_cast<int>(k_widget_h * sx);
        const int si = static_cast<int>(k_icon_size * sx);
        const int sm = static_cast<int>(k_margin * sx);

        setFixedSize(sw, sh);
        m_icon_label->setFixedSize(si, si);

        if (auto *lay = qobject_cast<QHBoxLayout *>(layout())) {
            lay->setContentsMargins(sm, sm, sm, sm);
            lay->setSpacing(sm);
        }

        setStyleSheet(QStringLiteral(
            "QLabel { color: #222; background: transparent; }"
            "#trophy_name { font-weight: 600; font-size: %1px; }"
            "#trophy_earned { color: #555; font-size: %2px; }")
                .arg(static_cast<int>(13 * sx))
                .arg(static_cast<int>(11 * sx)));
    }

    const QRect vp = viewport_rect();
    const int sw = static_cast<int>(k_widget_w * sx);
    const int sm = static_cast<int>(k_margin * sx);
    const int stop = static_cast<int>(k_top_margin * sx);
    const int hidden_x = vp.x() + vp.width();
    const int visible_x = vp.x() + vp.width() - sw - sm;
    const int target_y = vp.y() + stop;

    switch (m_stage) {
    case Stage::Idle:
        move(hidden_x, target_y);
        break;
    case Stage::SlideIn:
        m_anim->setEndValue(visible_x);
        move(pos().x(), target_y);
        break;
    case Stage::Hold:
        move(visible_x, target_y);
        break;
    case Stage::SlideOut:
        m_anim->setEndValue(hidden_x);
        move(pos().x(), target_y);
        break;
    }
}

void TrophyToast::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.setBrush(QColor(210, 210, 210));
    p.setPen(Qt::NoPen);
    const qreal sx = m_applied_scale > 0 ? m_applied_scale : 1.0;
    p.drawRoundedRect(rect(), 14 * sx, 14 * sx);
}