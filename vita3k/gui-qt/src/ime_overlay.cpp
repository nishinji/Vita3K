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

#include <gui-qt/ime_overlay.h>
#include <config/state.h>
#include <emuenv/state.h>
#include <ime/functions.h>
#include <util/string_utils.h>

#include <cmath>

#include <QEvent>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QPainter>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSlider>
#include <QTimer>
#include <QVBoxLayout>

namespace {

QString u16_to_qstring(const std::u16string &s) {
    QString q = QString::fromUtf16(reinterpret_cast<const char16_t *>(s.c_str()), static_cast<int>(s.size()));
    // & is a mnemonic marker in qt
    q.replace("&", "&&");
    return q;
}

const QString separator_style = QStringLiteral(
    "background: rgba(120,100,125,180); max-height: 1px; min-height: 1px;");

QString scaled_key_style(qreal s) {
    return QStringLiteral(
        "QPushButton { background: #ffffff; color: #000000;"
        "  border: 1px solid rgba(200,200,200,200); border-radius: %1px;"
        "  font-size: %2px; font-weight: 500; padding: 0 2px; }"
        "QPushButton:pressed { background: rgba(220,220,220,255); }")
        .arg(static_cast<int>(10 * s))
        .arg(static_cast<int>(17 * s));
}

QString scaled_special_char_style(qreal s) {
    return QStringLiteral(
        "QPushButton { background: transparent; color: #ffffff;"
        "  border: none; border-radius: 0;"
        "  font-size: %1px; font-weight: 500; padding: %2px 2px; }"
        "QPushButton:pressed { background: rgba(100,80,105,150); border-radius: %3px; }")
        .arg(static_cast<int>(16 * s))
        .arg(static_cast<int>(4 * s))
        .arg(static_cast<int>(4 * s));
}

QString scaled_action_style(qreal s) {
    return QStringLiteral(
        "QPushButton { background: rgba(155,137,157,255); color: #ffffff;"
        "  border: 1px solid rgba(140,122,142,200); border-radius: %1px;"
        "  font-size: %2px; font-weight: 600; padding: 0 4px; }"
        "QPushButton:pressed { background: rgba(135,117,137,255); }")
        .arg(static_cast<int>(10 * s))
        .arg(static_cast<int>(14 * s));
}

QString scaled_enter_style(qreal s) {
    return QStringLiteral(
        "QPushButton { background: rgba(141,180,83,255); color: #ffffff;"
        "  border: 1px solid rgba(120,160,65,200); border-radius: %1px;"
        "  font-size: %2px; font-weight: 700; padding: 0 4px; }"
        "QPushButton:pressed { background: rgba(120,160,65,255); }")
        .arg(static_cast<int>(10 * s))
        .arg(static_cast<int>(15 * s));
}

QString scaled_close_style(qreal s) {
    return QStringLiteral(
        "QPushButton { background: #000000; color: #ffffff;"
        "  border: 1px solid rgba(40,40,40,200); border-radius: %1px;"
        "  font-size: %2px; font-weight: 600; }"
        "QPushButton:pressed { background: rgba(40,40,40,255); }")
        .arg(static_cast<int>(10 * s))
        .arg(static_cast<int>(14 * s));
}

} // namespace

ImeOverlay::ImeOverlay(QWidget *game_widget, EmuEnvState &emuenv)
    : GameOverlayBase(game_widget, emuenv.display)
    , m_emuenv(emuenv) {
    setObjectName(QStringLiteral("ImeOverlay"));

    m_rebuild_timer = new QTimer(this);
    m_rebuild_timer->setSingleShot(true);
    m_rebuild_timer->setInterval(250);
    connect(m_rebuild_timer, &QTimer::timeout, this, [this]() {
        if (!m_built)
            return;
        build_keyboard();
        m_built_scale = scale_x();
        on_reposition();
    });
}

void ImeOverlay::update_ime() {
    auto &ime = m_emuenv.ime;

    if (!ime.state) {
        if (m_built)
            dismiss();
        return;
    }

    const qreal current_scale = scale_x();
    if (!m_built) {
        build_keyboard();
        m_built_scale = current_scale;
        show_overlay();
    }

    if (m_last_caps_level != ime.caps_level) {
        refresh_key_labels();
        m_last_caps_level = ime.caps_level;
    }
}

void ImeOverlay::dismiss() {
    m_rebuild_timer->stop();
    clear_keyboard();
    hide_overlay();
}

void ImeOverlay::on_reposition() {
    const QRect vp = viewport_rect();

    const qreal sy = scale_y();
    const int kb_height = m_keyboard ? m_keyboard->sizeHint().height() : static_cast<int>(220 * sy);
    const int y = vp.y() + vp.height() - kb_height;
    setGeometry(vp.x(), y, vp.width(), kb_height);

    if (m_keyboard) {
        m_keyboard->setGeometry(0, 0, vp.width(), kb_height);
    }

    if (m_built && m_built_scale > 0
        && std::abs(scale_x() - m_built_scale) > 0.05) {
        m_rebuild_timer->start();
    }
}

void ImeOverlay::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    if (m_numeric_mode)
        p.fillRect(rect(), QColor(59, 42, 61));
    else
        p.fillRect(rect(), QColor(194, 191, 194));
}

bool ImeOverlay::eventFilter(QObject *watched, QEvent *event) {
    if (watched == game_widget()) {
        if (event->type() == QEvent::Destroy || event->type() == QEvent::DeferredDelete) {
            dismiss();
            return true;
        }
    }
    return GameOverlayBase::eventFilter(watched, event);
}

void ImeOverlay::clear_keyboard() {
    if (m_keyboard) {
        m_keyboard->deleteLater();
        m_keyboard = nullptr;
    }
    m_keyboard_layout = nullptr;
    m_row1_keys.clear();
    m_row2_keys.clear();
    m_row3_keys.clear();
    m_numeric_row_keys.clear();
    m_special_keys.clear();
    m_numpad_keys.clear();
    m_special_scroll = nullptr;
    m_special_grid_widget = nullptr;
    m_shift_btn = nullptr;
    m_backspace_btn = nullptr;
    m_close_btn = nullptr;
    m_numeric_btn = nullptr;
    m_space_btn = nullptr;
    m_enter_btn = nullptr;
    m_left_btn = nullptr;
    m_right_btn = nullptr;
    m_punct1_btn = nullptr;
    m_punct2_btn = nullptr;
    m_lang_btn = nullptr;
    m_built = false;
    m_last_caps_level = UINT32_MAX;
}

static QPushButton *make_key(QWidget *parent, const QString &text, const QString &style, int h) {
    auto *btn = new QPushButton(text, parent);
    btn->setStyleSheet(style);
    btn->setFixedHeight(h);
    btn->setFocusPolicy(Qt::NoFocus);
    return btn;
}

void ImeOverlay::build_keyboard() {
    clear_keyboard();

    auto &ime = m_emuenv.ime;
    const qreal s = scale_x();
    const int kh = static_cast<int>(k_key_height * s);
    const int sp = static_cast<int>(k_spacing * s);
    const QString ks = scaled_key_style(s);
    const QString as = scaled_action_style(s);
    const QString cs = scaled_close_style(s);
    const QString es = scaled_enter_style(s);

    m_keyboard = new QWidget(this);
    m_keyboard_layout = new QVBoxLayout(m_keyboard);
    m_keyboard_layout->setContentsMargins(
        static_cast<int>(10 * s), static_cast<int>(8 * s),
        static_cast<int>(10 * s), static_cast<int>(8 * s));
    m_keyboard_layout->setSpacing(sp);

    if (m_numeric_mode) {
        build_numeric_keyboard();
    } else {
        build_letter_keyboard();
    }

    auto *bottom_row = new QHBoxLayout;
    bottom_row->setSpacing(sp);

    m_close_btn = make_key(m_keyboard, QStringLiteral("V"), cs, kh);
    m_close_btn->setFixedWidth(static_cast<int>(44 * s));
    connect(m_close_btn, &QPushButton::clicked, this, [this]() {
        m_emuenv.ime.event_id = SCE_IME_EVENT_PRESS_CLOSE;
    });
    bottom_row->addWidget(m_close_btn);

    m_numeric_btn = make_key(m_keyboard, m_numeric_mode ? QStringLiteral("ABC") : QStringLiteral("@123"), as, kh);
    m_numeric_btn->setFixedWidth(static_cast<int>(90 * s));
    connect(m_numeric_btn, &QPushButton::clicked, this, [this]() {
        m_numeric_mode = !m_numeric_mode;
        m_special_page = 0;
        build_keyboard();
        show_overlay();
    });
    bottom_row->addWidget(m_numeric_btn);

    if (m_numeric_mode) {
        m_space_btn = make_key(m_keyboard, QString::fromStdString(ime.layout.space_str), as, kh);
        connect(m_space_btn, &QPushButton::clicked, this, [this]() {
            ime_update_ponct(m_emuenv.ime, u" ");
        });
        bottom_row->addWidget(m_space_btn, 1);

        auto *zero_btn = make_key(m_keyboard, QStringLiteral("0"), ks, kh);
        zero_btn->setFixedWidth(static_cast<int>(140 * s));
        connect(zero_btn, &QPushButton::clicked, this, [this]() {
            on_key_pressed(u"0");
        });
        bottom_row->addWidget(zero_btn);

        auto *dot_btn = make_key(m_keyboard, QStringLiteral("."), ks, kh);
        dot_btn->setFixedWidth(static_cast<int>(48 * s));
        connect(dot_btn, &QPushButton::clicked, this, [this]() {
            on_ponct_pressed(u".");
        });
        bottom_row->addWidget(dot_btn);
    } else {
        if (m_emuenv.cfg.ime_langs.size() > 1) {
            m_lang_btn = make_key(m_keyboard, QStringLiteral("S/K"), as, kh);
            m_lang_btn->setFixedWidth(static_cast<int>(50 * s));
            connect(m_lang_btn, &QPushButton::clicked, this, [this]() {
                auto &langs = m_emuenv.cfg.ime_langs;
                auto it = std::find(langs.begin(), langs.end(), m_emuenv.cfg.current_ime_lang);
                if (it != langs.end())
                    ++it;
                if (it == langs.end())
                    it = langs.begin();
                m_emuenv.cfg.current_ime_lang = *it;
                init_ime_lang(m_emuenv.ime, static_cast<SceImeLanguage>(*it));
                build_keyboard();
                show_overlay();
            });
            bottom_row->addWidget(m_lang_btn);
        }

        const bool is_shift = ime.caps_level != IME_CAPS_NONE;
        const auto &punct_map = ime.layout.punct;
        if (punct_map.count(IME_ROW_FIRST) && !punct_map.at(IME_ROW_FIRST).empty()) {
            const auto &p = is_shift ? punct_map.at(IME_ROW_FIRST).back() : punct_map.at(IME_ROW_FIRST).front();
            m_punct1_btn = make_key(m_keyboard, u16_to_qstring(p), ks, kh);
            m_punct1_btn->setFixedWidth(static_cast<int>(50 * s));
            connect(m_punct1_btn, &QPushButton::clicked, this, [this]() {
                auto &ime = m_emuenv.ime;
                const bool shift = ime.caps_level != IME_CAPS_NONE;
                const auto &pm = ime.layout.punct;
                if (pm.count(IME_ROW_FIRST) && !pm.at(IME_ROW_FIRST).empty()) {
                    const auto &p = shift ? pm.at(IME_ROW_FIRST).back() : pm.at(IME_ROW_FIRST).front();
                    on_ponct_pressed(p);
                }
            });
            bottom_row->addWidget(m_punct1_btn);
        }

        m_left_btn = make_key(m_keyboard, QStringLiteral("<"), as, kh);
        m_left_btn->setFixedWidth(static_cast<int>(50 * s));
        connect(m_left_btn, &QPushButton::clicked, this, [this]() {
            ime_cursor_left(m_emuenv.ime);
        });
        bottom_row->addWidget(m_left_btn);

        m_space_btn = make_key(m_keyboard, QString::fromStdString(ime.layout.space_str), as, kh);
        connect(m_space_btn, &QPushButton::clicked, this, [this]() {
            ime_update_ponct(m_emuenv.ime, u" ");
        });
        bottom_row->addWidget(m_space_btn, 1);

        m_right_btn = make_key(m_keyboard, QStringLiteral(">"), as, kh);
        m_right_btn->setFixedWidth(static_cast<int>(50 * s));
        connect(m_right_btn, &QPushButton::clicked, this, [this]() {
            ime_cursor_right(m_emuenv.ime);
        });
        bottom_row->addWidget(m_right_btn);

        if (punct_map.count(IME_ROW_SECOND) && !punct_map.at(IME_ROW_SECOND).empty()) {
            const auto &p = is_shift ? punct_map.at(IME_ROW_SECOND).back() : punct_map.at(IME_ROW_SECOND).front();
            m_punct2_btn = make_key(m_keyboard, u16_to_qstring(p), ks, kh);
            m_punct2_btn->setFixedWidth(static_cast<int>(50 * s));
            connect(m_punct2_btn, &QPushButton::clicked, this, [this]() {
                auto &ime = m_emuenv.ime;
                const bool shift = ime.caps_level != IME_CAPS_NONE;
                const auto &pm = ime.layout.punct;
                if (pm.count(IME_ROW_SECOND) && !pm.at(IME_ROW_SECOND).empty()) {
                    const auto &p = shift ? pm.at(IME_ROW_SECOND).back() : pm.at(IME_ROW_SECOND).front();
                    on_ponct_pressed(p);
                }
            });
            bottom_row->addWidget(m_punct2_btn);
        }
    }

    m_enter_btn = make_key(m_keyboard, QString::fromStdString(ime.enter_label), es, kh);
    m_enter_btn->setFixedWidth(static_cast<int>(120 * s));
    connect(m_enter_btn, &QPushButton::clicked, this, [this]() {
        m_emuenv.ime.event_id = SCE_IME_EVENT_PRESS_ENTER;
    });
    bottom_row->addWidget(m_enter_btn);

    m_keyboard_layout->addLayout(bottom_row);

    m_keyboard->show();

    m_built = true;
    m_last_caps_level = ime.caps_level;
}

void ImeOverlay::build_letter_keyboard() {
    auto &ime = m_emuenv.ime;
    const qreal s = scale_x();
    const int kh = static_cast<int>(k_key_height * s);
    const int sp = static_cast<int>(k_spacing * s);
    const int indent = static_cast<int>(k_row2_indent * s);
    const QString ks = scaled_key_style(s);
    const QString as = scaled_action_style(s);

    const auto &layout = ime.layout;
    const bool is_shift = ime.caps_level != IME_CAPS_NONE;
    const auto &keys_map = is_shift ? layout.shift_keys : layout.keys;

    auto *row1 = new QHBoxLayout;
    row1->setSpacing(sp);
    if (keys_map.count(IME_ROW_FIRST)) {
        for (const auto &k : keys_map.at(IME_ROW_FIRST)) {
            auto *btn = make_key(m_keyboard, u16_to_qstring(k), ks, kh);
            const std::u16string key_copy = k;
            connect(btn, &QPushButton::clicked, this, [this, key_copy]() { on_key_pressed(key_copy); });
            row1->addWidget(btn);
            m_row1_keys.append(btn);
        }
    }
    m_keyboard_layout->addLayout(row1);

    auto *row2 = new QHBoxLayout;
    row2->setSpacing(sp);
    row2->addSpacing(indent);
    if (keys_map.count(IME_ROW_SECOND)) {
        for (const auto &k : keys_map.at(IME_ROW_SECOND)) {
            auto *btn = make_key(m_keyboard, u16_to_qstring(k), ks, kh);
            const std::u16string key_copy = k;
            connect(btn, &QPushButton::clicked, this, [this, key_copy]() { on_key_pressed(key_copy); });
            row2->addWidget(btn);
            m_row2_keys.append(btn);
        }
    }
    row2->addSpacing(indent);
    m_keyboard_layout->addLayout(row2);

    auto *row3 = new QHBoxLayout;
    row3->setSpacing(sp);

    m_shift_btn = make_key(m_keyboard, QStringLiteral("Shift"), as, kh);
    m_shift_btn->setFixedWidth(static_cast<int>(90 * s));
    connect(m_shift_btn, &QPushButton::clicked, this, [this]() {
        ime_toggle_shift(m_emuenv.ime);
        refresh_key_labels();
    });
    row3->addWidget(m_shift_btn);

    if (keys_map.count(IME_ROW_THIRD)) {
        for (const auto &k : keys_map.at(IME_ROW_THIRD)) {
            auto *btn = make_key(m_keyboard, u16_to_qstring(k), ks, kh);
            const std::u16string key_copy = k;
            connect(btn, &QPushButton::clicked, this, [this, key_copy]() { on_key_pressed(key_copy); });
            row3->addWidget(btn);
            m_row3_keys.append(btn);
        }
    }

    m_backspace_btn = make_key(m_keyboard, QStringLiteral("Backspace"), as, kh);
    m_backspace_btn->setFixedWidth(static_cast<int>(120 * s));
    connect(m_backspace_btn, &QPushButton::clicked, this, [this]() {
        ime_backspace(m_emuenv.ime);
    });
    row3->addWidget(m_backspace_btn);

    m_keyboard_layout->addLayout(row3);
}

void ImeOverlay::build_numeric_keyboard() {
    const auto &layout = m_emuenv.ime.layout;
    const qreal s = scale_x();
    const int kh = static_cast<int>(k_key_height * s);
    const int sp = static_cast<int>(k_spacing * s);
    const QString ks = scaled_key_style(s);
    const QString scs = scaled_special_char_style(s);
    const QString as = scaled_action_style(s);

    auto *content_row = new QHBoxLayout;
    content_row->setSpacing(sp + static_cast<int>(4 * s));

    const int special_area_height = kh * 3 + sp * 2 + static_cast<int>(8 * s);

    auto *slider = new QSlider(Qt::Vertical, m_keyboard);
    slider->setFixedSize(static_cast<int>(42 * s), special_area_height);
    slider->setInvertedAppearance(true);
    slider->setStyleSheet(QStringLiteral(
        "QSlider { background: transparent; }"
        "QSlider::groove:vertical {"
        "  background: rgba(80,60,85,200); width: %1px; border-radius: %2px;"
        "}"
        "QSlider::handle:vertical {"
        "  background: rgba(180,165,185,240); height: %3px; border-radius: %2px; margin: 0 2px;"
        "}"
        "QSlider::sub-page:vertical, QSlider::add-page:vertical {"
        "  background: transparent;"
        "}")
            .arg(static_cast<int>(36 * s))
            .arg(static_cast<int>(10 * s))
            .arg(static_cast<int>(60 * s)));
    content_row->addWidget(slider, 0, Qt::AlignTop);

    m_special_grid_widget = new QWidget;
    m_special_grid_widget->setAttribute(Qt::WA_TranslucentBackground);
    m_special_grid_widget->setAutoFillBackground(false);
    auto *special_layout = new QVBoxLayout(m_special_grid_widget);
    special_layout->setSpacing(0);
    special_layout->setContentsMargins(static_cast<int>(2 * s), 0, static_cast<int>(2 * s), 0);

    bool first_row = true;
    for (const auto &[row_id, keys] : layout.special_keys) {
        if (!first_row) {
            auto *sep = new QFrame(m_special_grid_widget);
            sep->setStyleSheet(separator_style);
            sep->setFrameShape(QFrame::HLine);
            sep->setFrameShadow(QFrame::Plain);
            special_layout->addWidget(sep);
        }
        first_row = false;

        auto *row_layout = new QHBoxLayout;
        row_layout->setSpacing(sp);
        row_layout->setContentsMargins(0, 0, 0, 0);
        for (const auto &k : keys) {
            auto *btn = make_key(m_special_grid_widget, u16_to_qstring(k), scs, kh);
            btn->setMinimumWidth(static_cast<int>(48 * s));
            const std::u16string key_copy = k;
            connect(btn, &QPushButton::clicked, this, [this, key_copy]() { on_key_pressed(key_copy); });
            row_layout->addWidget(btn);
            m_special_keys.append(btn);
        }
        special_layout->addLayout(row_layout);
    }

    m_special_scroll = new QScrollArea(m_keyboard);
    m_special_scroll->setWidget(m_special_grid_widget);
    m_special_scroll->setWidgetResizable(true);
    m_special_scroll->setFrameShape(QFrame::NoFrame);
    m_special_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_special_scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_special_scroll->setStyleSheet(QStringLiteral("QScrollArea { background: transparent; }"));
    m_special_scroll->viewport()->setAutoFillBackground(false);
    m_special_scroll->viewport()->setStyleSheet(QStringLiteral("background: transparent;"));
    m_special_scroll->setMaximumHeight(special_area_height);

    auto *scroll_bar = m_special_scroll->verticalScrollBar();
    connect(scroll_bar, &QScrollBar::rangeChanged, slider, [slider](int min, int max) {
        slider->setRange(min, max);
    });
    connect(slider, &QSlider::valueChanged, scroll_bar, &QScrollBar::setValue);
    connect(scroll_bar, &QScrollBar::valueChanged, slider, &QSlider::setValue);

    content_row->addWidget(m_special_scroll, 3);

    auto *numpad_layout = new QGridLayout;
    numpad_layout->setSpacing(sp);

    for (const auto &[row_id, keys] : layout.numeric_keys) {
        int col = 0;
        for (const auto &k : keys) {
            auto *btn = make_key(m_keyboard, u16_to_qstring(k), ks, kh);
            btn->setMinimumWidth(static_cast<int>(48 * s));
            const std::u16string key_copy = k;
            connect(btn, &QPushButton::clicked, this, [this, key_copy]() { on_key_pressed(key_copy); });
            numpad_layout->addWidget(btn, row_id - 1, col);
            m_numpad_keys.append(btn);
            ++col;
        }
    }

    m_backspace_btn = make_key(m_keyboard, QStringLiteral("Backspace"), as, kh);
    connect(m_backspace_btn, &QPushButton::clicked, this, [this]() {
        ime_backspace(m_emuenv.ime);
    });
    numpad_layout->addWidget(m_backspace_btn, 2, 3, 1, 2);

    content_row->addLayout(numpad_layout, 2);
    m_keyboard_layout->addLayout(content_row);
}

void ImeOverlay::refresh_key_labels() {
    auto &ime = m_emuenv.ime;
    const auto &layout = ime.layout;
    const bool is_shift = ime.caps_level != IME_CAPS_NONE;
    const auto &keys_map = is_shift ? layout.shift_keys : layout.keys;

    auto update_row = [&](QVector<QPushButton *> &btns, int row_id) {
        if (!keys_map.count(row_id))
            return;
        const auto &row_keys = keys_map.at(row_id);
        for (int i = 0; i < btns.size() && i < static_cast<int>(row_keys.size()); ++i) {
            btns[i]->setText(u16_to_qstring(row_keys[i]));
            btns[i]->disconnect();
            const std::u16string key_copy = row_keys[i];
            connect(btns[i], &QPushButton::clicked, this, [this, key_copy]() { on_key_pressed(key_copy); });
        }
    };

    update_row(m_row1_keys, IME_ROW_FIRST);
    update_row(m_row2_keys, IME_ROW_SECOND);
    update_row(m_row3_keys, IME_ROW_THIRD);

    const auto &punct_map = layout.punct;
    if (m_punct1_btn && punct_map.count(IME_ROW_FIRST) && !punct_map.at(IME_ROW_FIRST).empty()) {
        const auto &p = is_shift ? punct_map.at(IME_ROW_FIRST).back() : punct_map.at(IME_ROW_FIRST).front();
        m_punct1_btn->setText(u16_to_qstring(p));
    }
    if (m_punct2_btn && punct_map.count(IME_ROW_SECOND) && !punct_map.at(IME_ROW_SECOND).empty()) {
        const auto &p = is_shift ? punct_map.at(IME_ROW_SECOND).back() : punct_map.at(IME_ROW_SECOND).front();
        m_punct2_btn->setText(u16_to_qstring(p));
    }

    if (m_shift_btn) {
        if (ime.caps_level == IME_CAPS_LOCK)
            m_shift_btn->setText(QStringLiteral("CAPS"));
        else if (ime.caps_level == IME_CAPS_SHIFT)
            m_shift_btn->setText(QStringLiteral("Shift"));
        else
            m_shift_btn->setText(QStringLiteral("shift"));
    }
}

void ImeOverlay::on_key_pressed(const std::u16string &key) {
    auto &ime = m_emuenv.ime;
    if (ime.str.length() < ime.param.maxTextLength) {
        ime_update_key(ime, key);
        refresh_key_labels();
    }
}

void ImeOverlay::on_ponct_pressed(const std::u16string &key) {
    auto &ime = m_emuenv.ime;
    if (ime.str.length() < ime.param.maxTextLength)
        ime_update_ponct(ime, key);
}
