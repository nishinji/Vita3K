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

#include <gui-qt/pkg_install_dialog.h>
#include <packages/license.h>
#include <packages/sfo.h>
#include <rif2zrif.h>

#include <fstream>

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QInputDialog>
#include <QLabel>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QThread>
#include <QVBoxLayout>

PkgInstallDialog::PkgInstallDialog(EmuEnvState &emuenv, QWidget *parent)
    : QDialog(parent)
    , m_emuenv(emuenv) {
    setWindowTitle(tr("Install PKG"));
    setModal(true);

    const QString pkg_qt = QFileDialog::getOpenFileName(
        this,
        tr("Select PKG File"),
        QString(),
        tr("PlayStation Store Downloaded Package (*.pkg)"));

    if (pkg_qt.isEmpty()) {
        QMetaObject::invokeMethod(this, &QDialog::reject, Qt::QueuedConnection);
        return;
    }

    const fs::path pkg_path(pkg_qt.toStdString());

    const QString zrif = acquire_zrif(pkg_path);
    if (zrif.isEmpty()) {
        QMetaObject::invokeMethod(this, &QDialog::reject, Qt::QueuedConnection);
        return;
    }

    run_install(pkg_path, zrif);
}

QString PkgInstallDialog::acquire_zrif(const fs::path &pkg_path) {
    const std::string auto_zrif = find_pkg_zrif(pkg_path, m_emuenv.pref_path);
    if (!auto_zrif.empty())
        return QString::fromStdString(auto_zrif);

    QDialog picker(this);
    picker.setWindowTitle(tr("Select License Type"));
    picker.setModal(true);

    auto *layout = new QVBoxLayout(&picker);
    layout->addWidget(new QLabel(tr("No license was found automatically.\nHow would you like to provide it?"), &picker));

    auto *btn_file = new QPushButton(tr("Select .bin / .rif file…"), &picker);
    auto *btn_zrif = new QPushButton(tr("Enter zRIF key manually…"), &picker);
    auto *btn_cancel = new QPushButton(tr("Cancel"), &picker);
    layout->addWidget(btn_file);
    layout->addWidget(btn_zrif);
    layout->addWidget(btn_cancel);

    QString result_zrif;

    connect(btn_file, &QPushButton::clicked, &picker, [&]() {
        const QString license_path_qt = QFileDialog::getOpenFileName(
            &picker,
            tr("Select License File"),
            QString(),
            tr("PlayStation Vita software license file (*.bin *.rif)"));

        if (license_path_qt.isEmpty())
            return;

        std::ifstream binfile(license_path_qt.toStdString(),
            std::ios::binary | std::ios::ate);
        if (!binfile)
            return;

        result_zrif = QString::fromStdString(rif2zrif(binfile));
        m_license_path = fs::path(license_path_qt.toStdString());
        picker.accept();
    });

    connect(btn_zrif, &QPushButton::clicked, &picker, [&]() {
        bool ok = false;
        const QString key = QInputDialog::getText(
            &picker,
            tr("Enter zRIF Key"),
            tr("Paste your zRIF key:"),
            QLineEdit::Normal,
            QString(),
            &ok);
        if (ok && !key.trimmed().isEmpty()) {
            if (!validate_zrif(key.trimmed().toStdString())) {
                QMessageBox::critical(&picker, tr("Invalid zRIF Key"),
                    tr("The zRIF key you entered is not valid.\nPlease check and try again."));
                return;
            }
            result_zrif = key.trimmed();
            picker.accept();
        }
    });

    connect(btn_cancel, &QPushButton::clicked, &picker, &QDialog::reject);

    picker.exec();
    return result_zrif;
}

void PkgInstallDialog::run_install(const fs::path &pkg_path,
    const QString &zrif) {
    auto *main_layout = new QVBoxLayout(this);

    auto *title_label = new QLabel(tr("Installing package, please wait…"), this);
    title_label->setAlignment(Qt::AlignCenter);
    main_layout->addWidget(title_label);

    auto *pkg_label = new QLabel(QString::fromStdString(pkg_path.filename().string()), this);
    pkg_label->setAlignment(Qt::AlignCenter);
    main_layout->addWidget(pkg_label);

    auto *bar = new QProgressBar(this);
    bar->setRange(0, 100);
    bar->setValue(0);
    main_layout->addWidget(bar);

    setWindowFlags(windowFlags() & ~Qt::WindowCloseButtonHint);
    setMinimumWidth(400);

    auto *thread = new QThread(this);
    auto *worker = new PkgWorker(m_emuenv, pkg_path, zrif.toStdString());
    worker->moveToThread(thread);

    connect(thread, &QThread::started, worker, &PkgWorker::run);

    connect(worker, &PkgWorker::progress, this, [bar](float pct) {
        const int v = static_cast<int>(pct);
        bar->setValue(v);
    });

    connect(worker, &PkgWorker::finished, this,
        [this, worker, thread, pkg_path, zrif](bool success) {
            thread->quit();
            thread->wait();
            worker->deleteLater();

            setWindowFlags(windowFlags() | Qt::WindowCloseButtonHint);
            show();

            if (!success) {
                QMessageBox::critical(this, tr("Installation Failed"),
                    tr("Failed to install the package.\nCheck the log for details."));
                reject();
                return;
            }

            QLayoutItem *item;
            while ((item = layout()->takeAt(0)) != nullptr) {
                delete item->widget();
                delete item;
            }

            const QString app_title = QString::fromStdString(m_emuenv.app_info.app_title);
            const QString title_id = QString::fromStdString(m_emuenv.app_info.app_title_id);

            auto *msg = new QLabel(
                tr("Installation complete!\n\n%1 [%2]").arg(app_title, title_id),
                this);
            msg->setAlignment(Qt::AlignCenter);
            layout()->addWidget(msg);

            auto *del_pkg = new QCheckBox(tr("Delete PKG file after install"), this);
            layout()->addWidget(del_pkg);

            auto *del_bin = new QCheckBox(tr("Delete .bin / .rif file after install"), this);
            del_bin->setVisible(!m_license_path.empty());
            layout()->addWidget(del_bin);

            auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok, this);
            layout()->addWidget(buttons);

            connect(buttons, &QDialogButtonBox::accepted, this,
                [this, del_pkg, del_bin, pkg_path, title_id]() {
                    if (del_pkg->isChecked())
                        fs::remove(pkg_path);
                    if (del_bin->isChecked() && !m_license_path.empty())
                        fs::remove(m_license_path);

                    Q_EMIT install_complete(title_id);
                    accept();
                });

            adjustSize();
        });

    thread->start();
}
