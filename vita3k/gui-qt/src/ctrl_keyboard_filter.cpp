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

#include <gui-qt/ctrl_keyboard_filter.h>

#include <config/state.h>
#include <ctrl/ctrl.h>
#include <ctrl/state.h>
#include <touch/functions.h>
#include <util/log.h>

#include <QEvent>
#include <QKeyEvent>
#include <QWheelEvent>

#include <mutex>

CtrlKeyboardFilter::CtrlKeyboardFilter(EmuEnvState &emuenv, QObject *parent)
    : QObject(parent)
    , m_emuenv(emuenv) {};

bool CtrlKeyboardFilter::eventFilter(QObject *watched, QEvent *event) {
    switch (event->type()) {
    case QEvent::FocusOut: {
        std::lock_guard<std::mutex> lock(m_emuenv.ctrl.mutex);

        m_emuenv.ctrl.keyboard_state = {};
        m_ls_left = m_ls_right = m_ls_up = m_ls_down = false;
        m_rs_left = m_rs_right = m_rs_up = m_rs_down = false;
        return false;
    }
    case QEvent::Wheel: {
        auto *we = static_cast<QWheelEvent *>(event);
        const float delta = we->angleDelta().y() / 120.0f;
        if (delta != 0.0f)
            pinch_move(delta);
        return false;
    }
    case QEvent::KeyPress:
    case QEvent::KeyRelease: {
        auto *ke = static_cast<QKeyEvent *>(event);
        if (ke->isAutoRepeat())
            return false;

        const bool pressed = (event->type() == QEvent::KeyPress);
        update_state(ke->key(), pressed);

        if (pressed && ke->key() == m_emuenv.cfg.keyboard_button_psbutton)
            emit ps_button_pressed();

        if (pressed && ke->key() == m_emuenv.cfg.keyboard_gui_fullscreen)
            emit fullscreen_toggled();

        if (pressed && ke->key() == m_emuenv.cfg.keyboard_gui_toggle_touch)
            toggle_touchscreen();

        if (pressed && ke->key() == m_emuenv.cfg.keyboard_toggle_texture_replacement)
            emit texture_replacement_toggled();

        if (pressed && ke->key() == m_emuenv.cfg.keyboard_take_screenshot)
            emit screenshot_requested();

        if (ke->key() == m_emuenv.cfg.keyboard_pinch_modifier
            || ke->key() == m_emuenv.cfg.keyboard_alternate_pinch_in
            || ke->key() == m_emuenv.cfg.keyboard_alternate_pinch_out)
            pinch_modifier(pressed);

        if (pressed && ke->key() == m_emuenv.cfg.keyboard_alternate_pinch_in)
            pinch_automove(-0.5f);
        if (pressed && ke->key() == m_emuenv.cfg.keyboard_alternate_pinch_out)
            pinch_automove(0.5f);
        if (!pressed && (ke->key() == m_emuenv.cfg.keyboard_alternate_pinch_in || ke->key() == m_emuenv.cfg.keyboard_alternate_pinch_out))
            pinch_automove(0);

        return false;
    }
    default:
        return QObject::eventFilter(watched, event);
    }
}

void CtrlKeyboardFilter::update_state(int qt_key, bool pressed) {
    const auto &cfg = m_emuenv.cfg;

    if (qt_key == cfg.keyboard_leftstick_left)
        m_ls_left = pressed;
    else if (qt_key == cfg.keyboard_leftstick_right)
        m_ls_right = pressed;
    else if (qt_key == cfg.keyboard_leftstick_up)
        m_ls_up = pressed;
    else if (qt_key == cfg.keyboard_leftstick_down)
        m_ls_down = pressed;
    else if (qt_key == cfg.keyboard_rightstick_left)
        m_rs_left = pressed;
    else if (qt_key == cfg.keyboard_rightstick_right)
        m_rs_right = pressed;
    else if (qt_key == cfg.keyboard_rightstick_up)
        m_rs_up = pressed;
    else if (qt_key == cfg.keyboard_rightstick_down)
        m_rs_down = pressed;

    float axes[4];
    axes[0] = static_cast<float>(-m_ls_left + m_ls_right);
    axes[1] = static_cast<float>(-m_ls_up + m_ls_down);
    axes[2] = static_cast<float>(-m_rs_left + m_rs_right);
    axes[3] = static_cast<float>(-m_rs_up + m_rs_down);

    uint32_t buttons = 0;
    uint32_t buttons_ext = 0;

    auto toggle_bit = [&](int bind_key, uint32_t bit, uint32_t &mask) {
        if (qt_key == bind_key) {
            if (pressed)
                mask |= bit;
            else
                mask &= ~bit;
        }
    };

    {
        std::lock_guard<std::mutex> lock(m_emuenv.ctrl.mutex);
        auto &kb = m_emuenv.ctrl.keyboard_state;

        buttons = kb.buttons;
        buttons_ext = kb.buttons_ext;
    }

    auto toggle_common = [&](uint32_t &mask) {
        toggle_bit(cfg.keyboard_button_select, SCE_CTRL_SELECT, mask);
        toggle_bit(cfg.keyboard_button_start, SCE_CTRL_START, mask);
        toggle_bit(cfg.keyboard_button_up, SCE_CTRL_UP, mask);
        toggle_bit(cfg.keyboard_button_right, SCE_CTRL_RIGHT, mask);
        toggle_bit(cfg.keyboard_button_down, SCE_CTRL_DOWN, mask);
        toggle_bit(cfg.keyboard_button_left, SCE_CTRL_LEFT, mask);
        toggle_bit(cfg.keyboard_button_triangle, SCE_CTRL_TRIANGLE, mask);
        toggle_bit(cfg.keyboard_button_circle, SCE_CTRL_CIRCLE, mask);
        toggle_bit(cfg.keyboard_button_cross, SCE_CTRL_CROSS, mask);
        toggle_bit(cfg.keyboard_button_square, SCE_CTRL_SQUARE, mask);
        toggle_bit(cfg.keyboard_button_psbutton, SCE_CTRL_PSBUTTON, mask);
    };

    toggle_common(buttons);
    toggle_common(buttons_ext);

    toggle_bit(cfg.keyboard_button_l1, SCE_CTRL_L, buttons);
    toggle_bit(cfg.keyboard_button_r1, SCE_CTRL_R, buttons);

    toggle_bit(cfg.keyboard_button_l1, SCE_CTRL_L1, buttons_ext);
    toggle_bit(cfg.keyboard_button_r1, SCE_CTRL_R1, buttons_ext);
    toggle_bit(cfg.keyboard_button_l2, SCE_CTRL_L2, buttons_ext);
    toggle_bit(cfg.keyboard_button_r2, SCE_CTRL_R2, buttons_ext);
    toggle_bit(cfg.keyboard_button_l3, SCE_CTRL_L3, buttons_ext);
    toggle_bit(cfg.keyboard_button_r3, SCE_CTRL_R3, buttons_ext);

    {
        std::lock_guard<std::mutex> lock(m_emuenv.ctrl.mutex);
        auto &kb = m_emuenv.ctrl.keyboard_state;
        kb.buttons = buttons;
        kb.buttons_ext = buttons_ext;
        kb.axes[0] = axes[0];
        kb.axes[1] = axes[1];
        kb.axes[2] = axes[2];
        kb.axes[3] = axes[3];
    }
}
