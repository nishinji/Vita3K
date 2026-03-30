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

#include "ui_controls_dialog.h"

#include <gui-qt/controls_dialog.h>

#include <config/functions.h>
#include <config/state.h>
#include <ctrl/functions.h>
#include <ctrl/state.h>

#include <QCheckBox>
#include <QGroupBox>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLabel>
#include <QMessageBox>
#include <QPainter>
#include <QSlider>
#include <QSpinBox>
#include <QSvgRenderer>

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_gamepad.h>

QString ControlsDialog::key_name(int qt_key) {
    if (qt_key == 0)
        return tr("Unset");
    return QKeySequence(qt_key).toString(QKeySequence::NativeText);
}

QString ControlsDialog::controller_button_name(SDL_GamepadType type, SDL_GamepadButton btn) {
    switch (btn) {
    case SDL_GAMEPAD_BUTTON_DPAD_UP: return QStringLiteral("D-Pad Up");
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN: return QStringLiteral("D-Pad Down");
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT: return QStringLiteral("D-Pad Left");
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return QStringLiteral("D-Pad Right");
    default: break;
    }

    switch (type) {
    case SDL_GAMEPAD_TYPE_XBOX360:
    case SDL_GAMEPAD_TYPE_XBOXONE:
        switch (btn) {
        case SDL_GAMEPAD_BUTTON_BACK: return QStringLiteral("Back");
        case SDL_GAMEPAD_BUTTON_START: return QStringLiteral("Start");
        case SDL_GAMEPAD_BUTTON_LEFT_STICK: return QStringLiteral("LS");
        case SDL_GAMEPAD_BUTTON_RIGHT_STICK: return QStringLiteral("RS");
        case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: return QStringLiteral("LB");
        case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return QStringLiteral("RB");
        case SDL_GAMEPAD_BUTTON_SOUTH: return QStringLiteral("A");
        case SDL_GAMEPAD_BUTTON_EAST: return QStringLiteral("B");
        case SDL_GAMEPAD_BUTTON_WEST: return QStringLiteral("X");
        case SDL_GAMEPAD_BUTTON_NORTH: return QStringLiteral("Y");
        case SDL_GAMEPAD_BUTTON_GUIDE: return QStringLiteral("Guide");
        default: break;
        }
        break;
    case SDL_GAMEPAD_TYPE_STANDARD:
    case SDL_GAMEPAD_TYPE_PS3:
    case SDL_GAMEPAD_TYPE_PS4:
    case SDL_GAMEPAD_TYPE_PS5:
        switch (btn) {
        case SDL_GAMEPAD_BUTTON_BACK: return QStringLiteral("Select");
        case SDL_GAMEPAD_BUTTON_START: return QStringLiteral("Start");
        case SDL_GAMEPAD_BUTTON_LEFT_STICK: return QStringLiteral("L3");
        case SDL_GAMEPAD_BUTTON_RIGHT_STICK: return QStringLiteral("R3");
        case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: return QStringLiteral("L1");
        case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return QStringLiteral("R1");
        case SDL_GAMEPAD_BUTTON_SOUTH: return QStringLiteral("Cross");
        case SDL_GAMEPAD_BUTTON_EAST: return QStringLiteral("Circle");
        case SDL_GAMEPAD_BUTTON_WEST: return QStringLiteral("Square");
        case SDL_GAMEPAD_BUTTON_NORTH: return QStringLiteral("Triangle");
        case SDL_GAMEPAD_BUTTON_GUIDE: return QStringLiteral("PS");
        default: break;
        }
        break;
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO:
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT:
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT:
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR:
        switch (btn) {
        case SDL_GAMEPAD_BUTTON_BACK: return QStringLiteral("-");
        case SDL_GAMEPAD_BUTTON_START: return QStringLiteral("+");
        case SDL_GAMEPAD_BUTTON_LEFT_STICK: return QStringLiteral("LS");
        case SDL_GAMEPAD_BUTTON_RIGHT_STICK: return QStringLiteral("RS");
        case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: return QStringLiteral("L");
        case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return QStringLiteral("R");
        case SDL_GAMEPAD_BUTTON_SOUTH: return QStringLiteral("B");
        case SDL_GAMEPAD_BUTTON_EAST: return QStringLiteral("A");
        case SDL_GAMEPAD_BUTTON_WEST: return QStringLiteral("Y");
        case SDL_GAMEPAD_BUTTON_NORTH: return QStringLiteral("X");
        case SDL_GAMEPAD_BUTTON_GUIDE: return QStringLiteral("Home");
        default: break;
        }
        break;
    default:
        break;
    }

    const char *name = SDL_GetGamepadStringForButton(btn);
    return name ? QString::fromUtf8(name) : tr("Button %1").arg(static_cast<int>(btn));
}

QString ControlsDialog::controller_axis_name(SDL_GamepadType type, SDL_GamepadAxis axis) {
    switch (axis) {
    case SDL_GAMEPAD_AXIS_LEFTX: return QStringLiteral("Left X");
    case SDL_GAMEPAD_AXIS_LEFTY: return QStringLiteral("Left Y");
    case SDL_GAMEPAD_AXIS_RIGHTX: return QStringLiteral("Right X");
    case SDL_GAMEPAD_AXIS_RIGHTY: return QStringLiteral("Right Y");
    case SDL_GAMEPAD_AXIS_LEFT_TRIGGER:
        switch (type) {
        case SDL_GAMEPAD_TYPE_STANDARD:
        case SDL_GAMEPAD_TYPE_PS3:
        case SDL_GAMEPAD_TYPE_PS4:
        case SDL_GAMEPAD_TYPE_PS5:
            return QStringLiteral("L2");
        case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO:
        case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT:
        case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT:
        case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR:
            return QStringLiteral("ZL");
        default: return QStringLiteral("LT");
        }
    case SDL_GAMEPAD_AXIS_RIGHT_TRIGGER:
        switch (type) {
        case SDL_GAMEPAD_TYPE_STANDARD:
        case SDL_GAMEPAD_TYPE_PS3:
        case SDL_GAMEPAD_TYPE_PS4:
        case SDL_GAMEPAD_TYPE_PS5:
            return QStringLiteral("R2");
        case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO:
        case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT:
        case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT:
        case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR:
            return QStringLiteral("ZR");
        default: return QStringLiteral("RT");
        }
    default:
        return tr("Axis %1").arg(static_cast<int>(axis));
    }
}

void ControlsDialog::save_config() {
    config::serialize_config(m_emuenv.cfg, m_emuenv.cfg.config_path);
}

namespace {

QPixmap render_vita_svg() {
    static QPixmap cached;
    if (!cached.isNull())
        return cached;
    QSvgRenderer renderer(QStringLiteral(":/icons/PSV_Layout.svg"));
    QPixmap px(renderer.defaultSize() * 10);
    px.fill(Qt::transparent);
    QPainter painter(&px);
    painter.setRenderHints(QPainter::TextAntialiasing | QPainter::Antialiasing | QPainter::SmoothPixmapTransform);
    renderer.render(&painter, px.rect());
    painter.end();
    cached = px.scaled(550, 550, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    return cached;
}

QPushButton *make_bind_button() {
    auto *btn = new QPushButton();
    btn->setAutoDefault(false);
    btn->setFocusPolicy(Qt::ClickFocus);
    return btn;
}

QGroupBox *make_button_group(const QString &title, QPushButton *&out_btn) {
    out_btn = make_bind_button();
    auto *gb = new QGroupBox(title);
    auto *lay = new QVBoxLayout(gb);
    lay->setContentsMargins(5, 5, 5, 5);
    lay->addWidget(out_btn);
    return gb;
}

QWidget *make_diamond_widget(const QString &title,
    QPushButton *&btn_up, QPushButton *&btn_down,
    QPushButton *&btn_left, QPushButton *&btn_right) {
    auto *gb = new QGroupBox(title);
    auto *vlay = new QVBoxLayout(gb);
    vlay->setSpacing(6);
    vlay->setContentsMargins(5, 5, 5, 5);

    auto *up_row = new QHBoxLayout();
    up_row->setContentsMargins(0, 0, 0, 0);
    up_row->addStretch();
    up_row->addWidget(make_button_group(QObject::tr("Up"), btn_up));
    up_row->addStretch();
    vlay->addLayout(up_row);

    auto *lr_row = new QHBoxLayout();
    lr_row->addWidget(make_button_group(QObject::tr("Left"), btn_left));
    lr_row->addWidget(make_button_group(QObject::tr("Right"), btn_right));
    vlay->addLayout(lr_row);

    auto *down_row = new QHBoxLayout();
    down_row->setContentsMargins(0, 0, 0, 0);
    down_row->addStretch();
    down_row->addWidget(make_button_group(QObject::tr("Down"), btn_down));
    down_row->addStretch();
    vlay->addLayout(down_row);

    return gb;
}

} // namespace

ControlsDialog::ControlsDialog(EmuEnvState &emuenv, QWidget *parent)
    : QDialog(parent)
    , m_emuenv(emuenv)
    , m_ui(std::make_unique<Ui::ControlsDialog>()) {
    m_ui->setupUi(this);
    setWindowTitle(tr("Controls"));

    setStyleSheet(QStringLiteral(
        "QGroupBox { font-weight: bold; border: 1px solid gray; border-radius: 4px; margin-top: 0.5em; padding-top: 0.4em; }"
        "QGroupBox::title { subcontrol-origin: margin; subcontrol-position: top center; padding: 0 4px; }"));

    m_ui->l_controller_image->setPixmap(render_vita_svg());

    init_keyboard_bindings();
    refresh_all_keyboard_labels();
    connect(m_ui->btn_reset_keyboard, &QPushButton::clicked, this, &ControlsDialog::reset_keyboard_defaults);
    connect(m_ui->btn_close_keyboard, &QPushButton::clicked, this, &QDialog::reject);

    for (int i = 0; i < MAX_CONTROLLERS; ++i)
        build_controller_tab(i);

    m_sdl_poll_timer = new QTimer(this);
    m_sdl_poll_timer->setInterval(16);
    connect(m_sdl_poll_timer, &QTimer::timeout, this, &ControlsDialog::poll_sdl_events);
    m_sdl_poll_timer->start();

    connect(m_ui->tab_widget, &QTabWidget::currentChanged, this, [this](int) {
        cancel_keyboard_capture();
        cancel_controller_capture();
    });

    refresh_controller_tabs();
}

ControlsDialog::~ControlsDialog() {
    m_sdl_poll_timer->stop();
    cancel_keyboard_capture();
    cancel_controller_capture();
}

void ControlsDialog::init_keyboard_bindings() {
    auto &cfg = m_emuenv.cfg;

    m_kb_bindings = {
        { m_ui->btn_dpad_up, &cfg.keyboard_button_up, 16777235 },
        { m_ui->btn_dpad_down, &cfg.keyboard_button_down, 16777237 },
        { m_ui->btn_dpad_left, &cfg.keyboard_button_left, 16777234 },
        { m_ui->btn_dpad_right, &cfg.keyboard_button_right, 16777236 },
        { m_ui->btn_cross, &cfg.keyboard_button_cross, 88 },
        { m_ui->btn_circle, &cfg.keyboard_button_circle, 67 },
        { m_ui->btn_square, &cfg.keyboard_button_square, 90 },
        { m_ui->btn_triangle, &cfg.keyboard_button_triangle, 86 },
        { m_ui->btn_l1, &cfg.keyboard_button_l1, 81 },
        { m_ui->btn_r1, &cfg.keyboard_button_r1, 69 },
        { m_ui->btn_select, &cfg.keyboard_button_select, 16777251 },
        { m_ui->btn_start, &cfg.keyboard_button_start, 16777220 },
        { m_ui->btn_psbutton, &cfg.keyboard_button_psbutton, 80 },
        { m_ui->btn_lstick_up, &cfg.keyboard_leftstick_up, 87 },
        { m_ui->btn_lstick_down, &cfg.keyboard_leftstick_down, 83 },
        { m_ui->btn_lstick_left, &cfg.keyboard_leftstick_left, 65 },
        { m_ui->btn_lstick_right, &cfg.keyboard_leftstick_right, 68 },
        { m_ui->btn_rstick_up, &cfg.keyboard_rightstick_up, 73 },
        { m_ui->btn_rstick_down, &cfg.keyboard_rightstick_down, 75 },
        { m_ui->btn_rstick_left, &cfg.keyboard_rightstick_left, 74 },
        { m_ui->btn_rstick_right, &cfg.keyboard_rightstick_right, 76 },
        { m_ui->btn_l2, &cfg.keyboard_button_l2, 85 },
        { m_ui->btn_r2, &cfg.keyboard_button_r2, 79 },
        { m_ui->btn_l3, &cfg.keyboard_button_l3, 70 },
        { m_ui->btn_r3, &cfg.keyboard_button_r3, 72 },
        { m_ui->btn_fullscreen, &cfg.keyboard_gui_fullscreen, 16777274 },
        { m_ui->btn_toggle_touch, &cfg.keyboard_gui_toggle_touch, 84 },
        { m_ui->btn_tex_replace, &cfg.keyboard_toggle_texture_replacement, 0 },
        { m_ui->btn_screenshot, &cfg.keyboard_take_screenshot, 0 },
        { m_ui->btn_pinch_mod, &cfg.keyboard_pinch_modifier, 0 },
        { m_ui->btn_alt_pinch_in, &cfg.keyboard_alternate_pinch_in, 0 },
        { m_ui->btn_alt_pinch_out, &cfg.keyboard_alternate_pinch_out, 0 },
    };

    for (int i = 0; i < static_cast<int>(m_kb_bindings.size()); ++i) {
        auto *btn = m_kb_bindings[i].button;
        btn->setFocusPolicy(Qt::ClickFocus);
        btn->installEventFilter(this);
        connect(btn, &QPushButton::clicked, this, [this, i]() { start_keyboard_capture(i); });
    }
}

void ControlsDialog::refresh_all_keyboard_labels() {
    for (auto &b : m_kb_bindings)
        b.button->setText(key_name(*b.config_value));
}

void ControlsDialog::start_keyboard_capture(int index) {
    cancel_keyboard_capture();
    cancel_controller_capture();
    m_kb_capturing_index = index;
    auto *btn = m_kb_bindings[index].button;
    btn->setText(tr("[ Press a key\u2026 ]"));
    QPalette pal = btn->palette();
    pal.setColor(QPalette::Button, QColor(60, 100, 180));
    pal.setColor(QPalette::ButtonText, Qt::white);
    btn->setPalette(pal);
    grabKeyboard();
}

void ControlsDialog::cancel_keyboard_capture() {
    if (m_kb_capturing_index < 0)
        return;
    releaseKeyboard();
    auto &b = m_kb_bindings[m_kb_capturing_index];
    b.button->setPalette(QPalette());
    b.button->setText(key_name(*b.config_value));
    m_kb_capturing_index = -1;
}

void ControlsDialog::keyPressEvent(QKeyEvent *event) {
    if (m_kb_capturing_index < 0) {
        QDialog::keyPressEvent(event);
        return;
    }
    if (event->isAutoRepeat())
        return;
    const int key = event->key();
    if (key == Qt::Key_Escape) {
        cancel_keyboard_capture();
        return;
    }
    if (has_duplicate_key(key, m_kb_capturing_index)) {
        QMessageBox::warning(this, tr("Duplicate Key"),
            tr("The key \"%1\" is already assigned to another action.").arg(key_name(key)));
        return;
    }
    auto &b = m_kb_bindings[m_kb_capturing_index];
    *b.config_value = key;
    save_config();
    cancel_keyboard_capture();
    refresh_all_keyboard_labels();
}

bool ControlsDialog::eventFilter(QObject *watched, QEvent *event) {
    if (event->type() == QEvent::MouseButtonRelease) {
        auto *me = static_cast<QMouseEvent *>(event);
        if (me->button() == Qt::RightButton && m_kb_capturing_index < 0) {
            for (int i = 0; i < static_cast<int>(m_kb_bindings.size()); ++i) {
                if (m_kb_bindings[i].button == watched) {
                    *m_kb_bindings[i].config_value = 0;
                    m_kb_bindings[i].button->setText(key_name(0));
                    save_config();
                    return true;
                }
            }
        }
    }
    return QDialog::eventFilter(watched, event);
}

bool ControlsDialog::has_duplicate_key(int qt_key, int exclude_index) const {
    if (qt_key == 0)
        return false;
    for (int i = 0; i < static_cast<int>(m_kb_bindings.size()); ++i) {
        if (i != exclude_index && *m_kb_bindings[i].config_value == qt_key)
            return true;
    }
    return false;
}

void ControlsDialog::reset_keyboard_defaults() {
    for (auto &b : m_kb_bindings)
        *b.config_value = b.default_value;
    save_config();
    refresh_all_keyboard_labels();
}

void ControlsDialog::build_controller_tab(int index) {
    auto &t = m_ctrl_tabs[index];
    QWidget *tab_widgets[] = { m_ui->tab_controller_1, m_ui->tab_controller_2, m_ui->tab_controller_3, m_ui->tab_controller_4 };
    t.tab = tab_widgets[index];

    auto *tab_layout = t.tab->layout();
    if (!tab_layout)
        tab_layout = new QVBoxLayout(t.tab);
    auto *v_tab_layout = qobject_cast<QVBoxLayout *>(tab_layout);
    v_tab_layout->setSpacing(5);
    v_tab_layout->setContentsMargins(5, 5, 5, 5);

    t.lbl_controller_name = new QLabel(tr("No controller connected."));
    v_tab_layout->addWidget(t.lbl_controller_name);

    auto *main_area = new QHBoxLayout();
    main_area->setSpacing(0);

    auto *left_col = new QVBoxLayout();
    left_col->setSpacing(5);
    left_col->addWidget(make_diamond_widget(tr("D-Pad"), t.btn_dpad_up, t.btn_dpad_down, t.btn_dpad_left, t.btn_dpad_right));
    left_col->addWidget(make_diamond_widget(tr("Left Stick"), t.btn_lstick_up, t.btn_lstick_down, t.btn_lstick_left, t.btn_lstick_right));
    left_col->addStretch();
    main_area->addLayout(left_col);

    auto *center_col = new QVBoxLayout();
    center_col->setSpacing(5);

    auto *shoulder_row = new QHBoxLayout();
    shoulder_row->addWidget(make_button_group(tr("L"), t.btn_l1));
    shoulder_row->addStretch();
    shoulder_row->addWidget(make_button_group(tr("R"), t.btn_r1));
    center_col->addLayout(shoulder_row);

    t.l_controller_image = new QLabel();
    t.l_controller_image->setAlignment(Qt::AlignCenter);
    t.l_controller_image->setMinimumHeight(250);
    t.l_controller_image->setPixmap(render_vita_svg());
    center_col->addWidget(t.l_controller_image);

    auto *below_row = new QHBoxLayout();
    below_row->setSpacing(10);
    below_row->addWidget(make_button_group(tr("PS Button"), t.btn_psbutton));
    below_row->addStretch();
    below_row->addWidget(make_button_group(tr("Select"), t.btn_select));
    below_row->addWidget(make_button_group(tr("Start"), t.btn_start));
    center_col->addLayout(below_row);

    auto *pstv_group = new QGroupBox(tr("PS TV Mode"));
    auto *pstv_layout = new QHBoxLayout(pstv_group);
    pstv_layout->setSpacing(10);
    pstv_layout->addStretch();
    pstv_layout->addWidget(make_button_group(tr("L2"), t.btn_l2));
    pstv_layout->addWidget(make_button_group(tr("L3"), t.btn_l3));
    pstv_layout->addWidget(make_button_group(tr("R3"), t.btn_r3));
    pstv_layout->addWidget(make_button_group(tr("R2"), t.btn_r2));
    pstv_layout->addStretch();
    center_col->addWidget(pstv_group);

    auto *analog_group = new QGroupBox(tr("Analog Stick Multiplier"));
    auto *analog_layout = new QHBoxLayout(analog_group);
    t.slider_analog_multiplier = new QSlider(Qt::Horizontal);
    t.slider_analog_multiplier->setRange(10, 200);
    t.slider_analog_multiplier->setValue(100);
    t.slider_analog_multiplier->setTickPosition(QSlider::TicksBelow);
    t.slider_analog_multiplier->setTickInterval(50);
    t.lbl_analog_multiplier_value = new QLabel(QStringLiteral("1.0\u00d7"));
    t.lbl_analog_multiplier_value->setMinimumWidth(40);
    t.lbl_analog_multiplier_value->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    analog_layout->addWidget(t.slider_analog_multiplier);
    analog_layout->addWidget(t.lbl_analog_multiplier_value);
    center_col->addWidget(analog_group);

    const int ci = index;
    connect(t.slider_analog_multiplier, &QSlider::valueChanged, this, [this, ci](int v) {
        m_ctrl_tabs[ci].lbl_analog_multiplier_value->setText(QStringLiteral("%1\u00d7").arg(v / 100.0, 0, 'f', 1));
        m_emuenv.cfg.controller_analog_multiplier = v / 100.0f;
        save_config();
    });

    t.chk_disable_motion = new QCheckBox(tr("Disable Motion Controls"));
    center_col->addWidget(t.chk_disable_motion);
    connect(t.chk_disable_motion, &QCheckBox::toggled, this, [this](bool checked) { m_emuenv.cfg.disable_motion = checked; save_config(); });

    center_col->addStretch();
    main_area->addLayout(center_col, 1);

    auto *right_col = new QVBoxLayout();
    right_col->setSpacing(5);
    right_col->addWidget(make_diamond_widget(tr("Face Buttons"), t.btn_triangle, t.btn_cross, t.btn_square, t.btn_circle));
    right_col->addWidget(make_diamond_widget(tr("Right Stick"), t.btn_rstick_up, t.btn_rstick_down, t.btn_rstick_left, t.btn_rstick_right));
    right_col->addStretch();
    main_area->addLayout(right_col);

    v_tab_layout->addLayout(main_area, 1);

    auto *bottom_bar = new QHBoxLayout();
    t.btn_reset = new QPushButton(tr("Reset Controller Bindings"));
    t.btn_reset->setAutoDefault(false);
    bottom_bar->addWidget(t.btn_reset);
    bottom_bar->addStretch();
    auto *btn_close = new QPushButton(tr("Close"));
    btn_close->setAutoDefault(false);
    bottom_bar->addWidget(btn_close);
    v_tab_layout->addLayout(bottom_bar);

    connect(t.btn_reset, &QPushButton::clicked, this, [this, ci]() { reset_controller_defaults(ci); });
    connect(btn_close, &QPushButton::clicked, this, &QDialog::reject);

    t.bindings = {
        { t.btn_l1, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER },
        { t.btn_dpad_up, SDL_GAMEPAD_BUTTON_DPAD_UP },
        { t.btn_dpad_down, SDL_GAMEPAD_BUTTON_DPAD_DOWN },
        { t.btn_dpad_left, SDL_GAMEPAD_BUTTON_DPAD_LEFT },
        { t.btn_dpad_right, SDL_GAMEPAD_BUTTON_DPAD_RIGHT },
        { t.btn_select, SDL_GAMEPAD_BUTTON_BACK },
        { t.btn_psbutton, SDL_GAMEPAD_BUTTON_GUIDE },
        { t.btn_r1, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER },
        { t.btn_triangle, SDL_GAMEPAD_BUTTON_NORTH },
        { t.btn_cross, SDL_GAMEPAD_BUTTON_SOUTH },
        { t.btn_square, SDL_GAMEPAD_BUTTON_WEST },
        { t.btn_circle, SDL_GAMEPAD_BUTTON_EAST },
        { t.btn_start, SDL_GAMEPAD_BUTTON_START },
        { t.btn_l3, SDL_GAMEPAD_BUTTON_LEFT_STICK },
        { t.btn_r3, SDL_GAMEPAD_BUTTON_RIGHT_STICK },
    };

    for (int bi = 0; bi < static_cast<int>(t.bindings.size()); ++bi) {
        connect(t.bindings[bi].button, &QPushButton::clicked, this, [this, ci, bi]() { start_controller_capture(ci, bi); });
    }

    t.axis_bindings = {
        { t.btn_lstick_up, 1 },
        { t.btn_lstick_down, 1 },
        { t.btn_lstick_left, 0 },
        { t.btn_lstick_right, 0 },
        { t.btn_rstick_up, 3 },
        { t.btn_rstick_down, 3 },
        { t.btn_rstick_left, 2 },
        { t.btn_rstick_right, 2 },
        { t.btn_l2, 4 },
        { t.btn_r2, 5 },
    };

    for (int ai = 0; ai < static_cast<int>(t.axis_bindings.size()); ++ai) {
        connect(t.axis_bindings[ai].button, &QPushButton::clicked, this, [this, ci, ai]() { start_axis_capture(ci, ai); });
    }

    set_controller_tab_enabled(index, false);
}

void ControlsDialog::refresh_controller_tabs() {
    auto &ctrl = m_emuenv.ctrl;
    for (int i = 0; i < MAX_CONTROLLERS; ++i) {
        m_ctrl_tabs[i].connected = false;
        m_ctrl_tabs[i].port = -1;
        m_ctrl_tabs[i].guid = {};
        m_ctrl_tabs[i].type = SDL_GAMEPAD_TYPE_STANDARD;
    }
    for (const auto &[guid, controller] : ctrl.controllers) {
        const int port = controller.port;
        if (port < 0 || port >= MAX_CONTROLLERS)
            continue;
        auto &t = m_ctrl_tabs[port];
        t.connected = true;
        t.port = port;
        t.guid = guid;
        t.type = SDL_GetGamepadType(controller.controller.get());
        t.lbl_controller_name->setText(tr("Controller: %1").arg(QString::fromUtf8(controller.name)));
    }
    for (int i = 0; i < MAX_CONTROLLERS; ++i) {
        auto &t = m_ctrl_tabs[i];
        set_controller_tab_enabled(i, t.connected);
        if (!t.connected)
            t.lbl_controller_name->setText(tr("No controller connected."));
        t.slider_analog_multiplier->setValue(static_cast<int>(m_emuenv.cfg.controller_analog_multiplier * 100.0f));
        t.chk_disable_motion->setChecked(m_emuenv.cfg.disable_motion);
        refresh_controller_tab_labels(i);
    }
}

void ControlsDialog::refresh_controller_tab_labels(int index) {
    auto &t = m_ctrl_tabs[index];
    auto &binds = m_emuenv.cfg.controller_binds;
    for (auto &cb : t.bindings) {
        if (cb.vita_button == SDL_GAMEPAD_BUTTON_INVALID) {
            cb.button->setText(QStringLiteral("-"));
            continue;
        }
        const int idx = static_cast<int>(cb.vita_button);
        SDL_GamepadButton mapped = cb.vita_button;
        if (idx >= 0 && idx < static_cast<int>(binds.size()))
            mapped = static_cast<SDL_GamepadButton>(binds[idx]);
        cb.button->setText(controller_button_name(t.type, mapped));
    }

    auto &axis_binds = m_emuenv.cfg.controller_axis_binds;
    for (auto &ab : t.axis_bindings) {
        const int ai = ab.axis_index;
        SDL_GamepadAxis mapped = static_cast<SDL_GamepadAxis>(ai);
        if (ai >= 0 && ai < static_cast<int>(axis_binds.size()))
            mapped = static_cast<SDL_GamepadAxis>(axis_binds[ai]);
        ab.button->setText(controller_axis_name(t.type, mapped));
    }
}

void ControlsDialog::set_controller_tab_enabled(int index, bool enabled) {
    auto &t = m_ctrl_tabs[index];
    for (auto &cb : t.bindings)
        cb.button->setEnabled(enabled);
    for (auto &ab : t.axis_bindings)
        ab.button->setEnabled(enabled);
    t.slider_analog_multiplier->setEnabled(enabled);
    t.chk_disable_motion->setEnabled(enabled);
    t.btn_reset->setEnabled(enabled);
}

void ControlsDialog::start_controller_capture(int tab_index, int binding_index) {
    cancel_keyboard_capture();
    cancel_controller_capture();
    m_ctrl_capturing_tab = tab_index;
    m_ctrl_capturing_index = binding_index;
    m_ctrl_capturing_axis = false;
    auto *btn = m_ctrl_tabs[tab_index].bindings[binding_index].button;
    btn->setText(tr("[ Press a button\u2026 ]"));
    QPalette pal = btn->palette();
    pal.setColor(QPalette::Button, QColor(60, 100, 180));
    pal.setColor(QPalette::ButtonText, Qt::white);
    btn->setPalette(pal);
    m_sdl_poll_timer->start();
}

void ControlsDialog::start_axis_capture(int tab_index, int axis_binding_index) {
    cancel_keyboard_capture();
    cancel_controller_capture();
    m_ctrl_capturing_tab = tab_index;
    m_ctrl_capturing_index = axis_binding_index;
    m_ctrl_capturing_axis = true;
    auto *btn = m_ctrl_tabs[tab_index].axis_bindings[axis_binding_index].button;
    btn->setText(tr("[ Move an axis\u2026 ]"));
    QPalette pal = btn->palette();
    pal.setColor(QPalette::Button, QColor(60, 100, 180));
    pal.setColor(QPalette::ButtonText, Qt::white);
    btn->setPalette(pal);
    m_sdl_poll_timer->start();
}

void ControlsDialog::cancel_controller_capture() {
    if (m_ctrl_capturing_tab < 0 || m_ctrl_capturing_index < 0)
        return;
    auto &t = m_ctrl_tabs[m_ctrl_capturing_tab];
    if (m_ctrl_capturing_axis)
        t.axis_bindings[m_ctrl_capturing_index].button->setPalette(QPalette());
    else
        t.bindings[m_ctrl_capturing_index].button->setPalette(QPalette());
    m_ctrl_capturing_tab = -1;
    m_ctrl_capturing_index = -1;
    m_ctrl_capturing_axis = false;
    for (int i = 0; i < MAX_CONTROLLERS; ++i) {
        if (m_ctrl_tabs[i].connected)
            refresh_controller_tab_labels(i);
    }
}

void ControlsDialog::poll_sdl_events() {
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        switch (ev.type) {
        case SDL_EVENT_GAMEPAD_ADDED:
        case SDL_EVENT_GAMEPAD_REMOVED:
            refresh_controllers(m_emuenv.ctrl, m_emuenv);
            refresh_controller_tabs();
            break;
        case SDL_EVENT_KEY_DOWN:
            if (m_ctrl_capturing_tab >= 0 && m_ctrl_capturing_index >= 0) {
                cancel_controller_capture();
            }
            break;
        case SDL_EVENT_GAMEPAD_BUTTON_DOWN: {
            if (m_ctrl_capturing_tab < 0 || m_ctrl_capturing_index < 0 || m_ctrl_capturing_axis)
                break;
            auto &t = m_ctrl_tabs[m_ctrl_capturing_tab];
            const auto pressed = static_cast<SDL_GamepadButton>(ev.gbutton.button);
            auto &cb = t.bindings[m_ctrl_capturing_index];
            if (cb.vita_button != SDL_GAMEPAD_BUTTON_INVALID) {
                const int idx = static_cast<int>(cb.vita_button);
                auto &binds = m_emuenv.cfg.controller_binds;
                if (binds.size() <= static_cast<size_t>(idx))
                    binds.resize(idx + 1, 0);
                binds[idx] = static_cast<short>(pressed);
                save_config();
            }
            cancel_controller_capture();
            return;
        }
        case SDL_EVENT_GAMEPAD_AXIS_MOTION: {
            if (m_ctrl_capturing_tab < 0 || m_ctrl_capturing_index < 0 || !m_ctrl_capturing_axis)
                break;
            constexpr int16_t AXIS_DEADZONE = 16000;
            const auto axis_value = ev.gaxis.value;
            if (std::abs(axis_value) < AXIS_DEADZONE)
                break;
            const auto moved_axis = static_cast<SDL_GamepadAxis>(ev.gaxis.axis);
            auto &t = m_ctrl_tabs[m_ctrl_capturing_tab];
            auto &ab = t.axis_bindings[m_ctrl_capturing_index];
            const int ai = ab.axis_index;
            auto &axis_binds = m_emuenv.cfg.controller_axis_binds;
            if (axis_binds.size() <= static_cast<size_t>(ai))
                axis_binds.resize(ai + 1, 0);
            axis_binds[ai] = static_cast<short>(moved_axis);
            save_config();
            cancel_controller_capture();
            return;
        }
        default: break;
        }
    }
}

void ControlsDialog::reset_controller_defaults(int /*index*/) {
    m_emuenv.cfg.controller_binds = {
        SDL_GAMEPAD_BUTTON_SOUTH, SDL_GAMEPAD_BUTTON_EAST, SDL_GAMEPAD_BUTTON_WEST, SDL_GAMEPAD_BUTTON_NORTH,
        SDL_GAMEPAD_BUTTON_BACK, SDL_GAMEPAD_BUTTON_GUIDE, SDL_GAMEPAD_BUTTON_START,
        SDL_GAMEPAD_BUTTON_LEFT_STICK, SDL_GAMEPAD_BUTTON_RIGHT_STICK,
        SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER,
        SDL_GAMEPAD_BUTTON_DPAD_UP, SDL_GAMEPAD_BUTTON_DPAD_DOWN, SDL_GAMEPAD_BUTTON_DPAD_LEFT, SDL_GAMEPAD_BUTTON_DPAD_RIGHT
    };
    m_emuenv.cfg.controller_axis_binds = {
        SDL_GAMEPAD_AXIS_LEFTX, SDL_GAMEPAD_AXIS_LEFTY,
        SDL_GAMEPAD_AXIS_RIGHTX, SDL_GAMEPAD_AXIS_RIGHTY,
        SDL_GAMEPAD_AXIS_LEFT_TRIGGER, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER
    };
    save_config();
    for (int i = 0; i < MAX_CONTROLLERS; ++i) {
        if (m_ctrl_tabs[i].connected)
            refresh_controller_tab_labels(i);
    }
}
