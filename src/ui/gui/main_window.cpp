#include "main_window.hpp"
#include "main_window_helpers.hpp"
#include "localization.hpp"
#include "settings_editor.hpp"
#include "onedrive/version.hpp"

#include <QApplication>
#include <QCloseEvent>
#include <QEvent>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QTabWidget>
#include <QWidget>
#include <QGroupBox>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QCheckBox>
#include <QComboBox>
#include <QTreeWidget>
#include <QSignalBlocker>
#include <QTimer>

#include <utility>

namespace onedrive::gui {

using window_detail::drive_type_text;

MainWindow::MainWindow(
    AppStateViewModel state,
    AuthenticationFunction authenticate,
    Localization* localization,
    SynchronizationFunction synchronize,
    MonitoringFunction monitor
)
    : state_{std::move(state)},
      localization_{localization},
      tabs_{new QTabWidget{this}},
      login_{std::move(authenticate)},
      synchronization_{std::move(synchronize), nullptr, std::move(monitor)} {
    setWindowTitle(tr("onedrive client"));
    resize(860, 520);

    tabs_->setObjectName("mainTabs");
    tabs_->setUsesScrollButtons(true);
    create_overview_page();
    create_sync_page();
    create_logs_page();
    create_settings_page();
    create_files_page();
    tabs_->addTab(settings_, tr("Settings"));
    setCentralWidget(tabs_);
    create_header_controls();
    connect_sync_controller();
    connect_login_controller();
    refresh_settings();
    retranslate_ui();
    log_message(
        LogOperation::application,
        events::MessageKind::information,
        QT_TR_NOOP("Application started.")
    );
    log_message(
        LogOperation::account,
        events::MessageKind::information,
        login_status_source_,
        login_status_detail_
    );
    log_startup_errors();
    if (state_.configuration_status != ConfigurationStatus::valid) {
        tabs_->setCurrentWidget(settings_);
    }
}

void MainWindow::changeEvent(QEvent* event) {
    QMainWindow::changeEvent(event);
    if (event->type() == QEvent::LanguageChange && tabs_) {
        retranslate_ui();
    }
}

void MainWindow::retranslate_ui() {
    setWindowTitle(tr("onedrive client"));
    about_button_->setText(tr("About"));
    QApplication::setApplicationDisplayName(windowTitle());
    tabs_->setTabText(0, tr("Overview"));
    tabs_->setTabText(1, tr("Sync"));
    tabs_->setTabText(2, tr("Logs"));
    tabs_->setTabText(tabs_->indexOf(settings_), tr("Settings"));
    tabs_->setTabText(tabs_->indexOf(files_page_), tr("Files"));
    files_hint_->setText(tr(
        "Completed operations since this application started. Times are local. "
        "Preview and external Monitor operations are not included."
    ));
    files_filter_->setPlaceholderText(
        tr("Search file paths, actions and drives")
    );
    files_table_->setHeaderLabels(
        {tr("Time"), tr("Action"), tr("Path"), tr("Previous path"), tr("Drive")}
    );
    render_file_history();
    blocked_filter_->setPlaceholderText(tr("Search paths and reasons"));
    blocked_refresh_->setText(tr("Refresh"));
    blocked_table_->setHeaderLabels({tr("Path"), tr("Reason")});
    if (blocked_reason_->count() != 0) {
        blocked_reason_->setItemText(0, tr("All reasons"));
    }
    render_blocked_items();
    const auto set_label =
        [](QFormLayout* layout, int row, const QString& text) {
            auto* label = qobject_cast<QLabel*>(
                layout->itemAt(row, QFormLayout::LabelRole)->widget()
            );
            label->setText(text);
        };
    set_label(overview_layout_, 0, tr("Account"));
    set_label(overview_layout_, 1, tr("Configuration file"));
    set_label(overview_layout_, 2, tr("Sync directory"));
    set_label(overview_layout_, 3, tr("State directory"));
    set_label(overview_layout_, 4, tr("Login"));
    set_label(overview_layout_, 5, tr("Browser login"));
    set_label(overview_layout_, 8, tr("Synchronization"));
    set_label(overview_layout_, 9, tr("Drive type"));
    set_label(settings_layout_, 0, tr("Configuration file"));
    set_label(settings_layout_, 2, tr("Appearance"));
    settings_editor_->retranslate();
    theme_value_->setText(tr("Follow system light/dark appearance"));
    login_button_->setText(tr("Sign in"));
    refresh_actions();
    sync_group_->setTitle(tr("Synchronization"));
    preview_button_->setText(tr("Preview changes"));
    sync_button_->setText(tr("Sync now"));
    monitor_button_->setText(tr("Start monitoring"));
    stop_sync_button_->setText(tr("Stop"));
    log_level_->setItemText(0, tr("All levels"));
    log_level_->setItemText(1, tr("Trace"));
    log_level_->setItemText(2, tr("Debug"));
    log_level_->setItemText(3, tr("Information"));
    log_level_->setItemText(4, tr("Warning"));
    log_level_->setItemText(5, tr("Error"));
    log_level_->setItemText(6, tr("Critical"));
    log_level_->setAccessibleName(tr("Log level"));
    log_level_->setToolTip(tr("Show messages at this level or higher."));
    log_autoscroll_->setText(tr("Auto-scroll"));
    copy_logs_button_->setText(tr("Copy logs"));
    copy_logs_button_->setToolTip(tr("Copy the currently displayed logs."));
    clear_logs_button_->setText(tr("Clear logs"));
    clear_logs_button_->setToolTip(
        tr("Clear session history without stopping the active task.")
    );
    preview_button_->setToolTip(
        tr("Preview using saved settings; no synchronized files are changed.")
    );
    sync_button_->setToolTip(
        tr("Use saved settings; files may be uploaded, downloaded or deleted.")
    );
    monitor_button_->setToolTip(
        tr("Start a GUI-owned Monitor using saved settings. Closing this "
           "window stops only its own task.")
    );
    monitor_button_->setAccessibleDescription(monitor_button_->toolTip());
    stop_sync_button_->setToolTip(
        tr("Request cancellation and wait for the task to finish.")
    );
    preview_button_->setAccessibleDescription(preview_button_->toolTip());
    sync_button_->setAccessibleDescription(sync_button_->toolTip());
    stop_sync_button_->setAccessibleDescription(stop_sync_button_->toolTip());
    log_messages_->setAccessibleName(tr("Session logs"));
    sync_progress_->setAccessibleName(tr("Synchronization progress"));
    render_sync();
    render_logs();
    import_button_->setText(tr("Import TOML…"));
    save_button_->setText(tr("Save settings"));
    reload_button_->setText(tr("Reload configuration"));
    for (auto* browse : settings_->findChildren<QPushButton*>("browseButton")) {
        browse->setText(tr("Browse…"));
    }
    account_status_value_->setText(account_status_text(state_.account_status));
    drive_type_value_->setText(drive_type_text(state_.drive_type));
    refresh_startup_status();
    set_login_status(
        login_status_source_,
        login_status_detail_,
        events::MessageKind::information,
        false
    );
    if (login_url_is_instruction_) {
        login_url_->setText(
            tr("Complete authorization in your system browser.")
        );
    }
    if (language_selector_) {
        const QSignalBlocker blocker{language_selector_};
        language_label_->setText(tr("Language"));
        language_selector_->setAccessibleName(tr("Language"));
        for (int index = 0; index < language_selector_->count(); ++index) {
            const auto language = static_cast<Language>(
                language_selector_->itemData(index).toInt()
            );
            switch (language) {
            case Language::automatic:
                language_selector_->setItemText(index, tr("System language"));
                break;
            case Language::english:
                language_selector_->setItemText(index, tr("English"));
                break;
            case Language::simplified_chinese:
                language_selector_->setItemText(index, tr("Simplified Chinese"));
                break;
            }
            if (localization_->language() == language) {
                language_selector_->setCurrentIndex(index);
            }
        }
    }
}

void MainWindow::show_about() {
    QMessageBox dialog{this};
    dialog.setObjectName("aboutDialog");
    dialog.setWindowTitle(tr("About OneDrive client"));
    dialog.setIcon(QMessageBox::Information);
    dialog.setTextFormat(Qt::RichText);
    dialog.setText(
        tr("<b>OneDrive client</b><br>"
           "Version: %1<br>"
           "An independent, open-source OneDrive client.<br>"
           "License: GPL-3.0-or-later<br>"
           "Project: <a href=\"https://github.com/cloudfyy/onedrivecpp\">"
           "https://github.com/cloudfyy/onedrivecpp</a>")
            .arg(QString::fromLatin1(build_info::version))
    );
    dialog.setStandardButtons(QMessageBox::Ok);
    dialog.exec();
}

void MainWindow::refresh_actions() {
    const bool running = login_.running();
    const bool syncing = synchronization_.running();
    const bool occupied = monitor_busy();
    const bool signed_in =
        state_.account_status == AccountStatus::credentials_available;
    login_button_->setEnabled(
        !running && !syncing && !occupied && !signed_in && state_.can_sign_in()
    );
    account_action_button_->setText(
        running ? tr("Cancel login") : tr("Log out")
    );
    account_action_button_->setEnabled(
        !syncing && (running ? !cancelling_ : !occupied && signed_in)
    );
    const bool can_sync = !running && !syncing && !occupied &&
                          !monitor_.uncertain() && state_.can_synchronize();
    preview_button_->setEnabled(can_sync);
    const bool can_trigger =
        monitor_.status() && monitor_.status()->state == "waiting" &&
        !monitor_.status()->pending && monitor_.error().isEmpty() &&
        !monitor_.busy() && !sync_stopping_ && !running &&
        (!syncing || sync_monitoring_);
    sync_button_->setEnabled(can_sync || can_trigger);
    monitor_button_->setEnabled(can_sync);
    stop_sync_button_->setEnabled(
        !sync_stopping_ &&
        (syncing ||
         (monitor_.status() && monitor_.status()->state != "stopping" &&
          !monitor_.busy() && monitor_.error().isEmpty()))
    );
    settings_->setEnabled(!running && !syncing && !occupied);
}

void MainWindow::closeEvent(QCloseEvent* event) {
    closing_ = true;
    monitor_timer_->stop();
    if (login_.running()) {
        closing_ = true;
        cancel_login();
        event->ignore();
        return;
    }
    if (synchronization_.running()) {
        closing_ = true;
        stop_sync();
        event->ignore();
        return;
    }
    QMainWindow::closeEvent(event);
}

} // namespace onedrive::gui
