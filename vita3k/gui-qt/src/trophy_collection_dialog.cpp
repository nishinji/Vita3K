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

#include <gui-qt/trophy_collection_dialog.h>

#include <config/state.h>
#include <io/functions.h>
#include <np/trophy/context.h>
#include <util/fs.h>
#include <util/log.h>

#include <pugixml.hpp>

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QDateTime>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QProgressDialog>
#include <QPushButton>
#include <QStackedWidget>
#include <QTableView>
#include <QtConcurrent>

static constexpr int RoleRawValue = Qt::UserRole + 1;
static constexpr int RoleGrade = Qt::UserRole + 2;
static constexpr int RoleHidden = Qt::UserRole + 3;
static constexpr int RoleEarned = Qt::UserRole + 4;
static constexpr int RoleTrophyId = Qt::UserRole + 5;
static constexpr int RoleTimestamp = Qt::UserRole + 6;
static constexpr int RoleOriginalPixmap = Qt::UserRole + 7;

bool TrophyFilterProxy::filterAcceptsRow(int src_row, const QModelIndex &) const {
    const QStandardItemModel *m = qobject_cast<const QStandardItemModel *>(sourceModel());
    if (!m)
        return true;

    const bool earned = m->item(src_row, 0)->data(RoleEarned).toBool();
    const bool hidden = m->item(src_row, 0)->data(RoleHidden).toBool();
    const int grade = m->item(src_row, 0)->data(RoleGrade).toInt();

    if (earned && !show_unlocked)
        return false;
    if (!earned && !show_locked)
        return false;
    if (hidden && !earned && !show_hidden)
        return false;

    using G = np::trophy::SceNpTrophyGrade;
    switch (static_cast<G>(grade)) {
    case G::SCE_NP_TROPHY_GRADE_BRONZE: return show_bronze;
    case G::SCE_NP_TROPHY_GRADE_SILVER: return show_silver;
    case G::SCE_NP_TROPHY_GRADE_GOLD: return show_gold;
    case G::SCE_NP_TROPHY_GRADE_PLATINUM: return show_platinum;
    default: return true;
    }
}

TrophyCollectionDialog::TrophyCollectionDialog(EmuEnvState &emuenv, QWidget *parent)
    : QDialog(parent, Qt::Window)
    , m_emuenv(emuenv) {
    setWindowTitle(tr("Trophy Collection"));
    setObjectName(QStringLiteral("TrophyCollectionDialog"));
    setAttribute(Qt::WA_DeleteOnClose);
    resize(1024, 640);

    m_game_model = new QStandardItemModel(0, GC_Count, this);
    m_game_model->setHorizontalHeaderLabels({ tr("Icon"), tr("Game"), tr("Progress"), tr("Trophies") });

    m_game_table = new QTableView(this);
    m_game_table->setModel(m_game_model);
    m_game_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_game_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_game_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_game_table->setFocusPolicy(Qt::NoFocus);
    m_game_table->setShowGrid(false);
    m_game_table->setAlternatingRowColors(false);
    m_game_table->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_game_table->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_game_table->verticalHeader()->setVisible(false);
    m_game_table->verticalHeader()->setSectionResizeMode(QHeaderView::Fixed);
    m_game_table->verticalHeader()->setDefaultSectionSize(m_game_icon_size.height());
    m_game_table->horizontalHeader()->setStretchLastSection(true);
    m_game_table->horizontalHeader()->setSectionResizeMode(GC_Icon, QHeaderView::Fixed);
    m_game_table->horizontalHeader()->resizeSection(GC_Icon, m_game_icon_size.width());
    m_game_table->setContextMenuPolicy(Qt::CustomContextMenu);

    m_trophy_model = new QStandardItemModel(0, TC_Count, this);
    m_trophy_model->setHorizontalHeaderLabels({ tr("Icon"), tr("Name"), tr("Description"),
        tr("Grade"), tr("Status"), tr("ID"), tr("Earned") });

    auto *proxy = new TrophyFilterProxy(this);
    proxy->setSourceModel(m_trophy_model);
    m_trophy_proxy = proxy;

    m_trophy_table = new QTableView(this);
    m_trophy_table->setModel(m_trophy_proxy);
    m_trophy_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_trophy_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_trophy_table->setFocusPolicy(Qt::NoFocus);
    m_trophy_table->setShowGrid(false);
    m_trophy_table->setAlternatingRowColors(false);
    m_trophy_table->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_trophy_table->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_trophy_table->verticalHeader()->setVisible(false);
    m_trophy_table->verticalHeader()->setSectionResizeMode(QHeaderView::Fixed);
    m_trophy_table->horizontalHeader()->setStretchLastSection(true);
    m_trophy_table->horizontalHeader()->setSectionResizeMode(TC_Icon, QHeaderView::Fixed);
    m_trophy_table->setContextMenuPolicy(Qt::CustomContextMenu);

    m_progress_label = new QLabel(tr("Progress: 0% (0/0)"), this);

    m_icon_slider = new QSlider(Qt::Horizontal, this);
    m_icon_slider->setRange(0, 100);
    m_icon_slider->setValue(m_game_slider_pos);
    m_icon_slider->setFixedWidth(100);

    const auto make_check = [](const QString &text, bool checked) {
        auto *cb = new QCheckBox(text);
        cb->setChecked(checked);
        return cb;
    };

    auto *cb_locked = make_check(tr("Not Earned"), m_show_locked);
    auto *cb_unlocked = make_check(tr("Earned"), m_show_unlocked);
    auto *cb_hidden = make_check(tr("Hidden"), m_show_hidden);
    auto *cb_bronze = make_check(tr("Bronze"), m_show_bronze);
    auto *cb_silver = make_check(tr("Silver"), m_show_silver);
    auto *cb_gold = make_check(tr("Gold"), m_show_gold);
    auto *cb_platinum = make_check(tr("Platinum"), m_show_platinum);

    auto *top_bar = new QHBoxLayout;
    top_bar->addWidget(m_progress_label);
    top_bar->addSpacing(12);
    top_bar->addWidget(cb_locked);
    top_bar->addWidget(cb_unlocked);
    top_bar->addWidget(cb_hidden);
    top_bar->addSpacing(8);
    top_bar->addWidget(cb_bronze);
    top_bar->addWidget(cb_silver);
    top_bar->addWidget(cb_gold);
    top_bar->addWidget(cb_platinum);
    top_bar->addStretch();
    top_bar->addWidget(new QLabel(tr("Icon size:"), this));
    top_bar->addWidget(m_icon_slider);

    m_stacked = new QStackedWidget(this);
    m_stacked->addWidget(m_game_table);

    m_back_button = new QPushButton(tr("\u2190 Back to Games"), this);
    auto *trophy_page = new QWidget(this);
    auto *trophy_lay = new QVBoxLayout(trophy_page);
    trophy_lay->setContentsMargins(0, 0, 0, 0);
    trophy_lay->addWidget(m_back_button, 0, Qt::AlignLeft);
    trophy_lay->addWidget(m_trophy_table, 1);
    m_stacked->addWidget(trophy_page);

    auto *root = new QVBoxLayout(this);
    root->addLayout(top_bar);
    root->addWidget(m_stacked, 1);

    connect(m_game_table, &QTableView::doubleClicked,
        this, [this](const QModelIndex &idx) {
            if (!idx.isValid())
                return;
            m_selected_game_idx = m_game_model->item(idx.row(), GC_Name)
                                      ->data(RoleRawValue)
                                      .toInt();
            on_game_selection_changed(m_selected_game_idx);
            m_stacked->setCurrentIndex(1);
        });

    connect(m_back_button, &QPushButton::clicked, this, [this] {
        m_stacked->setCurrentIndex(0);
    });

    connect(m_stacked, &QStackedWidget::currentChanged, this, [this](int idx) {
        const QSignalBlocker blocker(m_icon_slider);
        m_icon_slider->setValue(idx == 0 ? m_game_slider_pos : m_trophy_slider_pos);
        if (idx == 0)
            update_summary_progress();
    });

    connect(m_icon_slider, &QSlider::valueChanged, this, [this](int val) {
        const int sz = 48 + (val * (160 - 48)) / 100;
        if (m_stacked->currentIndex() == 0) {
            m_game_icon_size = QSize(sz, sz);
            m_game_slider_pos = val;
            adjust_game_icon_column();
            repaint_game_icons();
        } else {
            m_trophy_icon_size = QSize(sz, sz);
            m_trophy_slider_pos = val;
            adjust_trophy_icon_column();
            repaint_trophy_icons();
        }
    });

    const auto wire_check = [&](QCheckBox *cb, bool &flag) {
        connect(cb, &QCheckBox::toggled, this, [this, &flag](bool v) {
            flag = v;
            apply_filter();
        });
    };
    wire_check(cb_locked, m_show_locked);
    wire_check(cb_unlocked, m_show_unlocked);
    wire_check(cb_hidden, m_show_hidden);
    wire_check(cb_bronze, m_show_bronze);
    wire_check(cb_silver, m_show_silver);
    wire_check(cb_gold, m_show_gold);
    wire_check(cb_platinum, m_show_platinum);

    connect(m_trophy_table, &QTableView::customContextMenuRequested,
        this, &TrophyCollectionDialog::show_trophy_context_menu);
    connect(m_game_table, &QTableView::customContextMenuRequested,
        this, &TrophyCollectionDialog::show_game_context_menu);

    load_all_games();
}

TrophyCollectionDialog::~TrophyCollectionDialog() = default;

void TrophyCollectionDialog::load_all_games() {
    m_db.clear();

    const fs::path trophy_conf = m_emuenv.pref_path
        / "ux0/user" / m_emuenv.io.user_id / "trophy/conf";

    if (!fs::exists(trophy_conf) || fs::is_empty(trophy_conf)) {
        populate_game_table();
        return;
    }

    QStringList dirs;
    for (const auto &entry : fs::directory_iterator(trophy_conf)) {
        if (fs::is_directory(entry))
            dirs << QString::fromStdString(entry.path().filename().string());
    }

    if (dirs.isEmpty()) {
        populate_game_table();
        return;
    }

    QProgressDialog progress(tr("Loading trophies…"), tr("Cancel"), 0, dirs.size(), this);
    progress.setWindowModality(Qt::WindowModal);
    progress.setMinimumDuration(0);

    for (int i = 0; i < dirs.size(); ++i) {
        if (progress.wasCanceled())
            break;
        load_game_data(dirs[i]);
        progress.setValue(i + 1);
    }

    populate_game_table();
}

bool TrophyCollectionDialog::load_game_data(const QString &np_com_id_str) {
    const std::string np_com_id = np_com_id_str.toStdString();
    const fs::path base = m_emuenv.pref_path / "ux0/user"
        / m_emuenv.io.user_id / "trophy";
    const fs::path conf_path = base / "conf" / np_com_id;
    const fs::path dat_path = base / "data" / np_com_id / "TROPUSR.DAT";

    if (!fs::exists(dat_path)) {
        LOG_ERROR("TROPUSR.DAT missing for {}", np_com_id);
        return false;
    }

    auto data = std::make_unique<TrophyGameData>();
    data->np_com_id = np_com_id;
    data->context.io = &m_emuenv.io;
    data->context.pref_path = m_emuenv.pref_path;
    data->context.trophy_progress_output_file_path = "ux0:user/" + m_emuenv.io.user_id
        + "/trophy/data/" + np_com_id + "/TROPUSR.DAT";

    {
        const SceUID fh = open_file(m_emuenv.io,
            data->context.trophy_progress_output_file_path.c_str(),
            SCE_O_RDONLY, m_emuenv.pref_path, "trophy_collection");
        if (fh < 0) {
            LOG_ERROR("Failed to open TROPUSR.DAT for {}", np_com_id);
            return false;
        }
        const bool loaded = data->context.load_trophy_progress_file(fh);
        close_file(m_emuenv.io, fh, "trophy_collection");
        if (!loaded) {
            LOG_ERROR("Failed to parse TROPUSR.DAT for {}", np_com_id);
            return false;
        }
    }

    const auto sfm_locale = fmt::format("TROP_{:0>2d}.SFM", m_emuenv.cfg.sys_lang);
    const fs::path sfm = fs::exists(conf_path / sfm_locale)
        ? conf_path / sfm_locale
        : conf_path / "TROP.SFM";

    pugi::xml_document doc;
    if (!doc.load_file(sfm.string().c_str())) {
        LOG_ERROR("Failed to parse {}", sfm.string());
        return false;
    }

    const auto root = doc.child("trophyconf");
    data->title = QString::fromStdString(root.child("title-name").text().as_string());

    const auto icon_locale = fmt::format("ICON0_{:0>2d}.PNG", m_emuenv.cfg.sys_lang);
    data->icon_path = QString::fromStdString(
        fs::exists(conf_path / icon_locale)
            ? (conf_path / icon_locale).string()
            : (conf_path / "ICON0.PNG").string());

    for (const auto &node : root.children("trophy")) {
        TrophyEntry e;
        e.id = node.attribute("id").as_int();
        e.hidden = node.attribute("hidden").as_string()[0] == 'y';
        e.name = QString::fromStdString(node.child("name").text().as_string());
        e.detail = QString::fromStdString(node.child("detail").text().as_string());

        switch (node.attribute("ttype").as_string()[0]) {
        case 'B': e.grade = static_cast<int>(np::trophy::SceNpTrophyGrade::SCE_NP_TROPHY_GRADE_BRONZE); break;
        case 'S': e.grade = static_cast<int>(np::trophy::SceNpTrophyGrade::SCE_NP_TROPHY_GRADE_SILVER); break;
        case 'G': e.grade = static_cast<int>(np::trophy::SceNpTrophyGrade::SCE_NP_TROPHY_GRADE_GOLD); break;
        case 'P': e.grade = static_cast<int>(np::trophy::SceNpTrophyGrade::SCE_NP_TROPHY_GRADE_PLATINUM); break;
        default: break;
        }

        e.earned = data->context.is_trophy_unlocked(e.id);
        e.timestamp = e.earned ? data->context.unlock_timestamps[e.id] : 0;

        if (e.earned) {
            data->unlocked++;
            data->grade_unlocked[e.grade]++;
        }

        data->trophies.push_back(std::move(e));
    }

    data->total = static_cast<int>(data->trophies.size());

    m_db.push_back(std::move(data));
    return true;
}

void TrophyCollectionDialog::populate_game_table() {
    m_game_model->removeRows(0, m_game_model->rowCount());

    std::sort(m_db.begin(), m_db.end(), [](const auto &a, const auto &b) {
        return a->title.toLower() < b->title.toLower();
    });

    for (int i = 0; i < static_cast<int>(m_db.size()); ++i) {
        const auto &g = *m_db[i];
        const int pct = g.total > 0 ? (g.unlocked * 100 / g.total) : 0;

        auto *icon_item = new QStandardItem;
        auto *name_item = new QStandardItem(g.title);
        auto *progress_item = new QStandardItem(tr("%1% (%2/%3)").arg(pct).arg(g.unlocked).arg(g.total));
        auto *trophy_item = new QStandardItem(QString::number(g.total));

        name_item->setData(i, RoleRawValue);
        progress_item->setData(pct, RoleRawValue);
        trophy_item->setData(g.total, RoleRawValue);

        m_game_model->appendRow({ icon_item, name_item, progress_item, trophy_item });

        load_game_icon_async(i, g.icon_path);
    }

    adjust_game_icon_column();
    update_summary_progress();
}

void TrophyCollectionDialog::populate_trophy_table(int game_idx) {
    m_trophy_model->removeRows(0, m_trophy_model->rowCount());

    if (game_idx < 0 || game_idx >= static_cast<int>(m_db.size()))
        return;

    const auto &g = *m_db[game_idx];
    const int pct = g.total > 0 ? (g.unlocked * 100 / g.total) : 0;
    m_progress_label->setText(tr("Progress: %1% (%2/%3)").arg(pct).arg(g.unlocked).arg(g.total));

    using G = np::trophy::SceNpTrophyGrade;
    const auto grade_str = [](int grade) -> QString {
        switch (static_cast<G>(grade)) {
        case G::SCE_NP_TROPHY_GRADE_PLATINUM: return QObject::tr("Platinum");
        case G::SCE_NP_TROPHY_GRADE_GOLD: return QObject::tr("Gold");
        case G::SCE_NP_TROPHY_GRADE_SILVER: return QObject::tr("Silver");
        case G::SCE_NP_TROPHY_GRADE_BRONZE: return QObject::tr("Bronze");
        default: return {};
        }
    };

    const fs::path conf_path = m_emuenv.pref_path / "ux0/user"
        / m_emuenv.io.user_id / "trophy/conf" / g.np_com_id;

    for (int row = 0; row < static_cast<int>(g.trophies.size()); ++row) {
        const auto &t = g.trophies[row];

        auto *icon_item = new QStandardItem;
        auto *name_item = new QStandardItem(t.name);
        auto *detail_item = new QStandardItem(t.detail);
        auto *grade_item = new QStandardItem(grade_str(t.grade));
        auto *status_item = new QStandardItem(t.earned ? tr("Earned") : tr("Not Earned"));
        auto *id_item = new QStandardItem(QString::number(t.id));
        auto *time_item = new QStandardItem(t.earned ? format_timestamp(t.timestamp) : tr("—"));

        icon_item->setData(t.hidden, RoleHidden);
        icon_item->setData(t.earned, RoleEarned);
        icon_item->setData(t.grade, RoleGrade);
        icon_item->setData(t.id, RoleTrophyId);
        id_item->setData(t.id, RoleRawValue);
        time_item->setData(QVariant::fromValue<quint64>(t.timestamp), RoleTimestamp);
        grade_item->setData(t.grade, RoleRawValue);

        m_trophy_model->appendRow({ icon_item, name_item, detail_item,
            grade_item, status_item, id_item, time_item });

        const QString icon_path = QString::fromStdString(
            (conf_path / fmt::format("TROP{:0>3d}.PNG", t.id)).string());
        load_trophy_icon_async(row, icon_path);
    }

    adjust_trophy_icon_column();
    apply_filter();
}

void TrophyCollectionDialog::load_game_icon_async(int row, const QString &path) {
    auto *watcher = new QFutureWatcher<QPixmap>(this);
    connect(watcher, &QFutureWatcher<QPixmap>::finished, this, [this, row, watcher] {
        if (auto *item = m_game_model->item(row, GC_Icon)) {
            const QPixmap orig = watcher->result();
            item->setData(orig, RoleOriginalPixmap);
            item->setData(orig.scaled(m_game_icon_size, Qt::KeepAspectRatio, Qt::SmoothTransformation),
                Qt::DecorationRole);
        }
        watcher->deleteLater();
    });
    watcher->setFuture(QtConcurrent::run([path] {
        QPixmap px;
        px.load(path);
        return px;
    }));
}

void TrophyCollectionDialog::load_trophy_icon_async(int row, const QString &path) {
    auto *watcher = new QFutureWatcher<QPixmap>(this);
    connect(watcher, &QFutureWatcher<QPixmap>::finished, this, [this, row, watcher] {
        if (auto *item = m_trophy_model->item(row, TC_Icon)) {
            const QPixmap orig = watcher->result();
            item->setData(orig, RoleOriginalPixmap);
            item->setData(orig.scaled(m_trophy_icon_size, Qt::KeepAspectRatio, Qt::SmoothTransformation),
                Qt::DecorationRole);
        }
        watcher->deleteLater();
    });
    watcher->setFuture(QtConcurrent::run([path] {
        QPixmap px;
        px.load(path);
        return px;
    }));
}

void TrophyCollectionDialog::apply_filter() {
    auto *proxy = static_cast<TrophyFilterProxy *>(m_trophy_proxy);
    proxy->show_locked = m_show_locked;
    proxy->show_unlocked = m_show_unlocked;
    proxy->show_hidden = m_show_hidden;
    proxy->show_bronze = m_show_bronze;
    proxy->show_silver = m_show_silver;
    proxy->show_gold = m_show_gold;
    proxy->show_platinum = m_show_platinum;
    proxy->invalidate();
}

void TrophyCollectionDialog::adjust_trophy_icon_column() {
    const int h = m_trophy_icon_size.height();
    const int w = m_trophy_icon_size.width();
    m_trophy_table->verticalHeader()->setDefaultSectionSize(h);
    m_trophy_table->verticalHeader()->setMinimumSectionSize(h);
    m_trophy_table->verticalHeader()->setMaximumSectionSize(h);
    m_trophy_table->horizontalHeader()->resizeSection(TC_Icon, w);
}

void TrophyCollectionDialog::adjust_game_icon_column() {
    const int h = m_game_icon_size.height();
    const int w = m_game_icon_size.width();
    m_game_table->verticalHeader()->setDefaultSectionSize(h);
    m_game_table->verticalHeader()->setMinimumSectionSize(h);
    m_game_table->verticalHeader()->setMaximumSectionSize(h);
    m_game_table->horizontalHeader()->resizeSection(GC_Icon, w);
}

void TrophyCollectionDialog::repaint_trophy_icons() {
    for (int r = 0; r < m_trophy_model->rowCount(); ++r) {
        auto *item = m_trophy_model->item(r, TC_Icon);
        if (!item)
            continue;
        const QVariant v = item->data(RoleOriginalPixmap);
        if (!v.canConvert<QPixmap>())
            continue;
        const QPixmap orig = v.value<QPixmap>();
        if (orig.isNull())
            continue;
        item->setData(orig.scaled(m_trophy_icon_size, Qt::KeepAspectRatio, Qt::SmoothTransformation),
            Qt::DecorationRole);
    }
}

void TrophyCollectionDialog::repaint_game_icons() {
    for (int r = 0; r < m_game_model->rowCount(); ++r) {
        auto *item = m_game_model->item(r, GC_Icon);
        if (!item)
            continue;
        const QVariant v = item->data(RoleOriginalPixmap);
        if (!v.canConvert<QPixmap>())
            continue;
        const QPixmap orig = v.value<QPixmap>();
        if (orig.isNull())
            continue;
        item->setData(orig.scaled(m_game_icon_size, Qt::KeepAspectRatio, Qt::SmoothTransformation),
            Qt::DecorationRole);
    }
}

void TrophyCollectionDialog::update_summary_progress() {
    int total_unlocked = 0;
    int total_trophies = 0;
    for (const auto &g : m_db) {
        total_unlocked += g->unlocked;
        total_trophies += g->total;
    }
    const int pct = total_trophies > 0 ? (total_unlocked * 100 / total_trophies) : 0;
    m_progress_label->setText(tr("Total: %1% (%2/%3 trophies across %4 games)")
            .arg(pct)
            .arg(total_unlocked)
            .arg(total_trophies)
            .arg(m_db.size()));
}

void TrophyCollectionDialog::on_game_selection_changed(int game_idx) {
    populate_trophy_table(game_idx);
}

void TrophyCollectionDialog::show_trophy_context_menu(const QPoint &pos) {
    const QModelIndex proxy_idx = m_trophy_table->indexAt(pos);
    if (!proxy_idx.isValid())
        return;
    const QModelIndex src_idx = m_trophy_proxy->mapToSource(proxy_idx);

    const int trophy_id = m_trophy_model->item(src_idx.row(), TC_Icon)->data(RoleTrophyId).toInt();
    const bool earned = m_trophy_model->item(src_idx.row(), TC_Icon)->data(RoleEarned).toBool();
    const int grade = m_trophy_model->item(src_idx.row(), TC_Icon)->data(RoleGrade).toInt();

    const int db_idx = m_selected_game_idx;
    if (db_idx < 0 || db_idx >= static_cast<int>(m_db.size()))
        return;

    QMenu menu(this);

    const QString name = m_trophy_model->item(src_idx.row(), TC_Name)->text();
    const QString detail = m_trophy_model->item(src_idx.row(), TC_Detail)->text();

    if (!name.isEmpty() || !detail.isEmpty()) {
        QMenu *copy_menu = menu.addMenu(tr("&Copy Info"));
        if (!name.isEmpty() && !detail.isEmpty())
            connect(copy_menu->addAction(tr("Name + Description")), &QAction::triggered, this,
                [name, detail] { QApplication::clipboard()->setText(name + "\n\n" + detail); });
        if (!name.isEmpty())
            connect(copy_menu->addAction(tr("Name")), &QAction::triggered, this,
                [name] { QApplication::clipboard()->setText(name); });
        if (!detail.isEmpty())
            connect(copy_menu->addAction(tr("Description")), &QAction::triggered, this,
                [detail] { QApplication::clipboard()->setText(detail); });
    }

    const bool game_running = !m_emuenv.io.app_path.empty();
    if (!game_running) {
        using G = np::trophy::SceNpTrophyGrade;
        const bool is_platinum = static_cast<G>(grade) == G::SCE_NP_TROPHY_GRADE_PLATINUM;
        menu.addSeparator();
        auto *lock_act = menu.addAction(earned ? tr("&Lock Trophy") : tr("&Unlock Trophy"));
        connect(lock_act, &QAction::triggered, this, [=, this] {
            if (is_platinum && !earned) {
                QMessageBox::information(this, tr("Not permitted"),
                    tr("Platinum trophies can only be unlocked in-game."));
                return;
            }
            auto &game = *m_db[db_idx];
            auto &ctx = game.context;
            auto *trophy_entry = [&]() -> TrophyEntry * {
                for (auto &t : game.trophies)
                    if (t.id == trophy_id)
                        return &t;
                return nullptr;
            }();
            if (!trophy_entry)
                return;
            np::NpTrophyError err;
            if (earned) {
                ctx.trophy_progress[trophy_id >> 5] &= ~(1u << (trophy_id & 31));
                ctx.unlock_timestamps[trophy_id] = 0;
                ctx.save_trophy_progress_file();
                trophy_entry->earned = false;
                trophy_entry->timestamp = 0;
                game.unlocked--;
                game.grade_unlocked[grade]--;
            } else {
                ctx.unlock_trophy(trophy_id, &err, false);
                trophy_entry->earned = true;
                trophy_entry->timestamp = ctx.unlock_timestamps[trophy_id];
                game.unlocked++;
                game.grade_unlocked[grade]++;
            }
            populate_trophy_table(db_idx);
        });
    }

    menu.exec(m_trophy_table->viewport()->mapToGlobal(pos));
}

void TrophyCollectionDialog::show_game_context_menu(const QPoint &pos) {
    const QModelIndex idx = m_game_table->indexAt(pos);
    if (!idx.isValid())
        return;

    const int db_idx = m_game_model->item(idx.row(), GC_Name)->data(RoleRawValue).toInt();
    if (db_idx < 0 || db_idx >= static_cast<int>(m_db.size()))
        return;

    const QString name = m_db[db_idx]->title;
    const fs::path conf = m_emuenv.pref_path / "ux0/user"
        / m_emuenv.io.user_id / "trophy/conf" / m_db[db_idx]->np_com_id;
    const fs::path data_dir = m_emuenv.pref_path / "ux0/user"
        / m_emuenv.io.user_id / "trophy/data" / m_db[db_idx]->np_com_id;

    QMenu menu(this);
    connect(menu.addAction(tr("&Remove")), &QAction::triggered, this, [=, this] {
        if (QMessageBox::question(this, tr("Confirm delete"),
                tr("Delete all trophies for:\n%1?").arg(name),
                QMessageBox::Yes | QMessageBox::No)
            != QMessageBox::Yes)
            return;
        fs::remove_all(conf);
        fs::remove_all(data_dir);
        m_db.erase(m_db.begin() + db_idx);
        m_selected_game_idx = -1;
        populate_game_table();
    });

    menu.exec(m_game_table->viewport()->mapToGlobal(pos));
}

void TrophyCollectionDialog::jump_to_trophy(const QString &np_com_id, int trophy_id) {
    for (int i = 0; i < static_cast<int>(m_db.size()); ++i) {
        if (QString::fromStdString(m_db[i]->np_com_id) != np_com_id)
            continue;
        m_game_table->selectRow(i);
        m_selected_game_idx = i;
        on_game_selection_changed(i);
        m_stacked->setCurrentIndex(1);
        for (int row = 0; row < m_trophy_proxy->rowCount(); ++row) {
            const QModelIndex pi = m_trophy_proxy->index(row, TC_Icon);
            const QModelIndex src = m_trophy_proxy->mapToSource(pi);
            if (m_trophy_model->item(src.row(), TC_Icon)->data(RoleTrophyId).toInt() == trophy_id) {
                m_trophy_table->selectRow(row);
                m_trophy_table->scrollTo(pi);
                break;
            }
        }
        break;
    }
    raise();
    activateWindow();
}

void TrophyCollectionDialog::refresh_game(const QString &np_com_id) {
    for (int i = 0; i < static_cast<int>(m_db.size()); ++i) {
        if (QString::fromStdString(m_db[i]->np_com_id) != np_com_id)
            continue;
        if (load_game_data(np_com_id)) {
            m_db[i] = std::move(m_db.back());
            m_db.pop_back();
        }
        if (m_selected_game_idx == i)
            populate_trophy_table(i);
        break;
    }
}

void TrophyCollectionDialog::on_trophy_unlocked(const TrophyUnlockData &data) {
    if (isVisible())
        refresh_game(data.np_com_id);
}

void TrophyCollectionDialog::reload() {
    m_selected_game_idx = -1;
    m_stacked->setCurrentIndex(0);
    load_all_games();
}

QString TrophyCollectionDialog::format_timestamp(quint64 unix_sec) {
    if (!unix_sec)
        return tr("Unknown");

    return QLocale().toString(
        QDateTime::fromSecsSinceEpoch(static_cast<qint64>(unix_sec)),
        QLocale::LongFormat);
}