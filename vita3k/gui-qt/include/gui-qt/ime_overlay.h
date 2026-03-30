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

#include <gui-qt/game_overlay_base.h>
#include <ime/types.h>

#include <QVector>

class QPushButton;
class QGridLayout;
class QHBoxLayout;
class QScrollArea;
class QVBoxLayout;
class QTimer;

struct EmuEnvState;

class ImeOverlay : public GameOverlayBase {
    Q_OBJECT

public:
    explicit ImeOverlay(QWidget *game_widget, EmuEnvState &emuenv);
    ~ImeOverlay() override = default;

    void update_ime();
    void dismiss();

protected:
    void on_reposition() override;
    void paintEvent(QPaintEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void build_keyboard();
    void build_letter_keyboard();
    void build_numeric_keyboard();
    void refresh_key_labels();
    void clear_keyboard();

    void on_key_pressed(const std::u16string &key);
    void on_ponct_pressed(const std::u16string &key);

    EmuEnvState &m_emuenv;

    QWidget *m_keyboard = nullptr;
    QVBoxLayout *m_keyboard_layout = nullptr;

    QVector<QPushButton *> m_row1_keys;
    QVector<QPushButton *> m_row2_keys;
    QVector<QPushButton *> m_row3_keys;

    QPushButton *m_shift_btn = nullptr;
    QPushButton *m_backspace_btn = nullptr;
    QPushButton *m_close_btn = nullptr;
    QPushButton *m_numeric_btn = nullptr;
    QPushButton *m_space_btn = nullptr;
    QPushButton *m_enter_btn = nullptr;
    QPushButton *m_left_btn = nullptr;
    QPushButton *m_right_btn = nullptr;
    QPushButton *m_punct1_btn = nullptr;
    QPushButton *m_punct2_btn = nullptr;
    QPushButton *m_lang_btn = nullptr;

    QVector<QPushButton *> m_numeric_row_keys;
    QScrollArea *m_special_scroll = nullptr;
    QWidget *m_special_grid_widget = nullptr;
    QVector<QPushButton *> m_special_keys;
    QVector<QPushButton *> m_numpad_keys;
    int m_special_page = 0;

    bool m_numeric_mode = false;
    bool m_built = false;
    uint32_t m_last_caps_level = UINT32_MAX;
    qreal m_built_scale = 0;
    QTimer *m_rebuild_timer = nullptr;

    static constexpr int k_key_height = 46;
    static constexpr int k_spacing = 6;
    static constexpr int k_row2_indent = 40;
};
