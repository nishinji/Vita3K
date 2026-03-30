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

#include "ui_settings_dialog.h"

#include <app/state.h>
#include <audio/state.h>
#include <compat/state.h>
#include <config/functions.h>
#include <config/settings.h>
#include <emuenv/state.h>
#include <gui-qt/gui_settings.h>
#include <gui-qt/settings_dialog.h>
#include <kernel/state.h>
#include <renderer/functions.h>
#include <renderer/state.h>
#include <util/log.h>
#include <util/net_utils.h>
#include <util/system.h>

#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QRadioButton>
#include <QSlider>
#include <QSpinBox>

#include <SDL3/SDL_camera.h>

namespace desc {
// Core
static const QString modules_automatic = QT_TR_NOOP(
    "Automatically select modules to load. Recommended for most games.");
static const QString modules_auto_manual = QT_TR_NOOP(
    "Auto-load modules and additionally load selected modules from the list.");
static const QString modules_manual = QT_TR_NOOP(
    "Only load the modules selected from the list. Advanced users only.");

// CPU
static const QString cpu_opt = QT_TR_NOOP(
    "Enable Dynarmic CPU optimizations.\nDisabling may improve compatibility at the cost of performance.");

// GPU
static const QString backend_renderer = QT_TR_NOOP(
    "Select the backend renderer.\nVulkan is recommended for most systems.");
static const QString renderer_accuracy = QT_TR_NOOP(
    "Set the renderer accuracy level. High accuracy may improve visuals but reduce performance.");
static const QString vsync = QT_TR_NOOP(
    "Enable V-Sync for OpenGL. Reduces tearing at the cost of potential input lag.");
static const QString disable_surface_sync = QT_TR_NOOP(
    "Disable surface synchronization.\nMay improve performance but can cause visual glitches.");
static const QString async_pipeline = QT_TR_NOOP(
    "Enable asynchronous pipeline compilation.\nReduces stuttering at the cost of potential visual artifacts.");
static const QString screen_filter = QT_TR_NOOP(
    "Select the screen filter used to display the rendered image.");
static const QString resolution_upscaling = QT_TR_NOOP(
    "Set the internal rendering resolution multiplier.\nHigher values improve visual quality but require more GPU power.");
static const QString anisotropic_filtering = QT_TR_NOOP(
    "Set the anisotropic filtering level.\nImproves texture quality at oblique viewing angles.");
static const QString export_textures = QT_TR_NOOP(
    "Export textures used by the game to the textures folder.");
static const QString import_textures = QT_TR_NOOP(
    "Import replacement textures from the textures folder.");
static const QString shader_cache = QT_TR_NOOP(
    "Enable shader caching. Reduces stuttering on subsequent runs.");
static const QString spirv_shader = QT_TR_NOOP(
    "Use SPIR-V shaders with Vulkan renderer.");
static const QString fps_hack = QT_TR_NOOP(
    "Enable FPS hack. May unlock or double the framerate in some games,\nbut can cause speed issues in others.");
static const QString memory_mapping = QT_TR_NOOP(
    "Select the Vita memory mapping method used by the Vulkan renderer.\nHigher methods improve accuracy but require more GPU features.");

// Audio
static const QString audio_backend = QT_TR_NOOP(
    "Select the audio backend.\nSDL is the default; Cubeb offers lower latency on some systems.");
static const QString audio_volume = QT_TR_NOOP(
    "Set the in-game audio volume.");
static const QString ngs_enable = QT_TR_NOOP(
    "Enable NGS (Next Generation Sound) support.\nRequired by many games for proper audio.");

// Camera
static const QString front_camera = QT_TR_NOOP(
    "Select the front camera source.\nSolid Color or Static Image can be used as substitutes.");
static const QString back_camera = QT_TR_NOOP(
    "Select the back camera source.\nSolid Color or Static Image can be used as substitutes.");

// System
static const QString enter_button = QT_TR_NOOP(
    "Select which button acts as the Enter/Confirm button.");
static const QString pstv_mode = QT_TR_NOOP(
    "Enable PlayStation TV mode.\nSome games may behave differently or become playable.");
static const QString show_mode = QT_TR_NOOP(
    "Enable Show Mode. Cycles through the home screen automatically.");
static const QString demo_mode = QT_TR_NOOP(
    "Enable Demo Mode.");

// Emulator
static const QString boot_fullscreen = QT_TR_NOOP(
    "Automatically enter fullscreen when booting a game.");
static const QString show_compile_shaders = QT_TR_NOOP(
    "Show a hint when shaders are being compiled during gameplay.");
static const QString show_touchpad_cursor = QT_TR_NOOP(
    "Show the touchpad cursor overlay during gameplay.");
static const QString check_for_updates = QT_TR_NOOP(
    "Check for Vita3K updates when the emulator starts.");
static const QString log_compat_warn = QT_TR_NOOP(
    "Log compatibility-related warnings for debugging.");
static const QString texture_cache = QT_TR_NOOP(
    "Enable the texture cache. Improves performance in most games.");
static const QString archive_log = QT_TR_NOOP(
    "Save old log files to an archive instead of overwriting them.");
static const QString discord_rpc = QT_TR_NOOP(
    "Enable Discord Rich Presence to show current game status.");
static const QString perf_overlay = QT_TR_NOOP(
    "Show a performance overlay with FPS and other statistics.");
static const QString file_loading_delay = QT_TR_NOOP(
    "Add an artificial delay to file loading. May fix timing issues in some games.");
static const QString stretch_display = QT_TR_NOOP(
    "Stretch the display area to fill the entire window, ignoring the original aspect ratio.");
static const QString pixel_perfect = QT_TR_NOOP(
    "Use pixel-perfect integer scaling in fullscreen mode when the resolution is a multiple of the Vita native resolution.");

// Network
static const QString psn_signed_in = QT_TR_NOOP(
    "Pretend to be signed in to PlayStation Network.\nRequired by some games to function.");
static const QString http_enable = QT_TR_NOOP(
    "Enable HTTP networking support for games that use it.");
static const QString adhoc_address = QT_TR_NOOP(
    "Select the network interface to use for ad-hoc multiplayer.");

// Debug
static const QString log_imports = QT_TR_NOOP(
    "Log all HLE import calls.");
static const QString log_exports = QT_TR_NOOP(
    "Log all HLE export calls.");
static const QString log_active_shaders = QT_TR_NOOP(
    "Log active shader programs.");
static const QString log_uniforms = QT_TR_NOOP(
    "Log shader uniform values.");
static const QString color_surface_debug = QT_TR_NOOP(
    "Colorize surfaces for debugging rendering issues.");
static const QString validation_layer = QT_TR_NOOP(
    "Enable Vulkan validation layers.\nUseful for debugging but reduces performance.");
static const QString dump_elfs = QT_TR_NOOP(
    "Dump loaded ELF files to disk for analysis.");

// System
static const QString sys_lang = QT_TR_NOOP(
    "Set the system language reported to applications.");
static const QString sys_date_format = QT_TR_NOOP(
    "Set the date format used by the emulated system.");
static const QString sys_time_format = QT_TR_NOOP(
    "Set the time format used by the emulated system.");
static const QString ime_langs = QT_TR_NOOP(
    "Select which IME keyboard languages are available to applications.");

// GUI
static const QString show_welcome_screen = QT_TR_NOOP(
    "Show the Welcome Dialog at startup.");
static const QString log_buffer_size = QT_TR_NOOP(
    "Set the maximum number of log lines to keep in memory.\nSet to 0 for infinite (no limit).");
} // namespace desc

SettingsDialog::SettingsDialog(EmuEnvState &emuenv,
    std::shared_ptr<GuiSettings> gui_settings,
    const std::string &app_path,
    int initial_tab,
    QWidget *parent)
    : QDialog(parent)
    , emuenv(emuenv)
    , m_gui_settings(std::move(gui_settings))
    , m_app_path(app_path)
    , m_initial_tab(initial_tab)
    , m_ui(std::make_unique<Ui::vita3k_settings_dialog>()) {
    m_ui->setupUi(this);

    setStyleSheet(QStringLiteral(
        "QGroupBox { font-weight: bold; border: 1px solid gray; border-radius: 4px; margin-top: 0.5em; padding-top: 0.4em; }"
        "QGroupBox::title { subcontrol-origin: margin; subcontrol-position: top center; padding: 0 4px; }"));

    for (auto *gb : { m_ui->gb_description_core, m_ui->gb_description_cpu,
             m_ui->gb_description_gpu, m_ui->gb_description_audio,
             m_ui->gb_description_camera,
             m_ui->gb_description_system, m_ui->gb_description_emulator,
             m_ui->gb_description_gui,
             m_ui->gb_description_network, m_ui->gb_description_debug }) {
        gb->setMinimumHeight(60);
    }

    m_button_boxes = {
        m_ui->button_box_core,
        m_ui->button_box_cpu,
        m_ui->button_box_gpu,
        m_ui->button_box_audio,
        m_ui->button_box_camera,
        m_ui->button_box_system,
        m_ui->button_box_emulator,
        m_ui->button_box_gui,
        m_ui->button_box_network,
        m_ui->button_box_debug,
    };

    if (!m_app_path.empty()) {
        QString title_name = QString::fromStdString(m_app_path);
        for (const auto &game : emuenv.app.game_list.games) {
            if (game.title_id == m_app_path) {
                title_name = QString::fromStdString(game.title);
                break;
            }
        }
        setWindowTitle(tr("Settings - %1").arg(title_name));
    }

    m_ui->tab_widget_settings->setCurrentIndex(m_initial_tab);

    load_config();
    setup_connections();
    update_gpu_visibility();
    update_camera_widgets();
    setup_dirty_tracking();

    if (!m_app_path.empty()) {
        auto *tabs = m_ui->tab_widget_settings;
        for (int i = tabs->count() - 1; i >= 0; --i) {
            QWidget *page = tabs->widget(i);
            if (page == m_ui->camera_tab
                || page == m_ui->gui_tab
                || page == m_ui->debug_tab) {
                tabs->setTabVisible(i, false);
            }
        }

        m_ui->gb_enter_button->setVisible(false);
        m_ui->show_mode->setVisible(false);
        m_ui->demo_mode->setVisible(false);
        m_ui->gb_system_settings->setVisible(false);

        m_ui->shader_cache->setVisible(false);
        m_ui->spirv_shader->setVisible(false);

        m_ui->boot_apps_fullscreen->setVisible(false);
        m_ui->show_compile_shaders->setVisible(false);
        m_ui->check_for_updates->setVisible(false);
        m_ui->log_compat_warn->setVisible(false);
        m_ui->texture_cache->setVisible(false);
        m_ui->archive_log->setVisible(false);
        m_ui->discord_rich_presence->setVisible(false);
        m_ui->gb_log_level->setVisible(false);
        m_ui->gb_performance_overlay->setVisible(false);
        m_ui->gb_storage->setVisible(false);
        m_ui->gb_screenshot->setVisible(false);

        m_ui->gb_http->setVisible(false);
        m_ui->gb_adhoc->setVisible(false);
    }

    for (auto *bb : m_button_boxes)
        bb->button(QDialogButtonBox::Apply)->setEnabled(false);
}

SettingsDialog::~SettingsDialog() = default;

void SettingsDialog::load_config() {
    if (!m_app_path.empty()) {
        if (!config::load_custom_config(m_config, emuenv.config_path, m_app_path)) {
            config::set_current_config(emuenv.cfg, emuenv.config_path, m_app_path);
            m_config = emuenv.cfg.current_config;
        }
    } else {
        config::set_current_config(emuenv.cfg, emuenv.config_path, {});
        m_config = emuenv.cfg.current_config;
    }

    switch (m_config.modules_mode) {
    case ModulesMode::AUTOMATIC:
        m_ui->rb_modules_automatic->setChecked(true);
        break;
    case ModulesMode::AUTO_MANUAL:
        m_ui->rb_modules_auto_manual->setChecked(true);
        break;
    case ModulesMode::MANUAL:
        m_ui->rb_modules_manual->setChecked(true);
        break;
    }
    populate_modules_list();

    m_ui->cpu_opt->setChecked(m_config.cpu_opt);

    m_ui->backend_renderer_box->clear();
#ifndef __APPLE__
    m_ui->backend_renderer_box->addItem(QStringLiteral("OpenGL"));
#endif
    m_ui->backend_renderer_box->addItem(QStringLiteral("Vulkan"));
    {
        const int idx = m_ui->backend_renderer_box->findText(
            QString::fromStdString(m_config.backend_renderer));
        if (idx >= 0)
            m_ui->backend_renderer_box->setCurrentIndex(idx);
    }

    m_ui->gpu_device_box->clear();
    if (emuenv.renderer) {
        const auto gpu_list = emuenv.renderer->get_gpu_list();
        for (const auto &gpu : gpu_list)
            m_ui->gpu_device_box->addItem(QString::fromStdString(gpu));
    } else {
        const auto gpu_list = renderer::enumerate_vulkan_gpu_names();
        for (const auto &gpu : gpu_list)
            m_ui->gpu_device_box->addItem(QString::fromStdString(gpu));
    }

    {
        const int gpu_count = m_ui->gpu_device_box->count();
        const int idx = (emuenv.cfg.gpu_idx >= 0 && emuenv.cfg.gpu_idx < gpu_count)
            ? emuenv.cfg.gpu_idx
            : 0;
        m_ui->gpu_device_box->setCurrentIndex(idx);
    }

    m_ui->renderer_accuracy_box->clear();
    m_ui->renderer_accuracy_box->addItem(tr("Standard"));
    m_ui->renderer_accuracy_box->addItem(tr("High"));
    m_ui->renderer_accuracy_box->setCurrentIndex(m_config.high_accuracy ? 1 : 0);

    m_ui->vsync->setChecked(m_config.v_sync);
    m_ui->disable_surface_sync->setChecked(m_config.disable_surface_sync);
    m_ui->async_pipeline_compilation->setChecked(m_config.async_pipeline_compilation);

    {
        m_ui->memory_mapping_box->clear();

        struct MappingEntry {
            const char *label;
            const char *value;
            int bit;
        };
        static const MappingEntry all_methods[] = {
            { "Disabled", "disabled", 0 },
            { "Double Buffer", "double-buffer", 1 },
            { "External Host", "external-host", 2 },
            { "Page Table", "page-table", 3 },
            { "Native Buffer", "native-buffer", 4 },
        };

        const int mask = emuenv.renderer
            ? emuenv.renderer->supported_mapping_methods_mask
            : renderer::enumerate_vulkan_mapping_methods(emuenv.cfg.gpu_idx);

        int current_idx = 0;
        int added = 0;
        for (const auto &m : all_methods) {
            if ((1 << m.bit) & mask) {
                m_ui->memory_mapping_box->addItem(tr(m.label), QString::fromLatin1(m.value));
                if (m_config.memory_mapping == m.value)
                    current_idx = added;
                added++;
            }
        }
        m_ui->memory_mapping_box->setCurrentIndex(current_idx);
    }

    m_ui->resolution_upscale->setMinimum(2);
    m_ui->resolution_upscale->setMaximum(32);
    m_ui->resolution_upscale->setSingleStep(1);
    m_ui->resolution_upscale->setValue(static_cast<int>(m_config.resolution_multiplier * 4.0f));
    update_resolution_label();

    m_ui->anisotropic_filter->setMinimum(0);
    m_ui->anisotropic_filter->setMaximum(4);
    m_ui->anisotropic_filter->setSingleStep(1);
    m_ui->anisotropic_filter->setValue(
        static_cast<int>(log2f(static_cast<float>(m_config.anisotropic_filtering))));
    update_aniso_label();

    m_ui->export_textures->setChecked(m_config.export_textures);
    m_ui->import_textures->setChecked(m_config.import_textures);

    m_ui->texture_export_format->clear();
    m_ui->texture_export_format->addItems({ QStringLiteral("PNG"), QStringLiteral("DDS") });
    m_ui->texture_export_format->setCurrentIndex(m_config.export_as_png ? 0 : 1);

    m_ui->shader_cache->setChecked(emuenv.cfg.shader_cache);
    m_ui->spirv_shader->setChecked(emuenv.cfg.spirv_shader);
    m_ui->fps_hack->setChecked(m_config.fps_hack);

    m_ui->audio_backend_box->clear();
    m_ui->audio_backend_box->addItems({ QStringLiteral("SDL"), QStringLiteral("Cubeb") });
    m_ui->audio_backend_box->setCurrentIndex(m_config.audio_backend == "Cubeb" ? 1 : 0);

    m_ui->audio_volume->setValue(m_config.audio_volume);
    m_ui->audio_volume_label->setText(tr("Audio Volume: %1%").arg(m_config.audio_volume));

    m_ui->ngs_enable->setChecked(m_config.ngs_enable);

    populate_cameras_list();

    if (emuenv.cfg.sys_button == 0)
        m_ui->enter_button_circle->setChecked(true);
    else
        m_ui->enter_button_cross->setChecked(true);

    m_ui->pstv_mode->setChecked(m_config.pstv_mode);
    m_ui->show_mode->setChecked(emuenv.cfg.show_mode);
    m_ui->demo_mode->setChecked(emuenv.cfg.demo_mode);

    m_ui->boot_apps_fullscreen->setChecked(emuenv.cfg.boot_apps_full_screen);
    m_ui->show_compile_shaders->setChecked(emuenv.cfg.show_compile_shaders);
    m_ui->check_for_updates->setChecked(emuenv.cfg.check_for_updates);
    m_ui->log_compat_warn->setChecked(emuenv.cfg.log_compat_warn);
    m_ui->texture_cache->setChecked(emuenv.cfg.texture_cache);
    m_ui->archive_log->setChecked(emuenv.cfg.archive_log);
#ifdef USE_DISCORD
    m_ui->discord_rich_presence->setChecked(emuenv.cfg.discord_rich_presence);
#else
    m_ui->discord_rich_presence->setVisible(false);
#endif

    m_ui->log_level_box->clear();
    m_ui->log_level_box->addItems({ tr("Trace"), tr("Debug"), tr("Info"),
        tr("Warning"), tr("Error"), tr("Critical"), tr("Off") });
    m_ui->log_level_box->setCurrentIndex(emuenv.cfg.log_level);

    m_ui->perf_overlay_enabled->setChecked(emuenv.cfg.performance_overlay);
    m_ui->perf_overlay_detail_box->clear();
    m_ui->perf_overlay_detail_box->addItems({ tr("Minimum"), tr("Low"), tr("Medium"), tr("Maximum") });
    m_ui->perf_overlay_detail_box->setCurrentIndex(emuenv.cfg.performance_overlay_detail);
    m_ui->perf_overlay_position_box->clear();
    m_ui->perf_overlay_position_box->addItems({ tr("Top Left"), tr("Top Center"), tr("Top Right"),
        tr("Bottom Left"), tr("Bottom Center"), tr("Bottom Right") });
    m_ui->perf_overlay_position_box->setCurrentIndex(emuenv.cfg.performance_overlay_position);

    m_ui->perf_overlay_detail->setEnabled(emuenv.cfg.performance_overlay);
    m_ui->perf_overlay_position->setEnabled(emuenv.cfg.performance_overlay);

    m_ui->current_emu_path->setText(
        tr("Current path: %1").arg(QString::fromStdString(emuenv.cfg.pref_path)));

    m_ui->screenshot_format->clear();
    m_ui->screenshot_format->addItems({ tr("None"), QStringLiteral("JPEG"), QStringLiteral("PNG") });
    m_ui->screenshot_format->setCurrentIndex(emuenv.cfg.screenshot_format);

    m_ui->stretch_the_display_area->setChecked(m_config.stretch_the_display_area);
    m_ui->fullscreen_hd_res_pixel_perfect->setChecked(m_config.fullscreen_hd_res_pixel_perfect);

    m_ui->file_loading_delay->setMinimum(0);
    m_ui->file_loading_delay->setMaximum(30);
    m_ui->file_loading_delay->setValue(m_config.file_loading_delay);
    update_file_loading_delay_label();

    m_ui->sys_lang_box->clear();
    m_ui->sys_lang_box->addItems({ tr("Japanese"), tr("English (US)"), tr("French"), tr("Spanish"),
        tr("German"), tr("Italian"), tr("Dutch"), tr("Portuguese (PT)"),
        tr("Russian"), tr("Korean"), tr("Chinese (Traditional)"),
        tr("Chinese (Simplified)"), tr("Finnish"), tr("Swedish"),
        tr("Danish"), tr("Norwegian"), tr("Polish"), tr("Portuguese (BR)"),
        tr("English (GB)"), tr("Turkish") });
    m_ui->sys_lang_box->setCurrentIndex(emuenv.cfg.sys_lang);

    m_ui->sys_date_format_box->clear();
    m_ui->sys_date_format_box->addItems({ tr("YYYY/MM/DD"), tr("DD/MM/YYYY"), tr("MM/DD/YYYY") });
    m_ui->sys_date_format_box->setCurrentIndex(emuenv.cfg.sys_date_format);

    m_ui->sys_time_format_box->clear();
    m_ui->sys_time_format_box->addItems({ tr("12-Hour"), tr("24-Hour") });
    m_ui->sys_time_format_box->setCurrentIndex(emuenv.cfg.sys_time_format);

    {
        struct ImeLang {
            const char *name;
            uint64_t flag;
        };
        static const ImeLang ime_lang_list[] = {
            { "Danish", 0x00000001ULL },
            { "German", 0x00000002ULL },
            { "English (US)", 0x00000004ULL },
            { "Spanish", 0x00000008ULL },
            { "French", 0x00000010ULL },
            { "Italian", 0x00000020ULL },
            { "Dutch", 0x00000040ULL },
            { "Norwegian", 0x00000080ULL },
            { "Polish", 0x00000100ULL },
            { "Portuguese (PT)", 0x00000200ULL },
            { "Russian", 0x00000400ULL },
            { "Finnish", 0x00000800ULL },
            { "Swedish", 0x00001000ULL },
            { "Japanese", 0x00002000ULL },
            { "Korean", 0x00004000ULL },
            { "Simplified Chinese", 0x00008000ULL },
            { "Traditional Chinese", 0x00010000ULL },
            { "Portuguese (BR)", 0x00020000ULL },
            { "English (GB)", 0x00040000ULL },
            { "Turkish", 0x00080000ULL },
        };
        m_ui->ime_langs_list->clear();

        uint64_t enabled_mask = 0;
        for (const auto &v : emuenv.cfg.ime_langs)
            enabled_mask |= v;
        for (const auto &lang : ime_lang_list) {
            auto *item = new QListWidgetItem(tr(lang.name), m_ui->ime_langs_list);
            item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
            item->setCheckState((enabled_mask & lang.flag) ? Qt::Checked : Qt::Unchecked);
            item->setData(Qt::UserRole, QVariant::fromValue(lang.flag));
        }
    }

    m_ui->psn_signed_in->setChecked(m_config.psn_signed_in);
    m_ui->http_enable->setChecked(emuenv.cfg.http_enable);
    m_ui->http_timeout_attempts->setValue(emuenv.cfg.http_timeout_attempts);
    m_ui->http_timeout_sleep->setValue(emuenv.cfg.http_timeout_sleep_ms);
    m_ui->http_read_end_attempts->setValue(emuenv.cfg.http_read_end_attempts);
    m_ui->http_read_end_sleep->setValue(emuenv.cfg.http_read_end_sleep_ms);
    populate_adhoc_list();

    m_ui->show_welcome->setChecked(emuenv.cfg.show_welcome);
    if (m_gui_settings) {
        const int buf_size = m_gui_settings->get_value(gui::l_bufferSize).toInt();
        m_ui->log_buffer_size->setValue(buf_size);
    }

    m_ui->log_imports->setChecked(emuenv.kernel.debugger.log_imports);
    m_ui->log_exports->setChecked(emuenv.kernel.debugger.log_exports);
    m_ui->log_active_shaders->setChecked(emuenv.cfg.log_active_shaders);
    m_ui->log_uniforms->setChecked(emuenv.cfg.log_uniforms);
    m_ui->color_surface_debug->setChecked(emuenv.cfg.color_surface_debug);
    m_ui->validation_layer->setChecked(emuenv.cfg.validation_layer);
    m_ui->dump_elfs->setChecked(emuenv.kernel.debugger.dump_elfs);
}

void SettingsDialog::save_config() {
    if (m_ui->rb_modules_automatic->isChecked())
        m_config.modules_mode = ModulesMode::AUTOMATIC;
    else if (m_ui->rb_modules_auto_manual->isChecked())
        m_config.modules_mode = ModulesMode::AUTO_MANUAL;
    else
        m_config.modules_mode = ModulesMode::MANUAL;

    m_config.lle_modules.clear();
    for (int i = 0; i < m_ui->modules_list->count(); ++i) {
        auto *item = m_ui->modules_list->item(i);
        if (item->checkState() == Qt::Checked)
            m_config.lle_modules.push_back(item->text().toStdString());
    }

    m_config.cpu_opt = m_ui->cpu_opt->isChecked();

    m_config.backend_renderer = m_ui->backend_renderer_box->currentText().toStdString();
    m_config.high_accuracy = m_ui->renderer_accuracy_box->currentIndex() == 1;
    m_config.v_sync = m_ui->vsync->isChecked();
    m_config.disable_surface_sync = m_ui->disable_surface_sync->isChecked();
    m_config.async_pipeline_compilation = m_ui->async_pipeline_compilation->isChecked();
    m_config.memory_mapping = m_ui->memory_mapping_box->currentData().toString().toStdString();
    m_config.screen_filter = m_ui->screen_filter_box->currentText().toStdString();
    m_config.resolution_multiplier = static_cast<float>(m_ui->resolution_upscale->value()) / 4.0f;
    m_config.anisotropic_filtering = 1 << m_ui->anisotropic_filter->value();
    m_config.export_textures = m_ui->export_textures->isChecked();
    m_config.import_textures = m_ui->import_textures->isChecked();
    m_config.export_as_png = m_ui->texture_export_format->currentIndex() == 0;
    m_config.fps_hack = m_ui->fps_hack->isChecked();

    emuenv.cfg.shader_cache = m_ui->shader_cache->isChecked();
    emuenv.cfg.spirv_shader = m_ui->spirv_shader->isChecked();
    emuenv.cfg.gpu_idx = m_ui->gpu_device_box->currentIndex();

    m_config.audio_backend = m_ui->audio_backend_box->currentText().toStdString();
    m_config.audio_volume = m_ui->audio_volume->value();
    m_config.ngs_enable = m_ui->ngs_enable->isChecked();

    {
        const int front_idx = m_ui->front_camera_box->currentIndex();
        if (front_idx == 0) {
            emuenv.cfg.front_camera_type = 0;
        } else if (front_idx == 1) {
            emuenv.cfg.front_camera_type = 1;
        } else {
            emuenv.cfg.front_camera_type = 2;
            emuenv.cfg.front_camera_id = m_ui->front_camera_box->currentText().toStdString();
        }

        const int back_idx = m_ui->back_camera_box->currentIndex();
        if (back_idx == 0) {
            emuenv.cfg.back_camera_type = 0;
        } else if (back_idx == 1) {
            emuenv.cfg.back_camera_type = 1;
        } else {
            emuenv.cfg.back_camera_type = 2;
            emuenv.cfg.back_camera_id = m_ui->back_camera_box->currentText().toStdString();
        }
    }

    emuenv.cfg.sys_button = m_ui->enter_button_cross->isChecked() ? 1 : 0;
    m_config.pstv_mode = m_ui->pstv_mode->isChecked();
    emuenv.cfg.show_mode = m_ui->show_mode->isChecked();
    emuenv.cfg.demo_mode = m_ui->demo_mode->isChecked();

    emuenv.cfg.boot_apps_full_screen = m_ui->boot_apps_fullscreen->isChecked();
    emuenv.cfg.show_compile_shaders = m_ui->show_compile_shaders->isChecked();
    emuenv.cfg.check_for_updates = m_ui->check_for_updates->isChecked();
    emuenv.cfg.log_compat_warn = m_ui->log_compat_warn->isChecked();
    emuenv.compat.log_compat_warn = emuenv.cfg.log_compat_warn;
    emuenv.cfg.texture_cache = m_ui->texture_cache->isChecked();
    emuenv.cfg.archive_log = m_ui->archive_log->isChecked();
#ifdef USE_DISCORD
    emuenv.cfg.discord_rich_presence = m_ui->discord_rich_presence->isChecked();
#endif

    const int new_log_level = m_ui->log_level_box->currentIndex();
    if (new_log_level != emuenv.cfg.log_level) {
        emuenv.cfg.log_level = new_log_level;
        logging::set_level(static_cast<spdlog::level::level_enum>(new_log_level));
    }

    emuenv.cfg.performance_overlay = m_ui->perf_overlay_enabled->isChecked();
    emuenv.cfg.performance_overlay_detail = m_ui->perf_overlay_detail_box->currentIndex();
    emuenv.cfg.performance_overlay_position = m_ui->perf_overlay_position_box->currentIndex();
    emuenv.cfg.screenshot_format = m_ui->screenshot_format->currentIndex();

    m_config.stretch_the_display_area = m_ui->stretch_the_display_area->isChecked();
    m_config.fullscreen_hd_res_pixel_perfect = m_ui->fullscreen_hd_res_pixel_perfect->isChecked();
    m_config.file_loading_delay = m_ui->file_loading_delay->value();

    emuenv.cfg.sys_lang = m_ui->sys_lang_box->currentIndex();
    emuenv.cfg.sys_date_format = m_ui->sys_date_format_box->currentIndex();
    emuenv.cfg.sys_time_format = m_ui->sys_time_format_box->currentIndex();

    {
        emuenv.cfg.ime_langs.clear();
        for (int i = 0; i < m_ui->ime_langs_list->count(); ++i) {
            auto *item = m_ui->ime_langs_list->item(i);
            if (item->checkState() == Qt::Checked) {
                const uint64_t flag = item->data(Qt::UserRole).toULongLong();
                emuenv.cfg.ime_langs.push_back(flag);
            }
        }
        if (emuenv.cfg.ime_langs.empty())
            emuenv.cfg.ime_langs.push_back(0x00000004ULL); // English (US)
    }

    m_config.psn_signed_in = m_ui->psn_signed_in->isChecked();
    emuenv.cfg.http_enable = m_ui->http_enable->isChecked();
    emuenv.cfg.http_timeout_attempts = m_ui->http_timeout_attempts->value();
    emuenv.cfg.http_timeout_sleep_ms = m_ui->http_timeout_sleep->value();
    emuenv.cfg.http_read_end_attempts = m_ui->http_read_end_attempts->value();
    emuenv.cfg.http_read_end_sleep_ms = m_ui->http_read_end_sleep->value();
    emuenv.cfg.adhoc_addr = m_ui->adhoc_address_box->currentIndex();

    emuenv.cfg.show_welcome = m_ui->show_welcome->isChecked();
    if (m_gui_settings) {
        m_gui_settings->set_value(gui::l_bufferSize, m_ui->log_buffer_size->value());
    }

    emuenv.kernel.debugger.log_imports = m_ui->log_imports->isChecked();
    emuenv.kernel.debugger.log_exports = m_ui->log_exports->isChecked();
    emuenv.cfg.log_active_shaders = m_ui->log_active_shaders->isChecked();
    emuenv.cfg.log_uniforms = m_ui->log_uniforms->isChecked();
    emuenv.cfg.color_surface_debug = m_ui->color_surface_debug->isChecked();
    emuenv.cfg.validation_layer = m_ui->validation_layer->isChecked();
    emuenv.kernel.debugger.dump_elfs = m_ui->dump_elfs->isChecked();

    emuenv.cfg.current_config = m_config;

    if (!m_app_path.empty()) {
        config::save_custom_config(m_config, emuenv.config_path, m_app_path);
    } else {
        config::copy_current_config_to_global(emuenv.cfg);
    }
    config::serialize_config(emuenv.cfg, emuenv.cfg.config_path);
}

void SettingsDialog::apply_config() {
    emuenv.cfg.current_config = m_config;

    emuenv.audio.set_global_volume(m_config.audio_volume / 100.f);
}

void SettingsDialog::populate_modules_list() {
    m_ui->modules_list->clear();
    const auto modules = config::get_modules_list(emuenv.pref_path, m_config.lle_modules);
    for (const auto &[name, enabled] : modules) {
        auto *item = new QListWidgetItem(QString::fromStdString(name), m_ui->modules_list);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(enabled ? Qt::Checked : Qt::Unchecked);
    }
    update_modules_list_enabled();
}

void SettingsDialog::update_modules_list_enabled() {
    const bool automatic = m_ui->rb_modules_automatic->isChecked();
    m_ui->modules_list->setEnabled(!automatic);
    m_ui->modules_search_bar->setEnabled(!automatic);
    m_ui->clear_modules_list->setEnabled(!automatic);
    m_ui->refresh_modules_list->setEnabled(!automatic);
}

void SettingsDialog::populate_cameras_list() {
    m_ui->front_camera_box->clear();
    m_ui->back_camera_box->clear();

    m_ui->front_camera_box->addItem(tr("Solid Color"));
    m_ui->front_camera_box->addItem(tr("Static Image"));
    m_ui->back_camera_box->addItem(tr("Solid Color"));
    m_ui->back_camera_box->addItem(tr("Static Image"));

    int cameras_count = 0;
    auto *sdl_cameras = SDL_GetCameras(&cameras_count);
    for (int i = 0; i < cameras_count; i++) {
        const char *cam_name = SDL_GetCameraName(sdl_cameras[i]);
        if (cam_name) {
            m_ui->front_camera_box->addItem(QString::fromUtf8(cam_name));
            m_ui->back_camera_box->addItem(QString::fromUtf8(cam_name));
        }
    }
    if (sdl_cameras)
        SDL_free(sdl_cameras);

    if (emuenv.cfg.front_camera_type == 0) {
        m_ui->front_camera_box->setCurrentIndex(0);
    } else if (emuenv.cfg.front_camera_type == 1) {
        m_ui->front_camera_box->setCurrentIndex(1);
    } else {
        const int idx = m_ui->front_camera_box->findText(
            QString::fromStdString(emuenv.cfg.front_camera_id));
        m_ui->front_camera_box->setCurrentIndex(idx >= 0 ? idx : 0);
    }

    if (emuenv.cfg.back_camera_type == 0) {
        m_ui->back_camera_box->setCurrentIndex(0);
    } else if (emuenv.cfg.back_camera_type == 1) {
        m_ui->back_camera_box->setCurrentIndex(1);
    } else {
        const int idx = m_ui->back_camera_box->findText(
            QString::fromStdString(emuenv.cfg.back_camera_id));
        m_ui->back_camera_box->setCurrentIndex(idx >= 0 ? idx : 0);
    }

    if (!emuenv.cfg.front_camera_image.empty())
        m_ui->front_camera_image_path->setText(QString::fromStdString(emuenv.cfg.front_camera_image));
    if (!emuenv.cfg.back_camera_image.empty())
        m_ui->back_camera_image_path->setText(QString::fromStdString(emuenv.cfg.back_camera_image));
}

void SettingsDialog::populate_adhoc_list() {
    m_ui->adhoc_address_box->clear();
    m_ui->subnet_mask_box->clear();

    const auto addrs = net_utils::get_all_assigned_addrs();
    for (const auto &addr : addrs) {
        const auto label = QString::fromStdString(addr.addr + " (" + addr.name + ")");
        m_ui->adhoc_address_box->addItem(label);
        m_ui->subnet_mask_box->addItem(QString::fromStdString(addr.netMask));
    }

    if (emuenv.cfg.adhoc_addr >= static_cast<int>(addrs.size())) {
        emuenv.cfg.adhoc_addr = 0;
        LOG_WARN("Invalid adhoc address index, resetting to 0");
    }

    if (!addrs.empty()) {
        m_ui->adhoc_address_box->setCurrentIndex(emuenv.cfg.adhoc_addr);
        m_ui->subnet_mask_box->setCurrentIndex(emuenv.cfg.adhoc_addr);
    }
}

void SettingsDialog::update_camera_widgets() {
    const int front_idx = m_ui->front_camera_box->currentIndex();
    m_ui->label_front_camera_color->setVisible(front_idx == 0);
    m_ui->front_camera_color_preview->setVisible(front_idx == 0);
    m_ui->front_camera_color_btn->setVisible(front_idx == 0);
    m_ui->label_front_camera_image->setVisible(front_idx == 1);
    m_ui->front_camera_image_path->setVisible(front_idx == 1);
    m_ui->front_camera_image_btn->setVisible(front_idx == 1);

    const int back_idx = m_ui->back_camera_box->currentIndex();
    m_ui->label_back_camera_color->setVisible(back_idx == 0);
    m_ui->back_camera_color_preview->setVisible(back_idx == 0);
    m_ui->back_camera_color_btn->setVisible(back_idx == 0);
    m_ui->label_back_camera_image->setVisible(back_idx == 1);
    m_ui->back_camera_image_path->setVisible(back_idx == 1);
    m_ui->back_camera_image_btn->setVisible(back_idx == 1);

    update_camera_color_preview();
}

void SettingsDialog::update_camera_color_preview() {
    const QColor front_color = QColor::fromRgba(emuenv.cfg.front_camera_color);
    m_ui->front_camera_color_preview->setStyleSheet(
        QStringLiteral("background-color: %1; border: 1px solid #888;").arg(front_color.name()));

    const QColor back_color = QColor::fromRgba(emuenv.cfg.back_camera_color);
    m_ui->back_camera_color_preview->setStyleSheet(
        QStringLiteral("background-color: %1; border: 1px solid #888;").arg(back_color.name()));
}

void SettingsDialog::update_file_loading_delay_label() {
    m_ui->file_loading_delay_label->setText(
        tr("File Loading Delay: %1 ms").arg(m_ui->file_loading_delay->value()));
}

void SettingsDialog::setup_connections() {
    for (auto *bb : m_button_boxes) {
        connect(bb->button(QDialogButtonBox::Save), &QPushButton::clicked,
            this, &SettingsDialog::on_save_clicked);
        connect(bb->button(QDialogButtonBox::Apply), &QPushButton::clicked,
            this, &SettingsDialog::on_apply_clicked);
        connect(bb->button(QDialogButtonBox::Close), &QPushButton::clicked,
            this, &SettingsDialog::on_close_clicked);
    }

    connect(m_ui->backend_renderer_box, &QComboBox::currentIndexChanged,
        this, &SettingsDialog::update_gpu_visibility);

    connect(m_ui->rb_modules_automatic, &QRadioButton::toggled,
        this, &SettingsDialog::update_modules_list_enabled);
    connect(m_ui->rb_modules_auto_manual, &QRadioButton::toggled,
        this, &SettingsDialog::update_modules_list_enabled);
    connect(m_ui->rb_modules_manual, &QRadioButton::toggled,
        this, &SettingsDialog::update_modules_list_enabled);

    connect(m_ui->resolution_upscale, &QSlider::valueChanged,
        this, &SettingsDialog::update_resolution_label);
    connect(m_ui->resolution_upscale_reset, &QPushButton::clicked, this, [this] {
        m_ui->resolution_upscale->setValue(4); // 1.0x
    });

    connect(m_ui->anisotropic_filter, &QSlider::valueChanged,
        this, &SettingsDialog::update_aniso_label);
    connect(m_ui->anisotropic_filter_reset, &QPushButton::clicked, this, [this] {
        m_ui->anisotropic_filter->setValue(0); // 1x
    });

    connect(m_ui->audio_volume, &QSlider::valueChanged, this, [this](int val) {
        m_ui->audio_volume_label->setText(tr("Audio Volume: %1%").arg(val));
    });

    connect(m_ui->perf_overlay_enabled, &QCheckBox::toggled, this, [this](bool on) {
        m_ui->perf_overlay_detail->setEnabled(on);
        m_ui->perf_overlay_position->setEnabled(on);
    });

    connect(m_ui->file_loading_delay, &QSlider::valueChanged,
        this, &SettingsDialog::update_file_loading_delay_label);

    connect(m_ui->modules_search_bar, &QLineEdit::textChanged, this, [this](const QString &text) {
        for (int i = 0; i < m_ui->modules_list->count(); ++i) {
            auto *item = m_ui->modules_list->item(i);
            item->setHidden(!item->text().contains(text, Qt::CaseInsensitive));
        }
    });

    connect(m_ui->clear_modules_list, &QPushButton::clicked, this, [this] {
        for (int i = 0; i < m_ui->modules_list->count(); ++i)
            m_ui->modules_list->item(i)->setCheckState(Qt::Unchecked);
    });
    connect(m_ui->refresh_modules_list, &QPushButton::clicked,
        this, &SettingsDialog::populate_modules_list);

    connect(m_ui->front_camera_box, &QComboBox::currentIndexChanged,
        this, &SettingsDialog::update_camera_widgets);
    connect(m_ui->back_camera_box, &QComboBox::currentIndexChanged,
        this, &SettingsDialog::update_camera_widgets);

    connect(m_ui->front_camera_color_btn, &QPushButton::clicked, this, [this] {
        const QColor color = QColorDialog::getColor(
            QColor::fromRgba(emuenv.cfg.front_camera_color), this, tr("Front Camera Color"));
        if (color.isValid()) {
            emuenv.cfg.front_camera_color = color.rgba();
            update_camera_color_preview();
        }
    });
    connect(m_ui->back_camera_color_btn, &QPushButton::clicked, this, [this] {
        const QColor color = QColorDialog::getColor(
            QColor::fromRgba(emuenv.cfg.back_camera_color), this, tr("Back Camera Color"));
        if (color.isValid()) {
            emuenv.cfg.back_camera_color = color.rgba();
            update_camera_color_preview();
        }
    });

    connect(m_ui->front_camera_image_btn, &QPushButton::clicked, this, [this] {
        const QString file = QFileDialog::getOpenFileName(
            this, tr("Select Front Camera Image"),
            QString::fromStdString(emuenv.cfg.front_camera_image),
            tr("Images (*.png *.jpg *.jpeg *.bmp)"));
        if (!file.isEmpty()) {
            emuenv.cfg.front_camera_image = file.toStdString();
            m_ui->front_camera_image_path->setText(file);
        }
    });
    connect(m_ui->back_camera_image_btn, &QPushButton::clicked, this, [this] {
        const QString file = QFileDialog::getOpenFileName(
            this, tr("Select Back Camera Image"),
            QString::fromStdString(emuenv.cfg.back_camera_image),
            tr("Images (*.png *.jpg *.jpeg *.bmp)"));
        if (!file.isEmpty()) {
            emuenv.cfg.back_camera_image = file.toStdString();
            m_ui->back_camera_image_path->setText(file);
        }
    });

    connect(m_ui->adhoc_address_box, &QComboBox::currentIndexChanged, this, [this](int idx) {
        m_ui->subnet_mask_box->setCurrentIndex(idx);
    });

    connect(m_ui->change_emu_path, &QPushButton::clicked, this, [this] {
        const QString dir = QFileDialog::getExistingDirectory(
            this, tr("Select Emulator Storage Folder"),
            QString::fromStdString(emuenv.cfg.pref_path));
        if (!dir.isEmpty()) {
            emuenv.pref_path = fs::path(dir.toStdString()) / "";
            emuenv.cfg.set_pref_path(emuenv.pref_path);
            m_ui->current_emu_path->setText(
                tr("Current path: %1").arg(dir));
        }
    });
    connect(m_ui->reset_emu_path, &QPushButton::clicked, this, [this] {
        if (emuenv.default_path != emuenv.pref_path) {
            emuenv.pref_path = emuenv.default_path;
            emuenv.cfg.set_pref_path(emuenv.pref_path);
            m_ui->current_emu_path->setText(
                tr("Current path: %1").arg(QString::fromStdString(emuenv.cfg.pref_path)));
        }
    });

    connect(m_ui->clear_custom_config, &QPushButton::clicked, this, [this] {
        const auto config_dir = emuenv.config_path / "config";
        if (fs::exists(config_dir)) {
            fs::remove_all(config_dir);
            LOG_INFO("Cleared all custom config settings.");
        }
    });

    connect(m_ui->clean_shaders_cache, &QPushButton::clicked, this, [this] {
        const auto shaders_cache_path = emuenv.cache_path / "shaders";
        fs::remove_all(shaders_cache_path);
        fs::remove_all(emuenv.cache_path / "shaderlog");
        fs::remove_all(emuenv.log_path / "shaderlog");
        LOG_INFO("Shaders cache cleaned.");
    });

    connect(m_ui->watch_code, &QPushButton::clicked, this, [this] {
        emuenv.kernel.debugger.watch_code = !emuenv.kernel.debugger.watch_code;
        emuenv.kernel.debugger.update_watches();
        m_ui->watch_code->setText(emuenv.kernel.debugger.watch_code ? tr("Unwatch Code") : tr("Watch Code"));
    });
    connect(m_ui->watch_memory, &QPushButton::clicked, this, [this] {
        emuenv.kernel.debugger.watch_memory = !emuenv.kernel.debugger.watch_memory;
        emuenv.kernel.debugger.update_watches();
        m_ui->watch_memory->setText(emuenv.kernel.debugger.watch_memory ? tr("Unwatch Memory") : tr("Watch Memory"));
    });
    connect(m_ui->watch_import_calls, &QPushButton::clicked, this, [this] {
        emuenv.kernel.debugger.watch_import_calls = !emuenv.kernel.debugger.watch_import_calls;
        emuenv.kernel.debugger.update_watches();
        m_ui->watch_import_calls->setText(emuenv.kernel.debugger.watch_import_calls ? tr("Unwatch Import Calls") : tr("Watch Import Calls"));
    });

    m_ui->watch_code->setText(emuenv.kernel.debugger.watch_code ? tr("Unwatch Code") : tr("Watch Code"));
    m_ui->watch_memory->setText(emuenv.kernel.debugger.watch_memory ? tr("Unwatch Memory") : tr("Watch Memory"));
    m_ui->watch_import_calls->setText(emuenv.kernel.debugger.watch_import_calls ? tr("Unwatch Import Calls") : tr("Watch Import Calls"));

    const std::vector<std::pair<QWidget *, QString>> desc_widgets = {
        // Core
        { m_ui->rb_modules_automatic, desc::modules_automatic },
        { m_ui->rb_modules_auto_manual, desc::modules_auto_manual },
        { m_ui->rb_modules_manual, desc::modules_manual },
        // CPU
        { m_ui->cpu_opt, desc::cpu_opt },
        // GPU
        { m_ui->backend_renderer_box, desc::backend_renderer },
        { m_ui->renderer_accuracy_box, desc::renderer_accuracy },
        { m_ui->vsync, desc::vsync },
        { m_ui->disable_surface_sync, desc::disable_surface_sync },
        { m_ui->async_pipeline_compilation, desc::async_pipeline },
        { m_ui->memory_mapping_box, desc::memory_mapping },
        { m_ui->screen_filter_box, desc::screen_filter },
        { m_ui->resolution_upscale, desc::resolution_upscaling },
        { m_ui->anisotropic_filter, desc::anisotropic_filtering },
        { m_ui->export_textures, desc::export_textures },
        { m_ui->import_textures, desc::import_textures },
        { m_ui->shader_cache, desc::shader_cache },
        { m_ui->spirv_shader, desc::spirv_shader },
        { m_ui->fps_hack, desc::fps_hack },
        // Audio
        { m_ui->audio_backend_box, desc::audio_backend },
        { m_ui->audio_volume, desc::audio_volume },
        { m_ui->ngs_enable, desc::ngs_enable },
        // Camera
        { m_ui->front_camera_box, desc::front_camera },
        { m_ui->back_camera_box, desc::back_camera },
        // System
        { m_ui->enter_button_circle, desc::enter_button },
        { m_ui->enter_button_cross, desc::enter_button },
        { m_ui->pstv_mode, desc::pstv_mode },
        { m_ui->show_mode, desc::show_mode },
        { m_ui->demo_mode, desc::demo_mode },
        { m_ui->sys_lang_box, desc::sys_lang },
        { m_ui->sys_date_format_box, desc::sys_date_format },
        { m_ui->sys_time_format_box, desc::sys_time_format },
        { m_ui->ime_langs_list, desc::ime_langs },
        // Emulator
        { m_ui->boot_apps_fullscreen, desc::boot_fullscreen },
        { m_ui->show_compile_shaders, desc::show_compile_shaders },
        { m_ui->check_for_updates, desc::check_for_updates },
        { m_ui->log_compat_warn, desc::log_compat_warn },
        { m_ui->texture_cache, desc::texture_cache },
        { m_ui->archive_log, desc::archive_log },
        { m_ui->discord_rich_presence, desc::discord_rpc },
        { m_ui->perf_overlay_enabled, desc::perf_overlay },
        { m_ui->stretch_the_display_area, desc::stretch_display },
        { m_ui->fullscreen_hd_res_pixel_perfect, desc::pixel_perfect },
        { m_ui->file_loading_delay, desc::file_loading_delay },
        // GUI
        { m_ui->show_welcome, desc::show_welcome_screen },
        { m_ui->log_buffer_size, desc::log_buffer_size },
        // Network
        { m_ui->psn_signed_in, desc::psn_signed_in },
        { m_ui->http_enable, desc::http_enable },
        { m_ui->adhoc_address_box, desc::adhoc_address },
        // Debug
        { m_ui->log_imports, desc::log_imports },
        { m_ui->log_exports, desc::log_exports },
        { m_ui->log_active_shaders, desc::log_active_shaders },
        { m_ui->log_uniforms, desc::log_uniforms },
        { m_ui->color_surface_debug, desc::color_surface_debug },
        { m_ui->validation_layer, desc::validation_layer },
        { m_ui->dump_elfs, desc::dump_elfs },
    };

    for (const auto &[widget, description] : desc_widgets) {
        widget->setProperty("_desc", description);
        widget->installEventFilter(this);
    }
}

bool SettingsDialog::eventFilter(QObject *watched, QEvent *event) {
    if (event->type() == QEvent::Enter) {
        auto *w = qobject_cast<QWidget *>(watched);
        if (w) {
            const QString d = w->property("_desc").toString();
            if (!d.isEmpty()) {
                QWidget *tab = w;
                while (tab && m_ui->tab_widget_settings->indexOf(tab) == -1)
                    tab = tab->parentWidget();
                if (tab)
                    set_description(tab, d);
            }
        }
    } else if (event->type() == QEvent::Leave) {
        auto *w = qobject_cast<QWidget *>(watched);
        if (w && !w->property("_desc").toString().isEmpty()) {
            QWidget *tab = w;
            while (tab && m_ui->tab_widget_settings->indexOf(tab) == -1)
                tab = tab->parentWidget();
            if (tab)
                set_description(tab, tr("Point your mouse at an option to display a description in here."));
        }
    }
    return QDialog::eventFilter(watched, event);
}

void SettingsDialog::set_description(QWidget *tab, const QString &text) {
    const std::vector<std::pair<QWidget *, QLabel *>> map = {
        { m_ui->core_tab, m_ui->description_core },
        { m_ui->cpu_tab, m_ui->description_cpu },
        { m_ui->gpu_tab, m_ui->description_gpu },
        { m_ui->audio_tab, m_ui->description_audio },
        { m_ui->camera_tab, m_ui->description_camera },
        { m_ui->system_tab, m_ui->description_system },
        { m_ui->emulator_tab, m_ui->description_emulator },
        { m_ui->gui_tab, m_ui->description_gui },
        { m_ui->network_tab, m_ui->description_network },
        { m_ui->debug_tab, m_ui->description_debug },
    };
    for (const auto &[t, label] : map) {
        if (t == tab) {
            label->setText(text);
            return;
        }
    }
}

void SettingsDialog::update_resolution_label() {
    const float mult = static_cast<float>(m_ui->resolution_upscale->value()) / 4.0f;
    const int w = static_cast<int>(960 * mult);
    const int h = static_cast<int>(544 * mult);
    m_ui->resolution_upscale_val->setText(
        QStringLiteral("%1x%2 (%3x)").arg(w).arg(h).arg(static_cast<double>(mult), 0, 'g', 3));
}

void SettingsDialog::update_aniso_label() {
    const int val = 1 << m_ui->anisotropic_filter->value();
    m_ui->anisotropic_filter_val->setText(QStringLiteral("%1x").arg(val));
}

void SettingsDialog::on_save_clicked() {
    save_config();
    apply_config();
    m_dirty = false;
    accept();
}

void SettingsDialog::on_apply_clicked() {
    save_config();
    apply_config();
    m_dirty = false;
    for (auto *bb : m_button_boxes)
        bb->button(QDialogButtonBox::Apply)->setEnabled(false);
}

void SettingsDialog::on_close_clicked() {
    reject();
}

void SettingsDialog::reject() {
    if (m_dirty) {
        const int result = QMessageBox::warning(this,
            tr("Unsaved Changes"),
            tr("You have unsaved changes. What would you like to do?"),
            QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel,
            QMessageBox::Save);

        switch (result) {
        case QMessageBox::Save:
            save_config();
            apply_config();
            m_dirty = false;
            QDialog::accept();
            return;
        case QMessageBox::Discard:
            QDialog::reject();
            return;
        default: // Cancel
            return;
        }
    }
    QDialog::reject();
}

void SettingsDialog::mark_dirty() {
    if (!m_dirty) {
        m_dirty = true;
        for (auto *bb : m_button_boxes)
            bb->button(QDialogButtonBox::Apply)->setEnabled(true);
    }
}

void SettingsDialog::setup_dirty_tracking() {
    for (auto *cb : findChildren<QCheckBox *>())
        connect(cb, &QCheckBox::toggled, this, [this] { mark_dirty(); });
    for (auto *rb : findChildren<QRadioButton *>())
        connect(rb, &QRadioButton::toggled, this, [this] { mark_dirty(); });
    for (auto *combo : findChildren<QComboBox *>())
        connect(combo, &QComboBox::currentIndexChanged, this, [this] { mark_dirty(); });
    for (auto *slider : findChildren<QSlider *>())
        connect(slider, &QSlider::valueChanged, this, [this] { mark_dirty(); });
    for (auto *list : findChildren<QListWidget *>())
        connect(list, &QListWidget::itemChanged, this, [this] { mark_dirty(); });
}

void SettingsDialog::update_gpu_visibility() {
    const bool is_vulkan = m_ui->backend_renderer_box->currentText() == QStringLiteral("Vulkan");

    const int mask = emuenv.renderer
        ? emuenv.renderer->supported_mapping_methods_mask
        : renderer::enumerate_vulkan_mapping_methods(emuenv.cfg.gpu_idx);
    const bool has_mapping = is_vulkan && (mask > 1);

    // Vulkan-only widgets
    m_ui->gb_gpu_device->setVisible(is_vulkan);
    m_ui->gb_renderer_accuracy->setVisible(is_vulkan);
    m_ui->gb_vulkan_options->setVisible(is_vulkan);
    m_ui->spirv_shader->setVisible(is_vulkan);

    m_ui->label_memory_mapping->setVisible(has_mapping);
    m_ui->memory_mapping_box->setVisible(has_mapping);

    // OpenGL-only widgets
    m_ui->gb_opengl_options->setVisible(!is_vulkan);

    {
        const QString previous = m_ui->screen_filter_box->currentText().isEmpty()
            ? QString::fromStdString(m_config.screen_filter)
            : m_ui->screen_filter_box->currentText();

        m_ui->screen_filter_box->clear();
        if (is_vulkan) {
            m_ui->screen_filter_box->addItems({ QStringLiteral("Nearest"), QStringLiteral("Bilinear"),
                QStringLiteral("Bicubic"), QStringLiteral("FXAA"), QStringLiteral("FSR") });
        } else {
            m_ui->screen_filter_box->addItems({ QStringLiteral("Bilinear"), QStringLiteral("FXAA") });
        }

        const int idx = m_ui->screen_filter_box->findText(previous);
        m_ui->screen_filter_box->setCurrentIndex(idx >= 0 ? idx : 0);
    }
}