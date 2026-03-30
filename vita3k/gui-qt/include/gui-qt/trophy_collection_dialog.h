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

#include <gui-qt/trophy_notifier.h>
#include <np/trophy/context.h>

#include <emuenv/state.h>

#include <QDialog>
#include <QSortFilterProxyModel>
#include <QStandardItemModel>

#include <array>
#include <memory>
#include <string>
#include <vector>

class QLabel;
class QPushButton;
class QStackedWidget;
class QTableView;
class QSlider;

struct TrophyEntry {
    int id = 0;
    QString name;
    QString detail;
    int grade = 0;
    bool hidden = false;
    bool earned = false;
    quint64 timestamp = 0;
};

struct TrophyGameData {
    TrophyGameData() { grade_unlocked.fill(0); }

    std::string np_com_id;
    QString title;
    QString icon_path;
    int total = 0;
    int unlocked = 0;
    std::array<int, 5> grade_unlocked;

    std::vector<TrophyEntry> trophies;

    np::trophy::Context context;
};

class TrophyFilterProxy : public QSortFilterProxyModel {
public:
    explicit TrophyFilterProxy(QObject *parent = nullptr)
        : QSortFilterProxyModel(parent) {}

    bool show_locked = true;
    bool show_unlocked = true;
    bool show_hidden = false;
    bool show_bronze = true;
    bool show_silver = true;
    bool show_gold = true;
    bool show_platinum = true;

    void invalidate() { invalidateFilter(); }

protected:
    bool filterAcceptsRow(int src_row, const QModelIndex &) const override;
};

class TrophyCollectionDialog : public QDialog {
    Q_OBJECT

public:
    explicit TrophyCollectionDialog(EmuEnvState &emuenv, QWidget *parent = nullptr);
    ~TrophyCollectionDialog() override;

    void jump_to_trophy(const QString &np_com_id, int trophy_id);

    void refresh_game(const QString &np_com_id);

public Q_SLOTS:
    void on_trophy_unlocked(const TrophyUnlockData &data);
    void reload();

private Q_SLOTS:
    void on_game_selection_changed(int game_idx);
    void apply_filter();
    void show_trophy_context_menu(const QPoint &pos);
    void show_game_context_menu(const QPoint &pos);

private:
    void load_all_games();
    bool load_game_data(const QString &np_com_id_dir);
    void populate_game_table();
    void populate_trophy_table(int game_index);

    void load_game_icon_async(int row, const QString &path);
    void load_trophy_icon_async(int row, const QString &path);

    void adjust_trophy_icon_column();
    void adjust_game_icon_column();
    void repaint_trophy_icons();
    void repaint_game_icons();
    void update_summary_progress();

    static QString format_timestamp(quint64 unix_sec);

    EmuEnvState &m_emuenv;

    std::vector<std::unique_ptr<TrophyGameData>> m_db;

    QLabel *m_progress_label = nullptr;
    QStackedWidget *m_stacked = nullptr;
    QPushButton *m_back_button = nullptr;
    QTableView *m_game_table = nullptr;
    QTableView *m_trophy_table = nullptr;
    QSlider *m_icon_slider = nullptr;

    QStandardItemModel *m_game_model = nullptr;
    QStandardItemModel *m_trophy_model = nullptr;
    QSortFilterProxyModel *m_trophy_proxy = nullptr;

    bool m_show_locked = true;
    bool m_show_unlocked = true;
    bool m_show_hidden = false;
    bool m_show_bronze = true;
    bool m_show_silver = true;
    bool m_show_gold = true;
    bool m_show_platinum = true;

    QSize m_game_icon_size{ 104, 104 };
    QSize m_trophy_icon_size{ 76, 76 };
    int m_game_slider_pos = 50;
    int m_trophy_slider_pos = 25;
    int m_selected_game_idx = -1;

    enum TrophyCol {
        TC_Icon = 0,
        TC_Name,
        TC_Detail,
        TC_Grade,
        TC_Status,
        TC_Id,
        TC_Time,
        TC_Count
    };

    enum GameCol {
        GC_Icon = 0,
        GC_Name,
        GC_Progress,
        GC_Trophies,
        GC_Count
    };
};