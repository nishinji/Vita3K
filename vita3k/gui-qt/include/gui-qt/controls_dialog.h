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

#include <emuenv/state.h>

#include <QDialog>
#include <QTimer>

#include <array>
#include <memory>
#include <vector>

#include <SDL3/SDL_gamepad.h>

namespace Ui {
class ControlsDialog;
}

class QCheckBox;
class QGroupBox;
class QLabel;
class QPushButton;
class QSlider;
class QSpinBox;

class ControlsDialog final : public QDialog {
    Q_OBJECT

public:
    explicit ControlsDialog(EmuEnvState &emuenv, QWidget *parent = nullptr);
    ~ControlsDialog();

    static constexpr int MAX_CONTROLLERS = 4;

protected:
    void keyPressEvent(QKeyEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    struct KeyboardBinding {
        QPushButton *button = nullptr;
        int *config_value = nullptr;
        int default_value = 0;
    };

    void init_keyboard_bindings();
    void refresh_all_keyboard_labels();
    void start_keyboard_capture(int index);
    void cancel_keyboard_capture();
    void reset_keyboard_defaults();
    bool has_duplicate_key(int qt_key, int exclude_index) const;

    static QString key_name(int qt_key);

    struct ControllerButtonBinding {
        QPushButton *button = nullptr;
        SDL_GamepadButton vita_button = SDL_GAMEPAD_BUTTON_INVALID;
    };

    struct ControllerAxisBinding {
        QPushButton *button = nullptr;
        int axis_index = -1;
    };

    struct ControllerTab {
        QWidget *tab = nullptr;
        QLabel *lbl_controller_name = nullptr;
        QLabel *l_controller_image = nullptr;

        QPushButton *btn_dpad_up = nullptr;
        QPushButton *btn_dpad_down = nullptr;
        QPushButton *btn_dpad_left = nullptr;
        QPushButton *btn_dpad_right = nullptr;
        QPushButton *btn_lstick_up = nullptr;
        QPushButton *btn_lstick_down = nullptr;
        QPushButton *btn_lstick_left = nullptr;
        QPushButton *btn_lstick_right = nullptr;
        QPushButton *btn_rstick_up = nullptr;
        QPushButton *btn_rstick_down = nullptr;
        QPushButton *btn_rstick_left = nullptr;
        QPushButton *btn_rstick_right = nullptr;
        QPushButton *btn_cross = nullptr;
        QPushButton *btn_circle = nullptr;
        QPushButton *btn_square = nullptr;
        QPushButton *btn_triangle = nullptr;
        QPushButton *btn_l1 = nullptr;
        QPushButton *btn_r1 = nullptr;
        QPushButton *btn_l2 = nullptr;
        QPushButton *btn_r2 = nullptr;
        QPushButton *btn_l3 = nullptr;
        QPushButton *btn_r3 = nullptr;
        QPushButton *btn_select = nullptr;
        QPushButton *btn_start = nullptr;
        QPushButton *btn_psbutton = nullptr;

        QSlider *slider_analog_multiplier = nullptr;
        QLabel *lbl_analog_multiplier_value = nullptr;
        QCheckBox *chk_disable_motion = nullptr;
        QPushButton *btn_reset = nullptr;

        std::vector<ControllerButtonBinding> bindings;
        std::vector<ControllerAxisBinding> axis_bindings;

        bool connected = false;
        int port = -1;
        SDL_GUID guid{};
        SDL_GamepadType type = SDL_GAMEPAD_TYPE_STANDARD;
    };

    void build_controller_tab(int index);
    void refresh_controller_tabs();
    void refresh_controller_tab_labels(int index);
    void set_controller_tab_enabled(int index, bool enabled);
    void start_controller_capture(int tab_index, int binding_index);
    void start_axis_capture(int tab_index, int axis_binding_index);
    void cancel_controller_capture();
    void reset_controller_defaults(int index);

    void poll_sdl_events();

    static QString controller_button_name(SDL_GamepadType type, SDL_GamepadButton btn);
    static QString controller_axis_name(SDL_GamepadType type, SDL_GamepadAxis axis);

    void save_config();

    EmuEnvState &m_emuenv;
    std::unique_ptr<Ui::ControlsDialog> m_ui;

    std::vector<KeyboardBinding> m_kb_bindings;
    int m_kb_capturing_index = -1;

    std::array<ControllerTab, MAX_CONTROLLERS> m_ctrl_tabs;
    int m_ctrl_capturing_tab = -1;
    int m_ctrl_capturing_index = -1;
    bool m_ctrl_capturing_axis = false;

    QTimer *m_sdl_poll_timer = nullptr;
};
