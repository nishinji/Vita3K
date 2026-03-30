#pragma once

#include <QDialog>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>

#include <archive.h>
#include <emuenv/state.h>
#include <util/fs.h>

#include <vector>

struct ArchiveInstallResult {
    QString archive_name;
    fs::path archive_full_path;
    QString title;
    QString title_id;
    QString category;
    QString content_id;
    bool success = false;
};

class ArchiveWorker : public QObject {
    Q_OBJECT
public:
    ArchiveWorker(EmuEnvState &emuenv,
        const std::vector<fs::path> &archives,
        ReinstallCallback reinstall_cb,
        QObject *parent = nullptr)
        : QObject(parent)
        , m_emuenv(emuenv)
        , m_archives(archives)
        , m_reinstall_cb(std::move(reinstall_cb)) {}

    QList<ArchiveInstallResult> results() const { return m_results; }

Q_SIGNALS:
    void progress(float pct);
    void finished(bool success);
    void global_progress(int done, int total);
    void content_progress(int done, int total);
    void file_progress(float pct);
    void current_title_changed(const QString &title);

public Q_SLOTS:
    void run() {
        const int total = static_cast<int>(m_archives.size());
        int done = 0;

        for (const auto &archive_path : m_archives) {
            Q_EMIT global_progress(done, total);

            const auto progress_cb = [this](const ArchiveContents &v) {
                if (v.count.has_value() && v.current.has_value())
                    Q_EMIT content_progress(
                        static_cast<int>(*v.current),
                        static_cast<int>(*v.count));
                if (v.progress.has_value())
                    Q_EMIT file_progress(*v.progress);
            };

            const std::vector<ContentInfo> contents = install_archive(m_emuenv, fs::path(archive_path.string()), progress_cb, m_reinstall_cb);

            const QString archive_name = QString::fromStdString(archive_path.filename().string());

            if (contents.empty()) {
                ArchiveInstallResult r;
                r.archive_name = archive_name;
                r.archive_full_path = archive_path;
                r.title = QObject::tr("(incompatible or no content found)");
                r.success = false;
                m_results.append(r);
            } else {
                for (const auto &info : contents) {
                    ArchiveInstallResult r;
                    r.archive_name = archive_name;
                    r.archive_full_path = archive_path;
                    r.title = QString::fromStdString(info.title);
                    r.title_id = QString::fromStdString(info.title_id);
                    r.category = QString::fromStdString(info.category);
                    r.content_id = QString::fromStdString(info.content_id);
                    r.success = info.state;
                    m_results.append(r);

                    Q_EMIT current_title_changed(r.title);
                }
            }

            ++done;
            Q_EMIT global_progress(done, total);
        }

        Q_EMIT finished(true);
    }

private:
    EmuEnvState &m_emuenv;
    std::vector<fs::path> m_archives;
    ReinstallCallback m_reinstall_cb;
    QList<ArchiveInstallResult> m_results;
};

class ArchiveInstallDialog : public QDialog {
    Q_OBJECT
public:
    explicit ArchiveInstallDialog(EmuEnvState &emuenv, QWidget *parent = nullptr);

Q_SIGNALS:
    void install_complete(const QList<ArchiveInstallResult> &results);

private:
    std::vector<fs::path> pick_archives();

    void run_install(const std::vector<fs::path> &archives);

    EmuEnvState &m_emuenv;
};