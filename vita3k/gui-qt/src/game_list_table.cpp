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

#include <config/settings.h>
#include <gui-qt/game_icon_item.h>
#include <gui-qt/game_list_delegate.h>
#include <gui-qt/game_list_table.h>
#include <util/log.h>

#include <QDateTime>
#include <QHeaderView>
#include <QImage>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QScrollBar>
#include <QTableWidgetItem>
#include <QThreadPool>

namespace {

class SortableItem : public QTableWidgetItem {
public:
    SortableItem()
        : QTableWidgetItem() {}

    explicit SortableItem(const QString &text,
        std::optional<QVariant> sort_key = std::nullopt)
        : QTableWidgetItem(text) {
        if (sort_key)
            setData(Qt::UserRole, *sort_key);
    }

    bool operator<(const QTableWidgetItem &other) const override {
        const QVariant a = data(Qt::UserRole);
        const QVariant b = other.data(Qt::UserRole);
        if (a.isValid() && b.isValid())
            return QVariant::compare(a, b) < 0;
        return text().toLower() < other.text().toLower();
    }
};

QString format_play_time(int64_t seconds) {
    if (seconds <= 0)
        return QObject::tr("Never played");
    const int64_t h = seconds / 3600;
    const int64_t m = (seconds % 3600) / 60;
    if (h > 0)
        return QStringLiteral("%1h %2m").arg(h).arg(m, 2, 10, QLatin1Char('0'));
    return QStringLiteral("%1m").arg(m);
}

} // namespace

GameListTable::GameListTable(QWidget *parent)
    : QTableWidget(parent) {
    setShowGrid(false);
    setFocusPolicy(Qt::NoFocus);
    setItemDelegate(new GameListDelegate(this));
    setEditTriggers(QAbstractItemView::NoEditTriggers);
    setSelectionBehavior(QAbstractItemView::SelectRows);
    setSelectionMode(QAbstractItemView::ExtendedSelection);
    setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
    verticalScrollBar()->setSingleStep(20);
    horizontalScrollBar()->setSingleStep(20);
    setContextMenuPolicy(Qt::CustomContextMenu);
    setAlternatingRowColors(false);
    setMouseTracking(true);
    setColumnCount(GAME_LIST_COLUMN_COUNT);

    setup_header();
    verticalHeader()->setDefaultSectionSize(m_icon_size.height());

    connect(horizontalHeader(), &QHeaderView::sectionClicked,
        this, &GameListTable::on_column_header_clicked);

    connect(this, &QTableWidget::customContextMenuRequested, this, [this](const QPoint &pos) {
        auto games = selected_games();
        if (games.empty())
            return;
        Q_EMIT context_menu_requested(viewport()->mapToGlobal(pos), games);
    });
}

void GameListTable::setup_header() {
    QHeaderView *hdr = horizontalHeader();
    hdr->setHighlightSections(false);
    hdr->setSortIndicatorShown(true);
    hdr->setStretchLastSection(true);
    hdr->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    hdr->setDefaultSectionSize(150);

    verticalHeader()->setSectionResizeMode(QHeaderView::Fixed);
    verticalHeader()->setVisible(false);

    const QStringList labels = game_list_column_labels();
    for (int i = 0; i < labels.size(); ++i)
        horizontalHeaderItem(i) ? horizontalHeaderItem(i)->setText(labels[i])
                                : setHorizontalHeaderItem(i, new QTableWidgetItem(labels[i]));

    hdr->setSectionResizeMode(static_cast<int>(GameListColumn::Icon),
        QHeaderView::Fixed);
    hdr->setSortIndicator(m_sort_column, m_sort_order);
}

void GameListTable::set_icon_size(QSize size) {
    m_icon_size = size;
    adjust_icon_column();
    repaint_icons();
}

void GameListTable::repaint_icons() {
    const int icon_col = static_cast<int>(GameListColumn::Icon);
    const qreal dpr = devicePixelRatioF();
    for (int r = 0; r < rowCount(); ++r) {
        auto *icon_item = dynamic_cast<GameIconItem *>(item(r, icon_col));
        if (!icon_item)
            continue;

        QPixmap orig = icon_item->pixmap_copy();
        if (orig.isNull()) {
            const QVariant dec = icon_item->data(Qt::DecorationRole);
            if (dec.canConvert<QPixmap>())
                orig = dec.value<QPixmap>();
        }
        if (orig.isNull())
            continue;

        icon_item->setData(Qt::DecorationRole,
            orig.scaled(m_icon_size * dpr, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    }
}

void GameListTable::adjust_icon_column() {
    const int icon_col = static_cast<int>(GameListColumn::Icon);
    const int h = m_icon_size.height();
    const int w = m_icon_size.width();
    verticalHeader()->setDefaultSectionSize(h);
    verticalHeader()->setMinimumSectionSize(h);
    verticalHeader()->setMaximumSectionSize(h);
    horizontalHeader()->resizeSection(icon_col, w);
}

void GameListTable::resize_columns_to_contents(int spacing) {
    const int icon_col = static_cast<int>(GameListColumn::Icon);
    horizontalHeader()->resizeSections(QHeaderView::ResizeToContents);
    horizontalHeader()->resizeSection(icon_col, m_icon_size.width());
    for (int i = 1; i < columnCount(); ++i) {
        if (isColumnHidden(i))
            continue;
        horizontalHeader()->resizeSection(i, horizontalHeader()->sectionSize(i) + spacing);
    }
}

void GameListTable::restore_layout(const QByteArray &state) {
    resize_columns_to_contents();
    const int icon_w = columnWidth(static_cast<int>(GameListColumn::Icon));

    if (!horizontalHeader()->restoreState(state) && rowCount()) {
        // nothing
    }

    horizontalHeader()->resizeSection(static_cast<int>(GameListColumn::Icon), icon_w);
}

void GameListTable::sort(int column, Qt::SortOrder order) {
    const int old_rows = rowCount();

    std::vector<int> to_rehide;
    for (int i = 0; i < columnCount(); ++i) {
        if (isColumnHidden(i)) {
            setColumnHidden(i, false);
            to_rehide.push_back(i);
        }
    }

    sortByColumn(column, order);

    for (int col : to_rehide)
        setColumnHidden(col, true);

    if (!rowCount()) {
        // Preserve column widths
        return;
    }

    adjust_icon_column();

    if (!old_rows)
        resize_columns_to_contents();

    horizontalHeader()->setSectionResizeMode(static_cast<int>(GameListColumn::Icon),
        QHeaderView::Fixed);
}

void GameListTable::populate(const std::vector<app::Game> &games,
    const std::map<std::string, app::GameTime> &user_times,
    const CompatState &compat,
    const fs::path &pref_path,
    const fs::path &config_path) {
    clearContents();
    verticalHeader()->setDefaultSectionSize(m_icon_size.height());
    setRowCount(static_cast<int>(games.size()));

    const qreal dpr = devicePixelRatioF();

    int row = 0;
    for (const app::Game &game : games) {
        auto *icon_item = new GameIconItem();
        icon_item->set_game(&game);
        icon_item->set_has_custom_config(config::has_custom_config(config_path, game.path));

        const std::string icon_path = game.icon_path;
        const std::string resolved_icon = icon_path.empty()
            ? std::string()
            : (pref_path / icon_path).generic_string();

        icon_item->set_icon_load_func([this, icon_item, resolved_icon, dpr]() {
            QThreadPool::globalInstance()->start([this, icon_item, resolved_icon, dpr]() {
                QImage img;
                if (!resolved_icon.empty())
                    img.load(QString::fromStdString(resolved_icon));

                QMetaObject::invokeMethod(this, [this, icon_item, img, dpr]() {
                    const QModelIndex idx = indexFromItem(icon_item);
                    if (!idx.isValid())
                        return;

                    QPixmap px;
                    if (img.isNull()) {
                        px = QPixmap(m_icon_size * dpr);
                        px.setDevicePixelRatio(dpr);
                        px.fill(Qt::transparent);
                    } else {
                        px = QPixmap::fromImage(img);
                        px.setDevicePixelRatio(dpr);
                    }
                    icon_item->set_pixmap(px);
                    icon_item->setData(Qt::DecorationRole,
                        px.scaled(m_icon_size * dpr, Qt::KeepAspectRatio,
                                  Qt::SmoothTransformation));
                    viewport()->update(visualItemRect(icon_item)); }, Qt::QueuedConnection);
            });
        });

        setItem(row, static_cast<int>(GameListColumn::Icon), icon_item);
        setItem(row, static_cast<int>(GameListColumn::Title),
            new SortableItem(QString::fromStdString(game.title)));
        setItem(row, static_cast<int>(GameListColumn::TitleId),
            new SortableItem(QString::fromStdString(game.title_id)));
        setItem(row, static_cast<int>(GameListColumn::Version),
            new SortableItem(QString::fromStdString(game.app_ver)));
        setItem(row, static_cast<int>(GameListColumn::Category),
            new SortableItem(QString::fromStdString(game.category)));

        CompatibilityState compat_state = CompatibilityState::UNKNOWN;
        if (const auto it = compat.app_compat_db.find(game.title_id);
            it != compat.app_compat_db.end()) {
            compat_state = it->second.state;
        }

        auto *compat_item = new SortableItem(
            QString(),
            QVariant(static_cast<int>(compat_state)));

        const QPixmap pill = compat_pixmap(compat_state);
        if (!pill.isNull())
            compat_item->setData(Qt::DecorationRole, pill);

        compat_item->setTextAlignment(Qt::AlignCenter);
        setItem(row, static_cast<int>(GameListColumn::CompatStatus), compat_item);

        const auto time_it = user_times.find(game.path);
        const std::time_t last_played = (time_it != user_times.end()) ? time_it->second.last_time_used : 0;
        const int64_t time_played = (time_it != user_times.end()) ? time_it->second.time_used : 0;

        QString last_played_str;
        QVariant last_played_sort;
        if (last_played != 0) {
            const QDateTime lp = QDateTime::fromSecsSinceEpoch(static_cast<qint64>(last_played));
            const bool recent = lp >= QDateTime::currentDateTime().addDays(-7);
            last_played_str = lp.toString(recent ? QStringLiteral("ddd hh:mm")
                                                 : QStringLiteral("yyyy-MM-dd"));
            last_played_sort = QVariant(static_cast<qlonglong>(last_played));
        } else {
            last_played_str = tr("Never");
            last_played_sort = QVariant(static_cast<qlonglong>(0));
        }
        setItem(row, static_cast<int>(GameListColumn::LastPlayed),
            new SortableItem(last_played_str, last_played_sort));

        setItem(row, static_cast<int>(GameListColumn::PlayTime),
            new SortableItem(format_play_time(time_played),
                QVariant(static_cast<qlonglong>(time_played))));

        ++row;
    }

    adjust_icon_column();
    resize_columns_to_contents();
    horizontalHeader()->setSectionResizeMode(static_cast<int>(GameListColumn::Icon),
        QHeaderView::Fixed);
    sort(m_sort_column, m_sort_order);

    Q_EMIT list_refreshed();
}

QString GameListTable::compat_text(const CompatibilityState state) const {
    switch (state) {
    case CompatibilityState::PLAYABLE: return tr("Playable");
    case CompatibilityState::INGAME_MORE: return tr("Ingame+");
    case CompatibilityState::INGAME_LESS: return tr("Ingame");
    case CompatibilityState::MENU: return tr("Menu");
    case CompatibilityState::INTRO: return tr("Intro");
    case CompatibilityState::BOOTABLE: return tr("Boots");
    case CompatibilityState::NOTHING: return tr("Nothing");
    default: return tr("Unknown");
    }
}

QColor GameListTable::compat_color(const CompatibilityState state) const {
    switch (state) {
    case CompatibilityState::UNKNOWN: return QColor(0x8a, 0x8a, 0x8a); // #8a8a8a
    case CompatibilityState::NOTHING: return QColor(0xff, 0x00, 0x00); // #ff0000
    case CompatibilityState::BOOTABLE: return QColor(0x62, 0x1f, 0xa5); // #621fa5
    case CompatibilityState::INTRO: return QColor(0xc7, 0x15, 0x85); // #c71585
    case CompatibilityState::MENU: return QColor(0x1d, 0x76, 0xdb); // #1d76db
    case CompatibilityState::INGAME_LESS: return QColor(0xe0, 0x8a, 0x1e); // #e08a1e
    case CompatibilityState::INGAME_MORE: return QColor(0xff, 0xd7, 0x00); // #ffd700
    case CompatibilityState::PLAYABLE: return QColor(0x0e, 0x8a, 0x16); // #0e8a16
    default: return {};
    }
}

QPixmap GameListTable::compat_pixmap(const CompatibilityState state) const {
    const QString text = compat_text(state);
    const QColor bg = compat_color(state);
    if (!bg.isValid())
        return {};

    QFont font;
    font.setPointSize(9);
    font.setBold(true);
    const QFontMetrics fm(font);

    const int pad_h = 10;
    const int pad_v = 4;
    const int w = fm.horizontalAdvance(text) + pad_h * 2;
    const int h = fm.height() + pad_v * 2;

    const qreal dpr = devicePixelRatioF();
    QPixmap pm(QSize(w, h) * dpr);
    pm.setDevicePixelRatio(dpr);
    pm.fill(Qt::transparent);

    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::TextAntialiasing);
    p.setBrush(bg);
    p.setPen(Qt::NoPen);
    p.drawRoundedRect(QRect(0, 0, w, h), h / 2.0, h / 2.0);

    const bool dark = (bg.red() * 299 + bg.green() * 587 + bg.blue() * 114) / 1000 < 128;
    p.setPen(dark ? Qt::white : Qt::black);
    p.setFont(font);
    p.drawText(QRect(0, 0, w, h), Qt::AlignCenter, text);

    return pm;
}

app::Game GameListTable::game_from_row(int row) const {
    if (const auto *icon_item = dynamic_cast<const GameIconItem *>(
            item(row, static_cast<int>(GameListColumn::Icon)))) {
        if (const app::Game *g = icon_item->game())
            return *g;
    }
    return {};
}

const app::Game *GameListTable::selected_game() const {
    const int row = currentRow();
    if (row < 0)
        return nullptr;
    if (const auto *icon_item = dynamic_cast<const GameIconItem *>(
            item(row, static_cast<int>(GameListColumn::Icon)))) {
        return icon_item->game();
    }
    return nullptr;
}

std::vector<const app::Game *> GameListTable::selected_games() const {
    std::vector<const app::Game *> result;
    const int icon_col = static_cast<int>(GameListColumn::Icon);
    for (const auto &range : selectedRanges()) {
        for (int r = range.topRow(); r <= range.bottomRow(); ++r) {
            if (const auto *icon_item = dynamic_cast<const GameIconItem *>(item(r, icon_col))) {
                if (const auto *g = icon_item->game())
                    result.push_back(g);
            }
        }
    }
    return result;
}

void GameListTable::mousePressEvent(QMouseEvent *event) {
    if (!indexAt(event->pos()).isValid())
        clearSelection();
    QTableWidget::mousePressEvent(event);

    const int row = currentRow();
    if (row < 0) {
        Q_EMIT game_selection_changed(nullptr);
        return;
    }
    if (const auto *icon_item = dynamic_cast<const GameIconItem *>(
            item(row, static_cast<int>(GameListColumn::Icon)))) {
        Q_EMIT game_selection_changed(icon_item->game());
    }
}

void GameListTable::mouseDoubleClickEvent(QMouseEvent *event) {
    QTableWidget::mouseDoubleClickEvent(event);
    const int row = currentRow();
    if (row >= 0)
        Q_EMIT game_boot_requested(game_from_row(row));
}

void GameListTable::keyPressEvent(QKeyEvent *event) {
    if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
        const int row = currentRow();
        if (row >= 0)
            Q_EMIT game_boot_requested(game_from_row(row));
        return;
    }
    QTableWidget::keyPressEvent(event);
}

void GameListTable::on_column_header_clicked(int col) {
    if (col == m_sort_column)
        m_sort_order = (m_sort_order == Qt::AscendingOrder)
            ? Qt::DescendingOrder
            : Qt::AscendingOrder;
    else
        m_sort_order = Qt::AscendingOrder;

    m_sort_column = col;
    horizontalHeader()->setSortIndicator(m_sort_column, m_sort_order);
    sort(m_sort_column, m_sort_order);
}