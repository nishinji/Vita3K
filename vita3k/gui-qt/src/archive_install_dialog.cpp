#include <gui-qt/archive_install_dialog.h>
#include <util/string_utils.h>

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFrame>
#include <QLabel>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QThread>
#include <QVBoxLayout>

#include <set>

namespace {

std::vector<fs::path> archives_in_dir(const fs::path &dir) {
    std::vector<fs::path> result;
    for (const auto &entry : fs::directory_iterator(dir)) {
        if (!entry.is_regular_file())
            continue;
        const auto ext = string_utils::tolower(entry.path().extension().string());
        if (ext == ".zip" || ext == ".vpk")
            result.push_back(entry.path());
    }
    return result;
}

} // namespace

ArchiveInstallDialog::ArchiveInstallDialog(EmuEnvState &emuenv, QWidget *parent)
    : QDialog(parent)
    , m_emuenv(emuenv) {
    setWindowTitle(tr("Install Archive"));
    setModal(true);

    const std::vector<fs::path> archives = pick_archives();
    if (archives.empty()) {
        QMetaObject::invokeMethod(this, &QDialog::reject, Qt::QueuedConnection);
        return;
    }

    run_install(archives);
}

std::vector<fs::path> ArchiveInstallDialog::pick_archives() {
    QDialog type_picker(this);
    type_picker.setWindowTitle(tr("Select Install Type"));
    type_picker.setModal(true);

    auto *layout = new QVBoxLayout(&type_picker);
    layout->addWidget(new QLabel(tr("What would you like to install?"), &type_picker));

    auto *btn_file = new QPushButton(tr("Select a file (.zip / .vpk)..."), &type_picker);
    auto *btn_dir = new QPushButton(tr("Select a directory (installs all archives inside)..."), &type_picker);
    auto *btn_cancel = new QPushButton(tr("Cancel"), &type_picker);
    layout->addWidget(btn_file);
    layout->addWidget(btn_dir);
    layout->addWidget(btn_cancel);

    enum class Choice { None,
        File,
        Dir };
    Choice choice = Choice::None;

    connect(btn_file, &QPushButton::clicked, &type_picker, [&] {
        choice = Choice::File;
        type_picker.accept();
    });
    connect(btn_dir, &QPushButton::clicked, &type_picker, [&] {
        choice = Choice::Dir;
        type_picker.accept();
    });
    connect(btn_cancel, &QPushButton::clicked, &type_picker, &QDialog::reject);

    if (type_picker.exec() != QDialog::Accepted || choice == Choice::None)
        return {};

    if (choice == Choice::File) {
        const QString path = QFileDialog::getOpenFileName(
            this,
            tr("Select Archive"),
            QString(),
            tr("PlayStation Vita commercial software package (NoNpDrm/FAGDec) / PlayStation Vita homebrew software package;;PlayStation Vita commercial software package (NoNpDrm/FAGDec) (*.zip);;PlayStation Vita homebrew software package (*.vpk)"));
        if (path.isEmpty())
            return {};
        return { fs::path(path.toStdString()) };
    }

    const QString dir = QFileDialog::getExistingDirectory(
        this,
        tr("Select Directory Containing Archives"));
    if (dir.isEmpty())
        return {};

    auto archives = archives_in_dir(fs::path(dir.toStdString()));
    if (archives.empty()) {
        QMessageBox::information(this, tr("No Archives Found"),
            tr("The selected directory contains no .zip or .vpk files."));
        return {};
    }
    return archives;
}
void ArchiveInstallDialog::run_install(const std::vector<fs::path> &archives) {
    const int total_archives = static_cast<int>(archives.size());

    auto *root_layout = new QVBoxLayout(this);

    auto *app_label = new QLabel(tr("Installing..."), this);
    app_label->setAlignment(Qt::AlignCenter);
    root_layout->addWidget(app_label);

    auto *global_label = new QLabel(tr("Archive: 0 / %1").arg(total_archives), this);
    root_layout->addWidget(global_label);
    auto *global_bar = new QProgressBar(this);
    global_bar->setRange(0, total_archives);
    global_bar->setValue(0);
    root_layout->addWidget(global_bar);

    auto *content_label = new QLabel(tr("Content: 0 / 0"), this);
    root_layout->addWidget(content_label);
    auto *content_bar = new QProgressBar(this);
    content_bar->setRange(0, 1);
    content_bar->setValue(0);
    root_layout->addWidget(content_bar);

    auto *file_bar = new QProgressBar(this);
    file_bar->setRange(0, 100);
    file_bar->setValue(0);
    root_layout->addWidget(file_bar);

    setWindowFlags(windowFlags() & ~Qt::WindowCloseButtonHint);
    setMinimumWidth(450);

    const ReinstallCallback reinstall_cb = [this]() -> bool {
        bool result = false;
        QMetaObject::invokeMethod(this, [this, &result]() { result = (QMessageBox::question(
                                                                          this,
                                                                          tr("Already Installed"),
                                                                          tr("This content is already installed.\nDo you want to overwrite it?"),
                                                                          QMessageBox::Yes | QMessageBox::No,
                                                                          QMessageBox::No)
                                                                == QMessageBox::Yes); }, Qt::BlockingQueuedConnection);
        return result;
    };

    auto *thread = new QThread(this);
    auto *worker = new ArchiveWorker(m_emuenv, archives, reinstall_cb);
    worker->moveToThread(thread);

    connect(thread, &QThread::started, worker, &ArchiveWorker::run);

    connect(worker, &ArchiveWorker::global_progress, this,
        [global_bar, global_label, total_archives](int done, int) {
            global_bar->setValue(done);
            global_label->setText(QObject::tr("Archive: %1 / %2").arg(done).arg(total_archives));
        });

    connect(worker, &ArchiveWorker::content_progress, this,
        [content_bar, content_label](int done, int total) {
            content_bar->setRange(0, total);
            content_bar->setValue(done);
            content_label->setText(QObject::tr("Content: %1 / %2").arg(done).arg(total));
        });

    connect(worker, &ArchiveWorker::file_progress, this,
        [file_bar](float pct) {
            const int v = static_cast<int>(pct);
            file_bar->setValue(v);
        });

    connect(worker, &ArchiveWorker::current_title_changed, this,
        [app_label](const QString &title) {
            app_label->setText(title);
        });
    connect(worker, &ArchiveWorker::finished, this,
        [this, worker, thread](bool) {
            thread->quit();
            thread->wait();

            const QList<ArchiveInstallResult> results = worker->results();
            worker->deleteLater();

            setWindowFlags(windowFlags() | Qt::WindowCloseButtonHint);
            show();

            QLayoutItem *item;
            while ((item = layout()->takeAt(0)) != nullptr) {
                delete item->widget();
                delete item;
            }

            auto *scroll = new QScrollArea(this);
            scroll->setWidgetResizable(true);
            auto *inner = new QWidget(scroll);
            auto *inner_layout = new QVBoxLayout(inner);

            QMap<QString, QList<const ArchiveInstallResult *>> by_archive;
            for (const auto &r : results)
                by_archive[r.archive_name].append(&r);

            for (auto it = by_archive.constBegin(); it != by_archive.constEnd(); ++it) {
                inner_layout->addWidget(new QLabel(QStringLiteral("<b>%1</b>").arg(it.key()), inner));
                for (const auto *r : it.value()) {
                    const QString line = r->success
                        ? QStringLiteral("  OK  %1 [%2]").arg(r->title, r->title_id)
                        : QStringLiteral("  FAIL  %1 [%2]").arg(r->title, r->title_id);
                    auto *lbl = new QLabel(line, inner);
                    lbl->setStyleSheet(r->success ? QStringLiteral("color: green;") : QStringLiteral("color: red;"));
                    inner_layout->addWidget(lbl);
                }
                auto *sep = new QFrame(inner);
                sep->setFrameShape(QFrame::HLine);
                inner_layout->addWidget(sep);
            }

            inner->setLayout(inner_layout);
            scroll->setWidget(inner);
            layout()->addWidget(scroll);

            auto *del_check = new QCheckBox(tr("Delete archive files after install"), this);
            layout()->addWidget(del_check);

            auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok, this);
            layout()->addWidget(buttons);

            connect(buttons, &QDialogButtonBox::accepted, this,
                [this, del_check, results]() {
                    if (del_check->isChecked()) {
                        std::set<fs::path> deleted;
                        for (const auto &r : results) {
                            if (!r.archive_full_path.empty() && deleted.insert(r.archive_full_path).second)
                                fs::remove(r.archive_full_path);
                        }
                    }
                    Q_EMIT install_complete(results);
                    accept();
                });

            resize(500, 400);
            show();
        });

    thread->start();
}