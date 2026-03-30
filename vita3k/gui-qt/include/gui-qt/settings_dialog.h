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

#include <config/state.h>

#include <QDialog>
#include <QDialogButtonBox>

#include <memory>
#include <string>
#include <vector>

struct EmuEnvState;
class GuiSettings;

namespace Ui {
class vita3k_settings_dialog;
}

class SettingsDialog final : public QDialog {
    Q_OBJECT

public:
    explicit SettingsDialog(EmuEnvState &emuenv,
        std::shared_ptr<GuiSettings> gui_settings,
        const std::string &app_path = {},
        int initial_tab = 0,
        QWidget *parent = nullptr);
    ~SettingsDialog();

    void reject() override;

private slots:
    void on_save_clicked();
    void on_apply_clicked();
    void on_close_clicked();

private:
    void load_config();
    void save_config();
    void apply_config();
    void populate_modules_list();
    void populate_cameras_list();
    void populate_adhoc_list();
    void setup_connections();
    void setup_dirty_tracking();
    void update_resolution_label();
    void update_aniso_label();
    void update_gpu_visibility();
    void update_camera_widgets();
    void update_camera_color_preview();
    void update_modules_list_enabled();
    void update_file_loading_delay_label();
    void mark_dirty();
    void set_description(QWidget *tab, const QString &text);

    bool eventFilter(QObject *watched, QEvent *event) override;

    std::vector<QDialogButtonBox *> m_button_boxes;

    EmuEnvState &emuenv;
    std::shared_ptr<GuiSettings> m_gui_settings;
    std::string m_app_path;
    int m_initial_tab;
    bool m_dirty = false;
    Config::CurrentConfig m_config;
    std::unique_ptr<Ui::vita3k_settings_dialog> m_ui;
};