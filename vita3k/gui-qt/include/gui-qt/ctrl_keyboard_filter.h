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

#include <QObject>

class CtrlKeyboardFilter : public QObject {
    Q_OBJECT

public:
    explicit CtrlKeyboardFilter(EmuEnvState &emuenv, QObject *parent = nullptr);

signals:
    void ps_button_pressed();
    void fullscreen_toggled();
    void toggle_touch_pressed();
    void texture_replacement_toggled();
    void screenshot_requested();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void update_state(int qt_key, bool pressed);

    EmuEnvState &m_emuenv;

    bool m_ls_left = false;
    bool m_ls_right = false;
    bool m_ls_up = false;
    bool m_ls_down = false;
    bool m_rs_left = false;
    bool m_rs_right = false;
    bool m_rs_up = false;
    bool m_rs_down = false;
};
