#include "main_window.hpp"
#include "main_window_helpers.hpp"
#include "configuration.hpp"
#include "localization.hpp"
#include "settings_editor.hpp"
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QTabWidget>
#include <exception>

namespace onedrive::gui {

using window_detail::drive_type_text;
using window_detail::make_value_label;

void MainWindow::create_settings_page() {
    auto* settings = new QWidget{tabs_};
    settings_ = settings;
    settings_->setObjectName("settingsPage");
    auto* settings_layout = new QFormLayout{settings};
    settings_layout_ = settings_layout;
    auto* config_path = make_value_label({});
    config_path->setTextInteractionFlags(Qt::TextSelectableByMouse);
    settings_layout->addRow(tr("Configuration file"), config_path);
    settings_editor_ = new SettingsEditor{settings};
    settings_layout->addRow(settings_editor_);
    sync_directory_edit_ =
        settings_editor_->findChild<QLineEdit*>("sync.data_directory");
    state_directory_edit_ =
        settings_editor_->findChild<QLineEdit*>("state.directory");
    auto* theme = make_value_label(tr("Follow system light/dark appearance"));
    theme_value_ = theme;
    settings_layout->addRow(tr("Appearance"), theme);

    auto* actions = new QHBoxLayout;
    auto* import_button = new QPushButton{tr("Import TOML…"), settings};
    auto* save_button = new QPushButton{tr("Save settings"), settings};
    import_button_ = import_button;
    save_button_ = save_button;
    import_button_->setObjectName("importSettingsButton");
    save_button_->setObjectName("saveSettingsButton");
    reload_button_ = new QPushButton{tr("Reload configuration"), settings};
    reload_button_->setObjectName("reloadSettingsButton");
    actions->addWidget(import_button);
    actions->addWidget(reload_button_);
    actions->addStretch();
    actions->addWidget(save_button);
    settings_layout->addRow(actions);
    settings_status_ = make_value_label({});
    settings_status_->setObjectName("settingsStatus");
    settings_layout->addRow(settings_status_);
    config_path->setText(QString::fromStdString(state_.config_file.string()));
    connect(import_button, &QPushButton::clicked, this, [this] {
        import_settings();
    });
    connect(save_button, &QPushButton::clicked, this, [this] {
        save_settings();
    });
    connect(
        reload_button_,
        &QPushButton::clicked,
        this,
        &MainWindow::reload_settings
    );
}

void MainWindow::refresh_settings(bool preserve_edits) {
    synchronization_.set_file_history_limit(state_.file_history_limit);
    monitor_.set_state_directory(
        state_.configuration_status == ConfigurationStatus::valid &&
                !state_.state_directory.empty()
            ? state_.state_directory
            : std::filesystem::path{}
    );
    refresh_monitor();
    blocked_items_.clear();
    blocked_error_.clear();
    render_blocked_items();
    refresh_blocked_items();
    config_file_value_->setText(
        QString::fromStdString(state_.config_file.string())
    );
    account_status_value_->setText(account_status_text(state_.account_status));
    drive_type_value_->setText(drive_type_text(state_.drive_type));
    sync_directory_value_->setText(
        QString::fromStdString(state_.sync_directory.string())
    );
    state_directory_value_->setText(
        QString::fromStdString(state_.state_directory.string())
    );
    if (!preserve_edits) {
        if (state_.configuration_status != ConfigurationStatus::invalid) {
            bool missing = false;
            settings_editor_->load(
                load_configuration(state_.config_file, missing)
            );
        }
    }
    const bool editable =
        state_.configuration_status != ConfigurationStatus::invalid;
    sync_directory_edit_->setEnabled(editable);
    state_directory_edit_->setEnabled(editable);
    settings_editor_->setEnabled(editable);
    save_button_->setEnabled(editable);
    refresh_actions();
    render_sync();
    refresh_startup_status();
    if (!state_.can_sign_in()) {
        set_login_status(
            QT_TR_NOOP(
                "Sign in is unavailable. Resolve the errors in Settings, then "
                "reload the configuration."
            ),
            {},
            events::MessageKind::information,
            false
        );
    } else if (state_.account_status == AccountStatus::credentials_available) {
        set_login_status(
            QT_TR_NOOP(
                "Saved sign-in information found. "
                "It will be checked when connecting to OneDrive."
            ),
            {},
            events::MessageKind::information,
            false
        );
    } else {
        set_login_status(
            QT_TR_NOOP("Sign in to connect a OneDrive account."),
            {},
            events::MessageKind::information,
            false
        );
    }
}

void MainWindow::refresh_startup_status() {
    QStringList messages;
    if (state_.configuration_status == ConfigurationStatus::missing) {
        messages << tr(
            "Configuration file is missing. Using defaults; complete setup "
            "in Settings."
        );
    } else if (state_.configuration_status == ConfigurationStatus::invalid) {
        messages
            << tr("Configuration could not be loaded:\n%1\n"
                  "Import a valid TOML file, or repair this file and reload. "
                  "The original file has not been changed.")
                   .arg(QString::fromStdString(state_.configuration_error));
    }
    if (!state_.authentication_error.empty()) {
        messages << tr("Authentication configuration needs attention:\n%1")
                        .arg(
                            QString::fromStdString(state_.authentication_error)
                        );
    }
    if (!state_.account_error.empty()) {
        messages << tr("Local account credentials could not be read:\n%1\n"
                       "Repair the state directory or select another one in "
                       "Settings, "
                       "then reload.")
                        .arg(QString::fromStdString(state_.account_error));
    }
    if (!state_.sync_error.empty()) {
        messages
            << tr("Sync directory needs attention before synchronization:\n%1")
                   .arg(QString::fromStdString(state_.sync_error));
    }
    startup_status_->setText(messages.join("\n\n"));
    settings_status_->setText(startup_status_->text());
}

void MainWindow::reload_settings() {
    if (login_.running() || synchronization_.running() || monitor_busy()) {
        return;
    }
    state_ = load_app_state_view_model(state_.config_file);
    refresh_settings();
    if (state_.configuration_status == ConfigurationStatus::valid) {
        log_message(
            LogOperation::configuration,
            events::MessageKind::information,
            QT_TR_NOOP("Configuration reloaded.")
        );
    }
    log_startup_errors();
}

void MainWindow::save_settings() {
    if (login_.running() || synchronization_.running() || monitor_busy()) {
        return;
    }
    std::vector<config::SettingPatch> patches;
    try {
        patches = settings_editor_->patches();
    } catch (const std::exception& error) {
        log_message(
            LogOperation::configuration,
            events::MessageKind::error,
            QT_TR_NOOP("Settings could not be saved:\n%1"),
            QString::fromUtf8(error.what())
        );
        QMessageBox::critical(
            this,
            tr("Cannot save settings"),
            tr("Settings could not be saved:\n%1")
                .arg(QString::fromUtf8(error.what()))
        );
        return;
    }
    const auto new_sync_directory =
        std::filesystem::path{sync_directory_edit_->text().toStdString()};
    const auto new_state_directory =
        std::filesystem::path{state_directory_edit_->text().toStdString()};
    const bool sync_path_changed = new_sync_directory != state_.sync_directory;
    const bool state_path_changed =
        new_state_directory != state_.state_directory;
    if (sync_path_changed || state_path_changed) {
        auto summary =
            tr("Save these directory changes?\n\n"
               "Existing files and synchronization state will not be moved.");
        if (sync_path_changed) {
            summary +=
                tr("\n\nSync directory:\n%1\n→ %2\n"
                   "Inspect a dry run before the next synchronization.")
                    .arg(
                        QString::fromStdString(state_.sync_directory.string()),
                        QString::fromStdString(new_sync_directory.string())
                    );
        }
        if (state_path_changed) {
            summary +=
                tr("\n\nState directory:\n%1\n→ %2\n"
                   "Existing account state and credentials will remain "
                   "in the old location.")
                    .arg(
                        QString::fromStdString(state_.state_directory.string()),
                        QString::fromStdString(new_state_directory.string())
                    );
        }
        if (QMessageBox::question(
                this,
                tr("Confirm directory changes"),
                summary,
                QMessageBox::Save | QMessageBox::Cancel
            ) != QMessageBox::Save) {
            return;
        }
    }

    try {
        const auto backup = save_settings_patch(state_.config_file, patches);
        state_ = load_app_state_view_model(state_.config_file);
        refresh_settings();
        log_message(
            LogOperation::configuration,
            events::MessageKind::information,
            QT_TR_NOOP(
                "Settings saved. Changes take effect on the next operation."
            )
        );
        auto message =
            tr("Settings saved. Changes take effect on the next operation.");
        if (backup) {
            message += tr("\nPrevious configuration backed up to:\n%1")
                           .arg(QString::fromStdString(backup->string()));
        }
        QMessageBox::information(this, tr("Settings saved"), message);
    } catch (const std::exception& error) {
        log_message(
            LogOperation::configuration,
            events::MessageKind::error,
            QT_TR_NOOP("Settings could not be saved:\n%1"),
            QString::fromUtf8(error.what())
        );
        QMessageBox::critical(
            this,
            tr("Cannot save settings"),
            tr("Settings could not be saved:\n%1")
                .arg(QString::fromUtf8(error.what()))
        );
    }
}

void MainWindow::import_settings() {
    if (login_.running() || synchronization_.running() || monitor_busy()) {
        return;
    }
    const auto selected = QFileDialog::getOpenFileName(
        this,
        tr("Import configuration"),
        {},
        tr("TOML files (*.toml);;All files (*)")
    );
    if (selected.isEmpty()) {
        return;
    }

    const auto source = std::filesystem::path{selected.toStdString()};
    try {
        const auto preview = preview_configuration(source, state_.config_file);
        const auto message =
            tr("Import this configuration to the active user "
               "configuration?\n\n"
               "Sync directory: %1\n"
               "State directory: %2\n"
               "Sync mode: %3\n"
               "Deletion policy: %4\n\n"
               "Relative file references will be resolved from the active "
               "configuration directory. The current configuration will be "
               "backed up.")
                .arg(
                    QString::fromStdString(preview.sync_directory.string()),
                    QString::fromStdString(preview.state_directory.string()),
                    sync_mode_text(preview.sync_mode),
                    delete_policy_text(preview.delete_policy)
                );
        if (QMessageBox::question(
                this,
                tr("Import configuration"),
                message,
                QMessageBox::Yes | QMessageBox::Cancel
            ) != QMessageBox::Yes) {
            return;
        }

        const auto backup = import_configuration(source, state_.config_file);
        state_ = load_app_state_view_model(state_.config_file);
        refresh_settings();
        log_message(
            LogOperation::configuration,
            events::MessageKind::information,
            QT_TR_NOOP("Configuration imported.")
        );
        log_startup_errors();
        auto confirmation = tr("Configuration imported.");
        if (backup) {
            confirmation += tr("\nPrevious configuration backed up to:\n%1")
                                .arg(QString::fromStdString(backup->string()));
        }
        QMessageBox::information(
            this, tr("Configuration imported"), confirmation
        );
    } catch (const std::exception& error) {
        log_message(
            LogOperation::configuration,
            events::MessageKind::error,
            QT_TR_NOOP("Configuration could not be imported:\n%1"),
            QString::fromUtf8(error.what())
        );
        QMessageBox::critical(
            this,
            tr("Cannot import configuration"),
            tr("Configuration could not be imported:\n%1")
                .arg(QString::fromUtf8(error.what()))
        );
    }
}

} // namespace onedrive::gui
