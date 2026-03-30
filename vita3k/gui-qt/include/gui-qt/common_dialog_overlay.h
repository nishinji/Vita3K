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

#include <dialog/state.h>

#include <QMap>
#include <QPixmap>

class QLabel;
class QLineEdit;
class QTextEdit;
class QProgressBar;
class QPushButton;
class QVBoxLayout;
class QHBoxLayout;
class QScrollArea;
class QTimer;

struct EmuEnvState;

class CommonDialogOverlay : public GameOverlayBase {
    Q_OBJECT

public:
    explicit CommonDialogOverlay(QWidget *game_widget, EmuEnvState &emuenv);
    ~CommonDialogOverlay() override = default;

    void update_dialog();
    void dismiss();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void paintEvent(QPaintEvent *event) override;
    void on_reposition() override;

private:
    void rebuild_for_scale();

    void clear_content();

    void build_ime_dialog();
    void build_message_dialog();
    void build_trophy_setup_dialog();
    void build_savedata_dialog();
    void build_savedata_list_mode();
    void build_savedata_fixed_mode();

    void update_message_progress();
    void update_savedata_progress();
    void update_trophy_timer();

    void on_ime_submit();
    void on_ime_cancel();
    void on_msg_button_clicked(int index);
    void on_savedata_slot_clicked(uint32_t slot_index);
    void on_savedata_button_clicked(int index);
    void on_savedata_cancel();

    QPixmap load_icon_pixmap(uint32_t index);

    EmuEnvState &m_emuenv;

    QWidget *m_content = nullptr;
    QVBoxLayout *m_content_layout = nullptr;

    DialogType m_active_type = NO_DIALOG;
    bool m_content_fullscreen = false;
    qreal m_built_scale = 0;
    QTimer *m_rebuild_timer = nullptr;

    QLineEdit *m_ime_input = nullptr;
    QTextEdit *m_ime_multiline = nullptr;

    QLabel *m_message_label = nullptr;
    QProgressBar *m_progress_bar = nullptr;
    QLabel *m_progress_label = nullptr;
    QVector<QPushButton *> m_buttons;

    QLabel *m_trophy_label = nullptr;

    QScrollArea *m_save_scroll = nullptr;
    QMap<uint32_t, QPixmap> m_icon_cache;

    static constexpr int k_margin = 16;
};
