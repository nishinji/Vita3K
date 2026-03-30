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

#include <gui-qt/log_widget.h>
#include <util/log.h>

#include <spdlog/common.h>

#include <QFont>
#include <QPlainTextEdit>
#include <QPointer>
#include <QScrollBar>
#include <QVBoxLayout>

#include <deque>
#include <mutex>
#include <utility>

LogWidget::LogWidget(QWidget *parent)
    : custom_dock_widget("Log", parent) {
    auto *container = new QWidget(this);
    auto *layout = new QVBoxLayout(container);
    layout->setContentsMargins(0, 0, 0, 0);

    m_log = new QPlainTextEdit(container);
    m_log->setReadOnly(true);
    m_log->setFont(QFont("Monospace", 9));
    m_log->document()->setMaximumBlockCount(1000);
    m_log->setStyleSheet("QPlainTextEdit { background-color: #1e1e1e; }");
    layout->addWidget(m_log);
    setWidget(container);

    connect(this, &LogWidget::log_message_received, this, &LogWidget::on_log_message, Qt::QueuedConnection);
}
static struct {
    std::mutex mutex;
    std::function<void(std::string, int)> callback;
    std::deque<std::pair<std::string, int>> backlog;
} s_log_state;

static constexpr size_t MAX_BACKLOG_LINES = 2000;

void LogWidget::register_callback() {
    logging::set_log_callback([](std::string msg, int level) {
        const std::lock_guard<std::mutex> lock(s_log_state.mutex);
        if (s_log_state.callback) {
            s_log_state.callback(std::move(msg), level);
            return;
        }

        if (s_log_state.backlog.size() >= MAX_BACKLOG_LINES)
            s_log_state.backlog.pop_front();
        s_log_state.backlog.emplace_back(std::move(msg), level);
    });
}

void LogWidget::attach() {
    QPointer<LogWidget> self(this);
    std::deque<std::pair<std::string, int>> backlog;

    {
        const std::lock_guard<std::mutex> lock(s_log_state.mutex);
        s_log_state.callback = [self](std::string msg, int level) {
            if (!self)
                return;
            emit self->log_message_received(QString::fromStdString(msg).trimmed(), level);
        };
        backlog = std::move(s_log_state.backlog);
        s_log_state.backlog.clear();
    }

    for (const auto &[msg, level] : backlog) {
        const auto qmsg = QString::fromStdString(msg).trimmed();
        QMetaObject::invokeMethod(this, [this, qmsg, level]() { on_log_message(qmsg, level); }, Qt::QueuedConnection);
    }
}

void LogWidget::on_log_message(const QString &msg, int level) {
    using L = spdlog::level::level_enum;

    QColor color;
    switch (static_cast<L>(level)) {
    case L::critical:
    case L::err: color = QColor(255, 80, 80); break;
    case L::warn: color = QColor(255, 200, 0); break;
    case L::info: color = Qt::white; break;
    default: color = QColor(180, 180, 180); break;
    }

    QScrollBar *bar = m_log->verticalScrollBar();
    const bool at_bottom = bar->value() == bar->maximum();

    QTextCursor cursor = m_log->textCursor();
    cursor.movePosition(QTextCursor::End);

    QTextCharFormat fmt;
    fmt.setForeground(color);

    cursor.insertText(msg + "\n", fmt);

    if (at_bottom)
        bar->setValue(bar->maximum());
}

void LogWidget::set_log_buffer_size(int size) {
    m_log->document()->setMaximumBlockCount(size);
}

int LogWidget::log_buffer_size() const {
    return m_log->document()->maximumBlockCount();
}