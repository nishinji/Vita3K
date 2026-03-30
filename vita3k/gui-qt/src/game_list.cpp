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

#include <app/functions.h>
#include <emuenv/state.h>
#include <gui-qt/game_list.h>
#include <gui-qt/game_list_table.h>
#include <gui-qt/gui_settings.h>
#include <util/log.h>

#include <QHBoxLayout>
#include <QHeaderView>
#include <QVBoxLayout>
#include <QWidget>

GameList::GameList(EmuEnvState &emuenv, QWidget *parent)
    : custom_dock_widget(tr("Game Library"), parent)
    , m_emuenv(emuenv) {
    setObjectName(QStringLiteral("GameList"));
    setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable);

    auto *central = new QWidget(this);
    auto *vbox = new QVBoxLayout(central);
    vbox->setContentsMargins(0, 4, 0, 0);
    vbox->setSpacing(4);

    m_table = new GameListTable(central);
    vbox->addWidget(m_table);

    setWidget(central);

    connect(m_table, &GameListTable::game_boot_requested,
        this, &GameList::on_game_boot_requested);

    connect(m_table, &GameListTable::game_selection_changed,
        this, &GameList::game_selection_changed);

    connect(m_table, &GameListTable::context_menu_requested,
        this, &GameList::context_menu_requested);
}

GameList::~GameList() = default;

void GameList::refresh(bool from_disk) {
    if (from_disk)
        app::scan_games(m_emuenv);
    else if (!app::load_cached_games(m_emuenv))
        app::scan_games(m_emuenv);

    app::load_time_games(m_emuenv);

    m_games = app::get_games(m_emuenv);
    const auto user_times = app::get_user_game_times(m_emuenv);
    m_table->populate(m_games, user_times, m_emuenv.compat, fs::path(m_emuenv.pref_path.string()), m_emuenv.config_path);

    if (!m_search_text.isEmpty())
        set_search_text(m_search_text);
}

void GameList::resize_icons(int slider_pos) {
    const int sz = 48 + (slider_pos * (160 - 48)) / 100;
    m_table->set_icon_size(QSize(sz, sz));
}

const app::Game *GameList::selected_game() const {
    return m_table->selected_game();
}

void GameList::set_search_text(const QString &text) {
    m_search_text = text.toLower();
    const int rows = m_table->rowCount();
    for (int r = 0; r < rows; ++r) {
        bool visible = true;
        if (!m_search_text.isEmpty()) {
            const auto *title_item = m_table->item(r, static_cast<int>(GameListColumn::Title));
            const auto *id_item = m_table->item(r, static_cast<int>(GameListColumn::TitleId));
            const QString title = title_item ? title_item->text().toLower() : QString();
            const QString id = id_item ? id_item->text().toLower() : QString();
            visible = title.contains(m_search_text) || id.contains(m_search_text);
        }
        m_table->setRowHidden(r, !visible);
    }
}

void GameList::on_game_boot_requested(const app::Game &game) {
    Q_EMIT boot_requested(game);
}

void GameList::closeEvent(QCloseEvent *event) {
    Q_EMIT frame_closed();
    QDockWidget::closeEvent(event);
}

void GameList::save_settings(GuiSettings &settings) const {
    if (!m_table)
        return;

    auto *hdr = m_table->horizontalHeader();
    settings.set_value(gui::gl_headerState, hdr->saveState(), false);
    settings.set_value(gui::gl_sortCol, hdr->sortIndicatorSection(), false);
    settings.set_value(gui::gl_sortAsc, hdr->sortIndicatorOrder() == Qt::AscendingOrder, true);
}

void GameList::load_settings(const GuiSettings &settings) {
    if (!m_table)
        return;

    const QByteArray header_state = settings.get_value(gui::gl_headerState).toByteArray();
    if (!header_state.isEmpty())
        m_table->restore_layout(header_state);

    const int sort_col = settings.get_value(gui::gl_sortCol).toInt();
    const bool sort_asc = settings.get_value(gui::gl_sortAsc).toBool();
    m_table->sort(sort_col, sort_asc ? Qt::AscendingOrder : Qt::DescendingOrder);
}