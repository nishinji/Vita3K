#pragma once

#include <app/app_list.h>
#include <config/state.h>
#include <emuenv/state.h>
#include <io/vfs.h>

#include <QAbstractTableModel>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QPixmap>
#include <QSettings>
#include <QSortFilterProxyModel>
#include <QStackedWidget>
#include <QStyledItemDelegate>
#include <QTableView>
#include <QVBoxLayout>
#include <QWidget>

#include <array>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class AppListSortModel;

class AppListModel final : public QAbstractTableModel {
    Q_OBJECT

public:
    enum Column : int {
        COLUMN_ICON,
        COLUMN_COMPATIBILITY,
        COLUMN_SERIAL,
        COLUMN_VERSION,
        COLUMN_CATEGORY,
        COLUMN_LAST_PLAYED,
        COLUMN_NAME,

        COLUMN_COUNT
    };

    AppListModel(EmuEnvState &emuenv, QObject *parent = nullptr);
    ~AppListModel();

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    int columnCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override;
    const app_list::app &app_at(int row) const { return m_apps[row]; }
    void refresh();

signals:
    void icon_loaded(const QString &path, const QPixmap &pixmap);

private slots:
    void on_icon_loaded(const QString &path, const QPixmap &pixmap);

private:
    EmuEnvState &emuenv;

    void set_column_display_names();

    std::vector<app_list::app> m_apps;
    std::array<QString, COLUMN_COUNT> m_column_display_names;

    mutable std::unordered_map<std::string, QPixmap> m_icon_cache;
    mutable std::unordered_set<std::string> m_loading;
    QPixmap m_placeholder_pixmap;
    QString m_fallback_icon_path;
};

class AppListWidget final : public QWidget {
    Q_OBJECT

public:
    AppListWidget(EmuEnvState &emuenv, QWidget *parent = nullptr);
    ~AppListWidget();

signals:
    void game_selected(const QString &title_id);

private:
    void show_column_menu(const QPoint &pos);
    void show_context_menu(const QPoint &pos);
    void update_status_label();
    void save_state();
    void restore_state();

    QTableView *m_view = nullptr;
    QLineEdit *m_search_bar = nullptr;
    QLabel *m_status_label = nullptr;
    QStackedWidget *m_stack = nullptr;
    QLabel *m_empty_label = nullptr;

    AppListModel *m_model = nullptr;
    AppListSortModel *m_sort_model = nullptr;
};
