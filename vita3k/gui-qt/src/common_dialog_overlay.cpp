#include <gui-qt/common_dialog_overlay.h>

#include <config/state.h>
#include <emuenv/state.h>
#include <util/string_utils.h>

#include <cmath>

#include <QEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QPalette>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QTextEdit>
#include <QTimer>
#include <QVBoxLayout>

#include <SDL3/SDL_timer.h>

namespace {

QString get_save_date_time(int date_format, const SceDateTime &dt) {
    QString date_str;
    switch (date_format) {
    case SCE_SYSTEM_PARAM_DATE_FORMAT_YYYYMMDD:
        date_str = QStringLiteral("%1/%2/%3").arg(dt.year).arg(dt.month).arg(dt.day);
        break;
    case SCE_SYSTEM_PARAM_DATE_FORMAT_DDMMYYYY:
        date_str = QStringLiteral("%1/%2/%3").arg(dt.day).arg(dt.month).arg(dt.year);
        break;
    case SCE_SYSTEM_PARAM_DATE_FORMAT_MMDDYYYY:
        date_str = QStringLiteral("%1/%2/%3").arg(dt.month).arg(dt.day).arg(dt.year);
        break;
    default:
        break;
    }
    return date_str + QStringLiteral(" %1:%2").arg(dt.hour).arg(dt.minute, 2, 10, QLatin1Char('0'));
}

} // namespace

CommonDialogOverlay::CommonDialogOverlay(QWidget *game_widget, EmuEnvState &emuenv)
    : GameOverlayBase(game_widget, emuenv.display)
    , m_emuenv(emuenv) {
    setObjectName(QStringLiteral("CommonDialogOverlay"));

    m_rebuild_timer = new QTimer(this);
    m_rebuild_timer->setSingleShot(true);
    m_rebuild_timer->setInterval(250);
    connect(m_rebuild_timer, &QTimer::timeout, this, &CommonDialogOverlay::rebuild_for_scale);
}

void CommonDialogOverlay::update_dialog() {
    auto &dialog = m_emuenv.common_dialog;

    if (dialog.status != SCE_COMMON_DIALOG_STATUS_RUNNING) {
        if (m_active_type != NO_DIALOG)
            dismiss();
        return;
    }

    const qreal current_scale = scale_x();

    if (m_active_type != dialog.type) {
        clear_content();
        m_active_type = dialog.type;

        switch (dialog.type) {
        case IME_DIALOG:
            build_ime_dialog();
            break;
        case MESSAGE_DIALOG:
            build_message_dialog();
            break;
        case TROPHY_SETUP_DIALOG:
            build_trophy_setup_dialog();
            break;
        case SAVEDATA_DIALOG:
            build_savedata_dialog();
            break;
        default:
            break;
        }

        m_built_scale = current_scale;
        show_overlay();
    } else if (m_active_type != NO_DIALOG && !isVisible()) {
        show_overlay();
    }

    switch (dialog.type) {
    case MESSAGE_DIALOG:
        update_message_progress();
        break;
    case TROPHY_SETUP_DIALOG:
        update_trophy_timer();
        break;
    case SAVEDATA_DIALOG:
        update_savedata_progress();
        break;
    default:
        break;
    }
}

void CommonDialogOverlay::dismiss() {
    m_rebuild_timer->stop();
    clear_content();
    m_active_type = NO_DIALOG;
    hide_overlay();
}

void CommonDialogOverlay::rebuild_for_scale() {
    auto &dialog = m_emuenv.common_dialog;
    if (dialog.status != SCE_COMMON_DIALOG_STATUS_RUNNING || m_active_type == NO_DIALOG)
        return;

    const qreal current_scale = scale_x();
    if (current_scale <= 0)
        return;

    clear_content();

    switch (m_active_type) {
    case IME_DIALOG:
        build_ime_dialog();
        break;
    case MESSAGE_DIALOG:
        build_message_dialog();
        break;
    case TROPHY_SETUP_DIALOG:
        build_trophy_setup_dialog();
        break;
    case SAVEDATA_DIALOG:
        build_savedata_dialog();
        break;
    default:
        break;
    }

    if (m_content)
        m_content->show();

    m_built_scale = current_scale;
    on_reposition();
}

void CommonDialogOverlay::on_reposition() {
    const QRect vp = viewport_rect();
    if (vp.isEmpty())
        return;

    if (m_content) {
        if (m_content_fullscreen) {
            m_content->setGeometry(vp);
        } else {
            const QSize hint = m_content->sizeHint();
            const int cw = qBound(m_content->minimumWidth(), hint.width(), m_content->maximumWidth());
            const int ch = qBound(m_content->minimumHeight(), hint.height(), m_content->maximumHeight());
            const int cx = vp.x() + (vp.width() - cw) / 2;
            const int cy = vp.y() + (vp.height() - ch) / 2;
            m_content->setGeometry(cx, cy, cw, ch);
        }
    }

    const qreal current_scale = scale_x();
    if (m_active_type != NO_DIALOG && m_built_scale > 0 && current_scale > 0
        && std::abs(current_scale - m_built_scale) > 0.05) {
        m_rebuild_timer->start();
    }
}

void CommonDialogOverlay::clear_content() {
    if (m_content) {
        m_content->deleteLater();
        m_content = nullptr;
    }
    m_content_layout = nullptr;
    m_content_fullscreen = false;
    m_ime_input = nullptr;
    m_ime_multiline = nullptr;
    m_message_label = nullptr;
    m_progress_bar = nullptr;
    m_progress_label = nullptr;
    m_trophy_label = nullptr;
    m_save_scroll = nullptr;
    m_icon_cache.clear();
    m_buttons.clear();
}

void CommonDialogOverlay::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.fillRect(rect(), QColor(0, 0, 0, 120));
}

bool CommonDialogOverlay::eventFilter(QObject *watched, QEvent *event) {
    if (event->type() == QEvent::MouseButtonRelease) {
        auto *widget = qobject_cast<QWidget *>(watched);
        if (widget) {
            const QVariant slot_var = widget->property("slot_index");
            if (slot_var.isValid()) {
                on_savedata_slot_clicked(slot_var.toUInt());
                return true;
            }
        }
    }
    return GameOverlayBase::eventFilter(watched, event);
}

void CommonDialogOverlay::build_ime_dialog() {
    auto &dialog = m_emuenv.common_dialog;
    auto &ime = dialog.ime;

    const qreal sx = scale_x();

    m_content = new QWidget(this);
    m_content->setStyleSheet(QStringLiteral(
        "QWidget#dialog_card { background: rgb(37, 40, 45); border-radius: %1px; }"
        "QLabel { color: white; background: transparent; }"
        "QPushButton { min-height: %2px; min-width: %3px; border-radius: %4px;"
        "  background: rgba(255,255,255,20); color: white; font-weight: 600; padding: 4px 16px; }"
        "QPushButton:hover { background: rgba(255,255,255,40); }"
        "QPushButton#cancel_btn { background: rgba(255,255,255,10); }"
        "QPushButton#cancel_btn:hover { background: rgba(255,255,255,25); }"
        "QLineEdit, QTextEdit { font-size: %5px; padding: 6px; border: 1px solid rgba(255,255,255,40);"
        "  border-radius: 4px; background: rgba(255,255,255,15); color: white; }")
            .arg(static_cast<int>(10 * sx))
            .arg(static_cast<int>(32 * sx))
            .arg(static_cast<int>(90 * sx))
            .arg(static_cast<int>(6 * sx))
            .arg(static_cast<int>(14 * sx)));
    m_content->setObjectName(QStringLiteral("dialog_card"));

    m_content_layout = new QVBoxLayout(m_content);
    m_content_layout->setContentsMargins(k_margin, k_margin, k_margin, k_margin);
    m_content_layout->setSpacing(10);

    auto *title = new QLabel(QString::fromUtf8(ime.title), m_content);
    title->setAlignment(Qt::AlignCenter);
    auto title_font = title->font();
    title_font.setPixelSize(static_cast<int>(16 * sx));
    title_font.setBold(true);
    title->setFont(title_font);
    m_content_layout->addWidget(title);

    if (ime.multiline) {
        m_ime_multiline = new QTextEdit(m_content);
        m_ime_multiline->setPlainText(QString::fromUtf8(ime.text));
        m_ime_multiline->setMinimumHeight(80);
        m_content_layout->addWidget(m_ime_multiline);
    } else {
        m_ime_input = new QLineEdit(m_content);
        m_ime_input->setText(QString::fromUtf8(ime.text));
        m_ime_input->setMaxLength(static_cast<int>(ime.max_length));
        connect(m_ime_input, &QLineEdit::returnPressed, this, &CommonDialogOverlay::on_ime_submit);
        m_content_layout->addWidget(m_ime_input);
    }

    auto *btn_row = new QHBoxLayout;
    btn_row->addStretch();

    auto *submit_btn = new QPushButton(tr("Submit"), m_content);
    connect(submit_btn, &QPushButton::clicked, this, &CommonDialogOverlay::on_ime_submit);
    btn_row->addWidget(submit_btn);

    if (ime.cancelable) {
        auto *cancel_btn = new QPushButton(tr("Cancel"), m_content);
        cancel_btn->setObjectName(QStringLiteral("cancel_btn"));
        connect(cancel_btn, &QPushButton::clicked, this, &CommonDialogOverlay::on_ime_cancel);
        btn_row->addWidget(cancel_btn);
    }

    m_content_layout->addLayout(btn_row);

    if (m_ime_input)
        m_ime_input->setFocus();
    else if (m_ime_multiline)
        m_ime_multiline->setFocus();
}

void CommonDialogOverlay::on_ime_submit() {
    auto &dialog = m_emuenv.common_dialog;

    QString text;
    if (m_ime_multiline)
        text = m_ime_multiline->toPlainText();
    else if (m_ime_input)
        text = m_ime_input->text();

    if (static_cast<uint32_t>(text.length()) > dialog.ime.max_length)
        text.truncate(static_cast<int>(dialog.ime.max_length));

    const std::string utf8 = text.toStdString();
    snprintf(dialog.ime.text, sizeof(dialog.ime.text), "%s", utf8.c_str());

    const std::u16string result16 = string_utils::utf8_to_utf16(utf8);
    memcpy(dialog.ime.result, result16.c_str(), (result16.length() + 1) * sizeof(uint16_t));

    dialog.ime.status = SCE_IME_DIALOG_BUTTON_ENTER;
    dialog.status = SCE_COMMON_DIALOG_STATUS_FINISHED;
    dialog.result = SCE_COMMON_DIALOG_RESULT_OK;
}

void CommonDialogOverlay::on_ime_cancel() {
    auto &dialog = m_emuenv.common_dialog;
    dialog.ime.status = SCE_IME_DIALOG_BUTTON_CLOSE;
    dialog.status = SCE_COMMON_DIALOG_STATUS_FINISHED;
    dialog.result = SCE_COMMON_DIALOG_RESULT_USER_CANCELED;
}

void CommonDialogOverlay::build_message_dialog() {
    auto &dialog = m_emuenv.common_dialog;
    auto &msg = dialog.msg;

    const qreal sx = scale_x();
    const qreal sy = scale_y();

    m_content = new QWidget(this);
    m_content->setStyleSheet(QStringLiteral(
        "QWidget#dialog_card { background: rgb(37, 40, 45); border-radius: %1px; }"
        "QLabel { color: white; background: transparent; }"
        "QPushButton { min-height: %2px; min-width: %3px; border-radius: %4px;"
        "  background: rgba(255,255,255,20); color: white; font-weight: 600;"
        "  padding: 4px 16px; font-size: %5px; }"
        "QPushButton:hover { background: rgba(255,255,255,40); }"
        "QProgressBar { border: 1px solid rgba(255,255,255,40); border-radius: %6px; background: rgba(82,82,91,30); }"
        "QProgressBar::chunk { background: rgb(141,180,83); border-radius: %7px; }")
            .arg(static_cast<int>(10 * sx))
            .arg(static_cast<int>(35 * sy))
            .arg(static_cast<int>(200 * sx))
            .arg(static_cast<int>(10 * sx))
            .arg(static_cast<int>(14 * sx))
            .arg(static_cast<int>(4 * sx))
            .arg(static_cast<int>(3 * sx)));
    m_content->setObjectName(QStringLiteral("dialog_card"));

    const QRect vp = viewport_rect();
    m_content->setFixedSize(static_cast<int>(vp.width() / 1.7), static_cast<int>(vp.height() / 1.5));

    m_content_layout = new QVBoxLayout(m_content);
    m_content_layout->setContentsMargins(
        static_cast<int>(50 * sx), static_cast<int>(30 * sy),
        static_cast<int>(50 * sx), static_cast<int>(30 * sy));
    m_content_layout->setSpacing(static_cast<int>(12 * sy));

    m_content_layout->addStretch();

    m_message_label = new QLabel(QString::fromStdString(msg.message), m_content);
    m_message_label->setAlignment(Qt::AlignCenter);
    m_message_label->setWordWrap(true);
    auto msg_font = m_message_label->font();
    msg_font.setPixelSize(static_cast<int>(16 * sx));
    m_message_label->setFont(msg_font);
    m_content_layout->addWidget(m_message_label);

    if (msg.has_progress_bar) {
        m_progress_bar = new QProgressBar(m_content);
        m_progress_bar->setRange(0, 100);
        m_progress_bar->setValue(static_cast<int>(msg.bar_percent));
        m_progress_bar->setTextVisible(false);
        m_progress_bar->setFixedHeight(static_cast<int>(7 * sy));
        m_content_layout->addWidget(m_progress_bar);

        m_progress_label = new QLabel(QStringLiteral("%1%").arg(msg.bar_percent), m_content);
        m_progress_label->setAlignment(Qt::AlignCenter);
        auto f = m_progress_label->font();
        f.setPixelSize(static_cast<int>(14 * sx));
        m_progress_label->setFont(f);
        m_content_layout->addWidget(m_progress_label);
    }

    m_content_layout->addStretch();

    if (msg.btn_num > 0) {
        auto *btn_row = new QHBoxLayout;
        btn_row->addStretch();

        for (uint8_t i = 0; i < msg.btn_num; i++) {
            auto *btn = new QPushButton(QString::fromStdString(msg.btn[i]), m_content);
            const int idx = i;
            connect(btn, &QPushButton::clicked, this, [this, idx]() {
                on_msg_button_clicked(idx);
            });
            btn_row->addWidget(btn);
            m_buttons.append(btn);
        }
        btn_row->addStretch();
        m_content_layout->addLayout(btn_row);
    }
}

void CommonDialogOverlay::update_message_progress() {
    auto &msg = m_emuenv.common_dialog.msg;
    if (m_progress_bar) {
        m_progress_bar->setValue(static_cast<int>(msg.bar_percent));
    }
    if (m_progress_label) {
        m_progress_label->setText(QStringLiteral("%1%").arg(msg.bar_percent));
    }
    if (m_message_label) {
        const QString new_msg = QString::fromStdString(msg.message);
        if (m_message_label->text() != new_msg)
            m_message_label->setText(new_msg);
    }
}

void CommonDialogOverlay::on_msg_button_clicked(int index) {
    auto &dialog = m_emuenv.common_dialog;
    dialog.msg.status = dialog.msg.btn_val[index];
    dialog.result = SCE_COMMON_DIALOG_RESULT_OK;
    dialog.status = SCE_COMMON_DIALOG_STATUS_FINISHED;
}

void CommonDialogOverlay::build_trophy_setup_dialog() {
    auto &dialog = m_emuenv.common_dialog;

    const qreal sx = scale_x();
    const qreal sy = scale_y();

    m_content = new QWidget(this);
    m_content->setStyleSheet(QStringLiteral(
        "QWidget#dialog_card { background: rgb(37, 40, 45); border-radius: %1px; }"
        "QLabel { color: #ddd; background: transparent; }")
            .arg(static_cast<int>(10 * sx)));
    m_content->setObjectName(QStringLiteral("dialog_card"));
    m_content->setFixedSize(static_cast<int>(764 * sx), static_cast<int>(440 * sy));

    m_content_layout = new QVBoxLayout(m_content);
    m_content_layout->setContentsMargins(
        static_cast<int>(k_margin * 3 * sx), static_cast<int>(k_margin * 4 * sy),
        static_cast<int>(k_margin * 3 * sx), static_cast<int>(k_margin * 4 * sy));

    const QString text = tr("Preparing to start the application...");
    m_trophy_label = new QLabel(text, m_content);
    m_trophy_label->setAlignment(Qt::AlignCenter);
    auto f = m_trophy_label->font();
    f.setPixelSize(static_cast<int>(18 * sx));
    m_trophy_label->setFont(f);
    m_content_layout->addWidget(m_trophy_label);
}

void CommonDialogOverlay::update_trophy_timer() {
    auto &dialog = m_emuenv.common_dialog;
    const uint64_t now = SDL_GetTicks();
    if (now >= dialog.trophy.tick) {
        dialog.status = SCE_COMMON_DIALOG_STATUS_FINISHED;
        dialog.result = SCE_COMMON_DIALOG_RESULT_OK;
    }
}

QPixmap CommonDialogOverlay::load_icon_pixmap(uint32_t index) {
    if (m_icon_cache.contains(index))
        return m_icon_cache[index];

    auto &sd = m_emuenv.common_dialog.savedata;
    if (index < sd.icon_buffer.size() && !sd.icon_buffer[index].empty()) {
        QPixmap pix;
        if (pix.loadFromData(sd.icon_buffer[index].data(),
                static_cast<uint>(sd.icon_buffer[index].size()))) {
            m_icon_cache[index] = pix;
            return pix;
        }
    }
    return QPixmap();
}

void CommonDialogOverlay::build_savedata_dialog() {
    auto &sd = m_emuenv.common_dialog.savedata;

    switch (sd.mode_to_display) {
    case SCE_SAVEDATA_DIALOG_MODE_LIST:
        build_savedata_list_mode();
        break;
    default:
    case SCE_SAVEDATA_DIALOG_MODE_FIXED:
        build_savedata_fixed_mode();
        break;
    }
}

void CommonDialogOverlay::build_savedata_list_mode() {
    auto &dialog = m_emuenv.common_dialog;
    auto &sd = dialog.savedata;

    m_content_fullscreen = true;

    const qreal sx = scale_x();
    const qreal sy = scale_y();
    const int thumb_w = static_cast<int>(160 * sx);
    const int thumb_h = static_cast<int>(90 * sy);

    m_content = new QWidget(this);
    m_content->setAttribute(Qt::WA_TranslucentBackground);
    m_content->setStyleSheet(QStringLiteral(
        "QWidget#savedata_list_root { background: transparent; }"
        "QLabel { color: white; background: transparent; }"
        "QPushButton#cancel_x { background: rgba(255,255,255,30); color: white;"
        "  font-size: %1px; font-weight: bold; border-radius: %2px;"
        "  min-width: %3px; max-width: %3px; min-height: %3px; max-height: %3px; }"
        "QPushButton#cancel_x:hover { background: rgba(255,255,255,60); }"
        "QScrollArea { background: transparent; border: none; }"
        "QScrollBar:vertical { width: %4px; background: transparent; }"
        "QScrollBar::handle:vertical { background: rgba(255,255,255,80); border-radius: %5px; min-height: 20px; }"
        "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }"
        "QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical { background: transparent; }")
            .arg(static_cast<int>(22 * sx))
            .arg(static_cast<int>(23 * sx))
            .arg(static_cast<int>(46 * sx))
            .arg(static_cast<int>(8 * sx))
            .arg(static_cast<int>(4 * sx)));
    m_content->setObjectName(QStringLiteral("savedata_list_root"));

    m_content_layout = new QVBoxLayout(m_content);
    m_content_layout->setContentsMargins(0, 0, 0, 0);
    m_content_layout->setSpacing(0);

    auto *top_bar = new QWidget(m_content);
    top_bar->setFixedHeight(static_cast<int>(92 * sy));
    top_bar->setAttribute(Qt::WA_TranslucentBackground);
    auto *top_layout = new QHBoxLayout(top_bar);
    top_layout->setContentsMargins(static_cast<int>(20 * sx), static_cast<int>(20 * sy),
        static_cast<int>(20 * sx), static_cast<int>(8 * sy));

    auto *cancel_btn = new QPushButton(QStringLiteral("X"), top_bar);
    cancel_btn->setObjectName(QStringLiteral("cancel_x"));
    connect(cancel_btn, &QPushButton::clicked, this, &CommonDialogOverlay::on_savedata_cancel);
    top_layout->addWidget(cancel_btn, 0, Qt::AlignVCenter);

    top_layout->addStretch();

    if (!sd.list_title.empty()) {
        auto *title_label = new QLabel(QString::fromStdString(sd.list_title), top_bar);
        title_label->setAlignment(Qt::AlignCenter);
        auto f = title_label->font();
        f.setPixelSize(static_cast<int>(22 * sx));
        f.setBold(true);
        title_label->setFont(f);
        top_layout->addWidget(title_label, 0, Qt::AlignVCenter);
        top_layout->addStretch();
    }

    auto *spacer_widget = new QWidget(top_bar);
    spacer_widget->setFixedWidth(static_cast<int>(46 * sx));
    top_layout->addWidget(spacer_widget);

    m_content_layout->addWidget(top_bar);

    auto *sep = new QFrame(m_content);
    sep->setFrameShape(QFrame::HLine);
    sep->setFixedHeight(1);
    sep->setStyleSheet(QStringLiteral("background: rgba(255,255,255,60); border: none;"));
    m_content_layout->addWidget(sep);

    m_save_scroll = new QScrollArea(m_content);
    m_save_scroll->setWidgetResizable(true);
    m_save_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_save_scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_save_scroll->setFrameShape(QFrame::NoFrame);
    m_save_scroll->viewport()->setAutoFillBackground(false);
    m_save_scroll->viewport()->setStyleSheet(QStringLiteral("background: transparent;"));

    auto *scroll_content = new QWidget();
    scroll_content->setAutoFillBackground(false);
    scroll_content->setStyleSheet(QStringLiteral("background: transparent;"));
    auto *scroll_layout = new QVBoxLayout(scroll_content);
    scroll_layout->setContentsMargins(0, 0, 0, 0);
    scroll_layout->setSpacing(0);

    bool has_any_save = false;
    const int band_left_margin = static_cast<int>(150 * sx);

    for (uint32_t i = 0; i < sd.slot_list_size; i++) {
        if (sd.title.size() <= i || sd.title[i].empty())
            continue;

        has_any_save = true;

        auto *band = new QFrame(scroll_content);
        band->setAutoFillBackground(true);
        {
            QPalette pal = band->palette();
            pal.setColor(QPalette::Window, Qt::transparent);
            band->setPalette(pal);
        }
        band->setStyleSheet(QStringLiteral("QFrame#save_band { background: transparent; }"
                                           "QFrame#save_band:hover { background: rgba(255,255,255,30); }"));
        band->setObjectName(QStringLiteral("save_band"));
        band->setFrameShape(QFrame::NoFrame);
        band->setCursor(Qt::PointingHandCursor);
        band->setFixedHeight(thumb_h);

        auto *band_layout = new QHBoxLayout(band);
        band_layout->setContentsMargins(band_left_margin, 0,
            static_cast<int>(30 * sx), 0);
        band_layout->setSpacing(static_cast<int>(20 * sx));

        auto *icon_label = new QLabel(band);
        icon_label->setFixedSize(thumb_w, thumb_h);
        icon_label->setAlignment(Qt::AlignCenter);
        const QPixmap pix = load_icon_pixmap(i);
        if (!pix.isNull()) {
            icon_label->setPixmap(pix.scaled(thumb_w, thumb_h, Qt::KeepAspectRatio, Qt::SmoothTransformation));
        } else {
            icon_label->setStyleSheet(QStringLiteral("background: rgba(60,60,60,180); border-radius: 4px;"));
        }
        band_layout->addWidget(icon_label, 0, Qt::AlignVCenter);

        auto *text_col = new QVBoxLayout;
        text_col->setSpacing(static_cast<int>(2 * sy));

        auto *title_label = new QLabel(QString::fromStdString(sd.title[i]), band);
        auto tf = title_label->font();
        tf.setPixelSize(static_cast<int>(18 * sx));
        tf.setBold(true);
        title_label->setFont(tf);

        auto make_detail_label = [&](const QString &text) -> QLabel * {
            auto *lbl = new QLabel(text, band);
            auto df = lbl->font();
            df.setPixelSize(static_cast<int>(14 * sx));
            lbl->setFont(df);
            lbl->setStyleSheet(QStringLiteral("color: rgba(255,255,255,180);"));
            return lbl;
        };

        const bool is_save_exist = (i < sd.slot_info.size() && sd.slot_info[i].isExist == 1);
        const bool show_date = (sd.has_date.size() > i && sd.has_date[i] && sd.date.size() > i);
        const bool show_subtitle = (sd.subtitle.size() > i && !sd.subtitle[i].empty());

        text_col->addStretch();
        text_col->addWidget(title_label);
        if (is_save_exist) {
            switch (sd.list_style) {
            case SCE_SAVEDATA_DIALOG_LIST_ITEM_STYLE_TITLE_DATE_SUBTITLE:
                if (show_date)
                    text_col->addWidget(make_detail_label(
                        get_save_date_time(m_emuenv.cfg.sys_date_format, sd.date[i])));
                if (show_subtitle)
                    text_col->addWidget(make_detail_label(QString::fromStdString(sd.subtitle[i])));
                break;
            case SCE_SAVEDATA_DIALOG_LIST_ITEM_STYLE_TITLE_SUBTITLE_DATE:
                if (show_subtitle)
                    text_col->addWidget(make_detail_label(QString::fromStdString(sd.subtitle[i])));
                if (show_date)
                    text_col->addWidget(make_detail_label(
                        get_save_date_time(m_emuenv.cfg.sys_date_format, sd.date[i])));
                break;
            case SCE_SAVEDATA_DIALOG_LIST_ITEM_STYLE_TITLE_DATE:
                if (show_date)
                    text_col->addWidget(make_detail_label(
                        get_save_date_time(m_emuenv.cfg.sys_date_format, sd.date[i])));
                break;
            default:
                if (show_date)
                    text_col->addWidget(make_detail_label(
                        get_save_date_time(m_emuenv.cfg.sys_date_format, sd.date[i])));
                if (show_subtitle)
                    text_col->addWidget(make_detail_label(QString::fromStdString(sd.subtitle[i])));
                break;
            }
        }
        text_col->addStretch();

        band_layout->addLayout(text_col, 1);

        const uint32_t slot_idx = i;
        band->installEventFilter(this);
        band->setProperty("slot_index", slot_idx);

        scroll_layout->addWidget(band);

        auto *band_sep = new QFrame(scroll_content);
        band_sep->setFixedHeight(1);
        band_sep->setStyleSheet(QStringLiteral("background: rgba(255,255,255,40); border: none; margin-left: %1px;")
                .arg(band_left_margin));
        scroll_layout->addWidget(band_sep);
    }

    if (!has_any_save) {
        auto *empty_label = new QLabel(tr("There is no saved data."), scroll_content);
        empty_label->setAlignment(Qt::AlignCenter);
        auto f = empty_label->font();
        f.setPixelSize(static_cast<int>(22 * sx));
        empty_label->setFont(f);
        scroll_layout->addSpacing(static_cast<int>(150 * sy));
        scroll_layout->addWidget(empty_label);
    }

    scroll_layout->addStretch();
    m_save_scroll->setWidget(scroll_content);
    m_content_layout->addWidget(m_save_scroll, 1);
}

void CommonDialogOverlay::build_savedata_fixed_mode() {
    auto &dialog = m_emuenv.common_dialog;
    auto &sd = dialog.savedata;

    m_content_fullscreen = false;

    const qreal sx = scale_x();
    const qreal sy = scale_y();
    const int thumb_w = static_cast<int>(160 * sx);
    const int thumb_h = static_cast<int>(90 * sy);

    m_content = new QWidget(this);
    m_content->setStyleSheet(QStringLiteral(
        "QWidget#dialog_card { background: rgb(37, 40, 45); border-radius: %1px; }"
        "QLabel { color: white; background: transparent; }"
        "QPushButton { min-height: %2px; min-width: %3px; border-radius: %4px;"
        "  background: rgba(255,255,255,20); color: white; font-weight: 600;"
        "  padding: 4px 16px; font-size: %5px; }"
        "QPushButton:hover { background: rgba(255,255,255,40); }"
        "QProgressBar { border: 1px solid rgba(255,255,255,40); border-radius: %6px; background: rgba(82,82,91,30); }"
        "QProgressBar::chunk { background: rgb(141,180,83); border-radius: %7px; }")
            .arg(static_cast<int>(12 * sx))
            .arg(static_cast<int>(46 * sy))
            .arg(static_cast<int>(160 * sx))
            .arg(static_cast<int>(12 * sx))
            .arg(static_cast<int>(14 * sx))
            .arg(static_cast<int>(6 * sx))
            .arg(static_cast<int>(5 * sx)));
    m_content->setObjectName(QStringLiteral("dialog_card"));
    m_content->setFixedSize(static_cast<int>(760 * sx), static_cast<int>(440 * sy));

    m_content_layout = new QVBoxLayout(m_content);
    m_content_layout->setContentsMargins(
        static_cast<int>(20 * sx), static_cast<int>(20 * sy),
        static_cast<int>(20 * sx), static_cast<int>(20 * sy));
    m_content_layout->setSpacing(static_cast<int>(8 * sy));

    const auto sel = sd.selected_save;

    auto *top_section = new QHBoxLayout;
    top_section->setSpacing(static_cast<int>(20 * sx));
    top_section->setContentsMargins(static_cast<int>(28 * sx), static_cast<int>(14 * sy), 0, 0);

    auto *icon_label = new QLabel(m_content);
    icon_label->setFixedSize(thumb_w, thumb_h);
    icon_label->setAlignment(Qt::AlignCenter);
    const QPixmap pix = load_icon_pixmap(sel);
    if (!pix.isNull()) {
        icon_label->setPixmap(pix.scaled(thumb_w, thumb_h, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    } else {
        icon_label->setStyleSheet(QStringLiteral("background: rgba(60,60,60,180); border-radius: 4px;"));
    }
    top_section->addWidget(icon_label, 0, Qt::AlignTop);

    auto *text_col = new QVBoxLayout;
    text_col->setSpacing(static_cast<int>(2 * sy));

    const bool is_save_exist = (sel < sd.slot_info.size() && sd.slot_info[sel].isExist == 1);

    if (sd.title.size() > sel && !sd.title[sel].empty()) {
        auto *title_label = new QLabel(QString::fromStdString(sd.title[sel]), m_content);
        auto f = title_label->font();
        f.setPixelSize(static_cast<int>(18 * sx));
        f.setBold(true);
        title_label->setFont(f);
        text_col->addWidget(title_label);
    }

    if (is_save_exist) {
        if (sd.has_date.size() > sel && sd.has_date[sel] && sd.date.size() > sel) {
            auto *date_label = new QLabel(
                get_save_date_time(m_emuenv.cfg.sys_date_format, sd.date[sel]), m_content);
            auto f = date_label->font();
            f.setPixelSize(static_cast<int>(14 * sx));
            date_label->setFont(f);
            date_label->setStyleSheet(QStringLiteral("color: rgba(255,255,255,180);"));
            text_col->addWidget(date_label);
        }

        if (sd.subtitle.size() > sel && !sd.subtitle[sel].empty()) {
            auto *sub_label = new QLabel(QString::fromStdString(sd.subtitle[sel]), m_content);
            auto f = sub_label->font();
            f.setPixelSize(static_cast<int>(14 * sx));
            sub_label->setFont(f);
            sub_label->setStyleSheet(QStringLiteral("color: rgba(255,255,255,180);"));
            text_col->addWidget(sub_label);
        }
    }

    text_col->addStretch();
    top_section->addLayout(text_col, 1);
    m_content_layout->addLayout(top_section);

    auto *sep = new QFrame(m_content);
    sep->setFrameShape(QFrame::HLine);
    sep->setFixedHeight(1);
    sep->setStyleSheet(QStringLiteral("background: rgba(255,255,255,40); border: none;"));
    m_content_layout->addWidget(sep);

    m_content_layout->addStretch();
    if (!sd.msg.empty()) {
        m_message_label = new QLabel(QString::fromStdString(sd.msg), m_content);
        m_message_label->setAlignment(Qt::AlignCenter);
        m_message_label->setWordWrap(true);
        auto f = m_message_label->font();
        f.setPixelSize(static_cast<int>(18 * sx));
        m_message_label->setFont(f);
        m_content_layout->addWidget(m_message_label);
    }

    if (sd.has_progress_bar) {
        m_progress_bar = new QProgressBar(m_content);
        m_progress_bar->setRange(0, 100);
        m_progress_bar->setValue(static_cast<int>(sd.bar_percent));
        m_progress_bar->setTextVisible(false);
        m_progress_bar->setFixedHeight(static_cast<int>(12 * sy));
        m_content_layout->addWidget(m_progress_bar);

        m_progress_label = new QLabel(QStringLiteral("%1%").arg(sd.bar_percent), m_content);
        m_progress_label->setAlignment(Qt::AlignCenter);
        auto f = m_progress_label->font();
        f.setPixelSize(static_cast<int>(14 * sx));
        m_progress_label->setFont(f);
        m_content_layout->addWidget(m_progress_label);
    }

    m_content_layout->addStretch();

    if (sd.btn_num > 0) {
        auto *btn_row = new QHBoxLayout;
        btn_row->addStretch();
        for (uint8_t i = 0; i < sd.btn_num; i++) {
            auto *btn = new QPushButton(QString::fromStdString(sd.btn[i]), m_content);
            btn->setFixedSize(static_cast<int>(320 * sx / (sd.btn_num > 1 ? 1 : 2)),
                static_cast<int>(46 * sy));
            const int idx = i;
            connect(btn, &QPushButton::clicked, this, [this, idx]() {
                on_savedata_button_clicked(idx);
            });
            btn_row->addWidget(btn);
            m_buttons.append(btn);
        }
        btn_row->addStretch();
        m_content_layout->addLayout(btn_row);
    }
}

void CommonDialogOverlay::update_savedata_progress() {
    auto &sd = m_emuenv.common_dialog.savedata;
    if (m_progress_bar) {
        m_progress_bar->setValue(static_cast<int>(sd.bar_percent));
    }
    if (m_progress_label) {
        m_progress_label->setText(QStringLiteral("%1%").arg(sd.bar_percent));
    }
}

void CommonDialogOverlay::on_savedata_slot_clicked(uint32_t slot_index) {
    auto &dialog = m_emuenv.common_dialog;
    dialog.savedata.selected_save = slot_index;
    dialog.result = SCE_COMMON_DIALOG_RESULT_OK;
    dialog.substatus = SCE_COMMON_DIALOG_STATUS_FINISHED;
    dialog.savedata.mode_to_display = SCE_SAVEDATA_DIALOG_MODE_FIXED;
}

void CommonDialogOverlay::on_savedata_button_clicked(int index) {
    auto &dialog = m_emuenv.common_dialog;
    dialog.savedata.button_id = dialog.savedata.btn_val[index];
    dialog.result = SCE_COMMON_DIALOG_RESULT_OK;
    dialog.substatus = SCE_COMMON_DIALOG_STATUS_FINISHED;
}

void CommonDialogOverlay::on_savedata_cancel() {
    auto &dialog = m_emuenv.common_dialog;
    dialog.savedata.button_id = SCE_SAVEDATA_DIALOG_BUTTON_ID_INVALID;
    dialog.result = SCE_COMMON_DIALOG_RESULT_USER_CANCELED;
    dialog.substatus = SCE_COMMON_DIALOG_STATUS_FINISHED;
}
