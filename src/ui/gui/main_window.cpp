#include "main_window.hpp"
#include <QTimer>
#include "onedrive/sync/blocked_policy.hpp"

#include "configuration.hpp"
#include "localization.hpp"
#include "settings_editor.hpp"
#include "onedrive/version.hpp"
#include "onedrive/app/factory.hpp"
#include "onedrive/app/queries.hpp"
#include "onedrive/exit_code.hpp"
#include "onedrive/util/byte_size.hpp"

#include <QActionGroup>
#include <QApplication>
#include <QFileDialog>
#include <QCloseEvent>
#include <QDesktopServices>
#include <QEvent>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QMenuBar>
#include <QPushButton>
#include <QTabWidget>
#include <QWidget>
#include <QUrl>
#include <QGroupBox>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QVBoxLayout>
#include <QScrollBar>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QTextBlock>
#include <QTextCursor>
#include <QTreeWidget>
#include <QHeaderView>
#include <QSignalBlocker>

#include <exception>
#include <utility>

namespace onedrive::gui {
namespace {

constexpr std::size_t maximum_log_history = 500;

class LogBlockData final : public QTextBlockUserData {
public:
    explicit LogBlockData(std::size_t sequence)
        : sequence{sequence} {
    }
    std::size_t sequence;
};

QString short_detail(const QString& text) {
    const auto first_line = text.section('\n', 0, 0);
    return first_line.size() > 240 ? first_line.left(237) + "..." : first_line;
}

QString number(std::uint64_t value) {
    return QString::number(static_cast<qulonglong>(value));
}

QString data_size(std::uint64_t bytes, const QLocale& locale) {
    const auto size = util::scale_bytes(bytes);
    const auto amount = size.unit == "B"
                            ? locale.toString(static_cast<qulonglong>(bytes))
                            : locale.toString(size.value, 'f', 1);
    return QString{"%1 %2"}.arg(
        amount,
        QLatin1String{
            size.unit.data(), static_cast<qsizetype>(size.unit.size())
        }
    );
}

QString duration(std::uint64_t seconds, const QLocale& locale) {
    const auto amount = [&locale](std::uint64_t value) {
        return locale.toString(static_cast<qulonglong>(value));
    };
    if (seconds >= 86400) {
        return MainWindow::tr("%1 d %2 h")
            .arg(amount(seconds / 86400), amount(seconds % 86400 / 3600));
    }
    if (seconds >= 3600) {
        return MainWindow::tr("%1 h %2 min")
            .arg(amount(seconds / 3600), amount(seconds % 3600 / 60));
    }
    if (seconds >= 60) {
        return MainWindow::tr("%1 min %2 s")
            .arg(amount(seconds / 60), amount(seconds % 60));
    }
    return MainWindow::tr("%1 s").arg(amount(seconds));
}

QLabel* make_value_label(const QString& text) {
    auto* label = new QLabel{text};
    label->setTextFormat(Qt::PlainText);
    label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    label->setWordWrap(true);
    return label;
}

} // namespace

MainWindow::MainWindow(
    AppStateViewModel state,
    AuthenticationFunction authenticate,
    Localization* localization,
    SynchronizationFunction synchronize,
    MonitoringFunction monitor
)
    : state_{std::move(state)},
      localization_{localization},
      login_{std::move(authenticate)},
      synchronization_{std::move(synchronize), nullptr, std::move(monitor)} {
    setWindowTitle(tr("onedrive client"));
    resize(860, 520);

    auto* tabs = new QTabWidget{this};
    tabs_ = tabs;
    tabs_->setObjectName("mainTabs");
    auto* overview = new QWidget{tabs};
    overview->setObjectName("overviewPage");
    auto* overview_layout = new QFormLayout{overview};
    overview_layout_ = overview_layout;
    account_status_value_ = make_value_label({});
    config_file_value_ = make_value_label({});
    sync_directory_value_ = make_value_label({});
    state_directory_value_ = make_value_label({});
    overview_layout->addRow(tr("Account"), account_status_value_);
    overview_layout->addRow(tr("Configuration file"), config_file_value_);
    overview_layout->addRow(tr("Sync directory"), sync_directory_value_);
    overview_layout->addRow(tr("State directory"), state_directory_value_);
    login_status_ =
        make_value_label(tr("Sign in to connect a OneDrive account."));
    login_status_->setObjectName("loginStatus");
    login_url_ = make_value_label({});
    overview_layout->addRow(tr("Login"), login_status_);
    overview_layout->addRow(tr("Browser login"), login_url_);
    auto* login_actions = new QHBoxLayout;
    login_button_ = new QPushButton{tr("Sign in"), overview};
    login_button_->setObjectName("signInButton");
    account_action_button_ = new QPushButton{tr("Log out"), overview};
    account_action_button_->setObjectName("accountActionButton");
    login_actions->addWidget(login_button_);
    login_actions->addWidget(account_action_button_);
    login_actions->addStretch();
    overview_layout->addRow(login_actions);
    startup_status_ = make_value_label({});
    startup_status_->setObjectName("startupStatus");
    overview_layout->addRow(startup_status_);
    overview_sync_status_ = make_value_label({});
    overview_sync_status_->setObjectName("overviewSyncStatus");
    overview_layout->addRow(tr("Synchronization"), overview_sync_status_);
    tabs->addTab(overview, tr("Overview"));

    sync_page_ = new QWidget{tabs};
    sync_page_->setObjectName("syncPage");
    auto* sync_page_layout = new QVBoxLayout{sync_page_};
    sync_group_ = new QGroupBox{sync_page_};
    sync_group_->setObjectName("synchronizationGroup");
    auto* sync_layout = new QVBoxLayout{sync_group_};
    auto* sync_actions = new QHBoxLayout;
    preview_button_ = new QPushButton{sync_group_};
    preview_button_->setObjectName("previewSyncButton");
    sync_button_ = new QPushButton{sync_group_};
    sync_button_->setObjectName("startSyncButton");
    monitor_button_ = new QPushButton{sync_group_};
    monitor_button_->setObjectName("startMonitorButton");
    stop_sync_button_ = new QPushButton{sync_group_};
    stop_sync_button_->setObjectName("stopSyncButton");
    sync_actions->addWidget(preview_button_);
    sync_actions->addWidget(sync_button_);
    sync_actions->addWidget(monitor_button_);
    sync_actions->addWidget(stop_sync_button_);
    sync_actions->addStretch();
    sync_layout->addLayout(sync_actions);
    sync_status_ = make_value_label({});
    sync_status_->setObjectName("syncStatus");
    sync_layout->addWidget(sync_status_);
    monitor_status_ = make_value_label({});
    monitor_status_->setObjectName("monitorStatus");
    sync_layout->addWidget(monitor_status_);
    sync_context_ = make_value_label({});
    sync_context_->setObjectName("syncContext");
    sync_layout->addWidget(sync_context_);
    sync_progress_ = new QProgressBar{sync_group_};
    sync_progress_->setObjectName("syncProgress");
    sync_progress_->setAlignment(Qt::AlignCenter);
    sync_progress_->setStyleSheet(
        "QProgressBar#syncProgress::chunk { background-color: #0078d4; }"
    );
    sync_layout->addWidget(sync_progress_);
    sync_progress_hint_ = make_value_label({});
    sync_progress_hint_->setObjectName("syncProgressHint");
    sync_layout->addWidget(sync_progress_hint_);
    transfer_status_ = make_value_label({});
    transfer_status_->setObjectName("transferStatus");
    sync_layout->addWidget(transfer_status_);
    upload_progress_ = new QProgressBar{sync_group_};
    upload_progress_->setObjectName("uploadProgress");
    upload_progress_->setAlignment(Qt::AlignCenter);
    upload_progress_->setStyleSheet(
        "QProgressBar#uploadProgress::chunk { background-color: #0078d4; }"
    );
    sync_layout->addWidget(upload_progress_);
    upload_status_ = make_value_label({});
    upload_status_->setObjectName("uploadStatus");
    sync_layout->addWidget(upload_status_);
    sync_summary_status_ = make_value_label({});
    sync_summary_status_->setObjectName("syncSummary");
    sync_layout->addWidget(sync_summary_status_);
    sync_page_layout->addWidget(sync_group_);
    blocked_group_ = new QGroupBox{sync_page_};
    blocked_group_->setObjectName("blockedItemsGroup");
    auto* blocked_layout = new QVBoxLayout{blocked_group_};
    auto* blocked_actions = new QHBoxLayout;
    blocked_filter_ = new QLineEdit{blocked_group_};
    blocked_filter_->setObjectName("blockedItemsFilter");
    blocked_reason_ = new QComboBox{blocked_group_};
    blocked_reason_->setObjectName("blockedItemsReason");
    blocked_refresh_ = new QPushButton{blocked_group_};
    blocked_refresh_->setObjectName("refreshBlockedItemsButton");
    blocked_actions->addWidget(blocked_filter_, 1);
    blocked_actions->addWidget(blocked_reason_);
    blocked_actions->addWidget(blocked_refresh_);
    blocked_layout->addLayout(blocked_actions);
    blocked_status_ = make_value_label({});
    blocked_status_->setObjectName("blockedItemsStatus");
    blocked_layout->addWidget(blocked_status_);
    blocked_table_ = new QTreeWidget{blocked_group_};
    blocked_table_->setObjectName("blockedItemsTable");
    blocked_table_->setColumnCount(2);
    blocked_table_->setRootIsDecorated(false);
    blocked_table_->setSelectionMode(QAbstractItemView::SingleSelection);
    blocked_table_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    blocked_table_->header()->setSectionResizeMode(
        1, QHeaderView::ResizeToContents
    );
    blocked_layout->addWidget(blocked_table_, 1);
    blocked_details_ = new QPlainTextEdit{blocked_group_};
    blocked_details_->setObjectName("blockedItemsDetails");
    blocked_details_->setReadOnly(true);
    blocked_details_->setMaximumHeight(130);
    blocked_layout->addWidget(blocked_details_);
    sync_page_layout->addWidget(blocked_group_, 1);
    connect(
        blocked_filter_,
        &QLineEdit::textChanged,
        this,
        &MainWindow::render_blocked_items
    );
    connect(
        blocked_reason_,
        &QComboBox::currentIndexChanged,
        this,
        &MainWindow::render_blocked_items
    );
    connect(
        blocked_table_,
        &QTreeWidget::itemSelectionChanged,
        this,
        &MainWindow::render_blocked_details
    );
    connect(
        blocked_refresh_,
        &QPushButton::clicked,
        this,
        &MainWindow::refresh_blocked_items
    );
    connect(
        &synchronization_,
        &SyncController::blocked_items_ready,
        this,
        [this](
            const std::vector<storage::BlockedItem>& items, const QString& error
        ) {
            blocked_items_ = items;
            blocked_error_ = error;
            const QSignalBlocker blocker{blocked_reason_};
            const auto selected = blocked_reason_->currentData().toString();
            blocked_reason_->clear();
            blocked_reason_->addItem(tr("All reasons"), QString{});
            for (const auto& item : items) {
                const auto code = QString::fromStdString(item.reason_code);
                if (blocked_reason_->findData(code) < 0) {
                    blocked_reason_->addItem(code, code);
                }
            }
            const auto index = blocked_reason_->findData(selected);
            blocked_reason_->setCurrentIndex(index < 0 ? 0 : index);
            render_blocked_items();
        }
    );
    tabs->addTab(sync_page_, tr("Sync"));

    logs_page_ = new QWidget{tabs};
    logs_page_->setObjectName("logsPage");
    auto* logs_layout = new QVBoxLayout{logs_page_};
    auto* log_actions = new QHBoxLayout;
    log_level_ = new QComboBox{logs_page_};
    log_level_->setObjectName("logLevelFilter");
    log_level_->addItem({}, -1);
    for (const auto kind :
         {events::MessageKind::trace,
          events::MessageKind::debug,
          events::MessageKind::information,
          events::MessageKind::warning,
          events::MessageKind::error,
          events::MessageKind::critical}) {
        log_level_->addItem({}, static_cast<int>(kind));
    }
    log_actions->addWidget(log_level_);
    log_autoscroll_ = new QCheckBox{logs_page_};
    log_autoscroll_->setObjectName("logAutoScroll");
    log_autoscroll_->setChecked(true);
    log_actions->addWidget(log_autoscroll_);
    log_actions->addStretch();
    copy_logs_button_ = new QPushButton{logs_page_};
    copy_logs_button_->setObjectName("copyLogsButton");
    clear_logs_button_ = new QPushButton{logs_page_};
    clear_logs_button_->setObjectName("clearLogsButton");
    log_actions->addWidget(copy_logs_button_);
    log_actions->addWidget(clear_logs_button_);
    logs_layout->addLayout(log_actions);
    log_messages_ = new QPlainTextEdit{logs_page_};
    log_messages_->setObjectName("logMessages");
    log_messages_->setReadOnly(true);
    log_messages_->setUndoRedoEnabled(false);
    logs_layout->addWidget(log_messages_);
    tabs->addTab(logs_page_, tr("Logs"));

    auto* settings = new QWidget{tabs};
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
    tabs->addTab(settings, tr("Settings"));

    setCentralWidget(tabs);
    if (localization_) {
        language_menu_ = menuBar()->addMenu(tr("Language"));
        language_menu_->setObjectName("languageMenu");
        language_actions_ = new QActionGroup{this};
        language_actions_->setExclusive(true);
        for (const auto language :
             {Language::automatic,
              Language::english,
              Language::simplified_chinese}) {
            auto* action = language_menu_->addAction(QString{});
            action->setCheckable(true);
            action->setData(static_cast<int>(language));
            language_actions_->addAction(action);
            connect(action, &QAction::triggered, this, [this, language] {
                localization_->set_language(language);
                retranslate_ui();
            });
        }
    }
    help_menu_ = menuBar()->addMenu(tr("Help"));
    help_menu_->setObjectName("helpMenu");
    about_action_ = help_menu_->addAction(tr("About"));
    about_action_->setObjectName("aboutAction");
    connect(about_action_, &QAction::triggered, this, &MainWindow::show_about);
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
    connect(
        login_button_, &QPushButton::clicked, this, &MainWindow::start_login
    );
    connect(account_action_button_, &QPushButton::clicked, this, [this] {
        if (login_.running()) {
            cancel_login();
        } else {
            logout();
        }
    });
    connect(preview_button_, &QPushButton::clicked, this, [this] {
        start_sync(true);
    });
    connect(sync_button_, &QPushButton::clicked, this, [this] {
        start_sync(false);
    });
    connect(
        monitor_button_,
        &QPushButton::clicked,
        this,
        &MainWindow::start_monitoring
    );
    connect(
        &monitor_,
        &MonitorController::changed,
        this,
        &MainWindow::monitor_changed
    );
    connect(
        &monitor_,
        &MonitorController::acknowledged,
        this,
        [this](
            monitor::ControlCommand command, const QString& acknowledgement
        ) {
            if (command == monitor::ControlCommand::stop) {
                sync_stopping_ = true;
                sync_phase_ = SyncPhase::stopping;
            }
            log_message(
                LogOperation::monitoring,
                events::MessageKind::information,
                acknowledgement == "merged"
                    ? QT_TR_NOOP(
                          "Monitor request merged; this does not mean "
                          "synchronization or shutdown is complete."
                      )
                    : QT_TR_NOOP(
                          "Monitor request accepted; this does not mean "
                          "synchronization or shutdown is complete."
                      )
            );
            refresh_actions();
            render_sync();
        }
    );
    connect(
        &monitor_,
        &MonitorController::request_failed,
        this,
        [this](const QString& error) {
            if (!synchronization_.running()) {
                sync_stopping_ = false;
            }
            log_message(
                LogOperation::monitoring,
                events::MessageKind::error,
                QT_TR_NOOP("Monitor request failed: %1"),
                error
            );
            refresh_actions();
            render_sync();
        }
    );
    monitor_timer_ = new QTimer{this};
    monitor_timer_->setInterval(1000);
    connect(
        monitor_timer_, &QTimer::timeout, this, &MainWindow::refresh_monitor
    );
    monitor_timer_->start();
    connect(
        stop_sync_button_, &QPushButton::clicked, this, &MainWindow::stop_sync
    );
    connect(
        log_level_,
        &QComboBox::currentIndexChanged,
        this,
        &MainWindow::render_logs
    );
    connect(log_autoscroll_, &QCheckBox::toggled, this, [this](bool enabled) {
        if (enabled) {
            auto* scroll = log_messages_->verticalScrollBar();
            scroll->setValue(scroll->maximum());
        }
    });
    connect(copy_logs_button_, &QPushButton::clicked, this, [this] {
        QApplication::clipboard()->setText(log_messages_->toPlainText());
    });
    connect(clear_logs_button_, &QPushButton::clicked, this, [this] {
        log_history_.clear();
        log_omitted_ = 0;
        render_logs();
    });
    connect(
        &synchronization_,
        &SyncController::started,
        this,
        [this](bool preview) {
            sync_preview_ = preview;
            if (!sync_stopping_) {
                sync_phase_ = SyncPhase::working;
            }
            log_message(
                sync_monitoring_ ? LogOperation::monitoring
                : preview        ? LogOperation::preview
                                 : LogOperation::synchronization,
                events::MessageKind::information,
                sync_monitoring_
                    ? QT_TR_NOOP("Starting GUI-owned monitoring...")
                : preview ? QT_TR_NOOP("Previewing changes...")
                          : QT_TR_NOOP("Synchronizing...")
            );
            render_sync();
        },
        Qt::QueuedConnection
    );
    connect(
        &synchronization_,
        &SyncController::events_ready,
        this,
        &MainWindow::receive_sync_events
    );
    connect(
        &synchronization_,
        &SyncController::finished,
        this,
        [this](int result, const QString& error, bool preview) {
            sync_preview_ = preview;
            sync_result_ = result;
            sync_error_ = error;
            sync_stopping_ = false;
            if (!sync_monitoring_) {
                sync_finished_at_ = QDateTime::currentDateTime();
            }
            sync_phase_ = !error.isEmpty()               ? SyncPhase::failed
                          : result == exit_code::success ? SyncPhase::completed
                          : result == exit_code::cancelled
                              ? SyncPhase::cancelled
                          : result == exit_code::blocked ? SyncPhase::blocked
                                                         : SyncPhase::failed;
            if (sync_monitoring_) {
                log_message(
                    LogOperation::monitoring,
                    error.isEmpty() && (result == exit_code::success ||
                                        result == exit_code::cancelled)
                        ? events::MessageKind::information
                        : events::MessageKind::error,
                    !error.isEmpty() ? QT_TR_NOOP("Monitoring failed: %1")
                    : result == exit_code::success ||
                            result == exit_code::cancelled
                        ? QT_TR_NOOP("Monitoring stopped.")
                        : QT_TR_NOOP("Monitoring failed (exit code %1)."),
                    !error.isEmpty() ? error
                    : result == exit_code::success ||
                            result == exit_code::cancelled
                        ? QString{}
                        : QString::number(result)
                );
                monitor_.set_state_directory(state_.state_directory);
                refresh_monitor();
            } else {
                log_sync_result();
            }
            refresh_actions();
            render_sync();
            if (!closing_) {
                refresh_blocked_items();
            }
            if (closing_) {
                close();
            }
        }
    );
    connect(
        &login_,
        &LoginController::browser_requested,
        this,
        [this](const QString& url) {
            if (cancelling_ || closing_) {
                return;
            }
            login_url_->setText(
                tr("Complete authorization in your system browser.")
            );
            login_url_is_instruction_ = true;
            authorization_url_ = url;
            set_login_status(QT_TR_NOOP(
                "Waiting for browser authorization (up to 5 minutes)..."
            ));
            open_login_url();
        },
        Qt::QueuedConnection
    );
    connect(
        &login_,
        &LoginController::finished,
        this,
        [this](const QString& error, bool cancelled) {
            login_url_->clear();
            login_url_is_instruction_ = false;
            authorization_url_.clear();
            refresh_actions();
            settings_->setEnabled(true);
            if (cancelled) {
                set_login_status(QT_TR_NOOP("Login cancelled."));
            } else if (!error.isEmpty()) {
                set_login_status(
                    QT_TR_NOOP("Login failed: %1"),
                    error,
                    events::MessageKind::error
                );
            } else if (closing_) {
                set_login_status(
                    QT_TR_NOOP("Signed in. Account credentials saved."),
                    {},
                    events::MessageKind::information
                );
            } else {
                try {
                    state_ = load_app_state_view_model(state_.config_file);
                    refresh_settings(true);
                    if (state_.can_sign_in()) {
                        set_login_status(
                            QT_TR_NOOP("Signed in. Account credentials saved."),
                            {},
                            events::MessageKind::information
                        );
                    } else {
                        const auto& detail = !state_.configuration_error.empty()
                                                 ? state_.configuration_error
                                             : !state_.account_error.empty()
                                                 ? state_.account_error
                                                 : state_.authentication_error;
                        set_login_status(
                            QT_TR_NOOP(
                                "Login completed, but account status could not "
                                "be "
                                "refreshed: %1"
                            ),
                            QString::fromStdString(detail),
                            events::MessageKind::error
                        );
                    }
                } catch (const std::exception& exception) {
                    set_login_status(
                        QT_TR_NOOP(
                            "Login completed, but account status could not be "
                            "refreshed: %1"
                        ),
                        QString::fromUtf8(exception.what()),
                        events::MessageKind::error
                    );
                }
            }
            if (closing_) {
                close();
            }
        }
    );
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
    help_menu_->setTitle(tr("Help"));
    about_action_->setText(tr("About"));
    QApplication::setApplicationDisplayName(windowTitle());
    tabs_->setTabText(0, tr("Overview"));
    tabs_->setTabText(1, tr("Sync"));
    tabs_->setTabText(2, tr("Logs"));
    tabs_->setTabText(3, tr("Settings"));
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
    if (language_menu_) {
        language_menu_->setTitle(tr("Language"));
        for (auto* action : language_actions_->actions()) {
            const auto language = static_cast<Language>(action->data().toInt());
            switch (language) {
            case Language::automatic:
                action->setText(tr("System language"));
                break;
            case Language::english:
                action->setText(tr("English"));
                break;
            case Language::simplified_chinese:
                action->setText(tr("Simplified Chinese"));
                break;
            }
            action->setChecked(localization_->language() == language);
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

void MainWindow::set_login_status(
    const char* source,
    const QString& detail,
    events::MessageKind kind,
    bool record
) {
    login_status_source_ = source;
    login_status_detail_ = detail;
    auto text = tr(source);
    if (!detail.isNull()) {
        text = text.arg(detail);
    }
    login_status_->setText(text);
    if (record) {
        log_message(LogOperation::account, kind, source, detail);
    }
}

void MainWindow::start_sync(bool preview) {
    if (!preview && monitor_.status()) {
        const auto& reply = *monitor_.status();
        if (reply.state == "waiting" && !reply.pending && !sync_stopping_ &&
            monitor_.error().isEmpty() && !monitor_.busy()) {
            request_monitor(monitor::ControlCommand::trigger, reply.target());
        }
        return;
    }
    if (monitor_busy()) {
        return;
    }
    start_task(preview, false);
}

void MainWindow::start_monitoring() {
    if (monitor_busy()) {
        return;
    }
    start_task(false, true);
}

void MainWindow::start_task(bool preview, bool monitoring) {
    if (login_.running() || synchronization_.running() || monitor_busy() ||
        monitor_.uncertain() || !state_.can_synchronize()) {
        return;
    }
    sync_preview_ = preview;
    sync_monitoring_ = monitoring;
    had_monitor_ = false;
    attached_monitor_instance_.clear();
    sync_stopping_ = false;
    sync_phase_ = SyncPhase::preparing;
    sync_error_.clear();
    sync_delta_.reset();
    sync_download_.reset();
    sync_upload_.reset();
    sync_stage_.reset();
    sync_summary_.reset();
    log_message(
        monitoring ? LogOperation::monitoring
        : preview  ? LogOperation::preview
                   : LogOperation::synchronization,
        events::MessageKind::information,
        monitoring ? QT_TR_NOOP("Starting GUI-owned monitoring...")
        : preview  ? QT_TR_NOOP("Preparing preview...")
                   : QT_TR_NOOP("Preparing synchronization...")
    );
    try {
        if (monitoring) {
            synchronization_.start_monitoring(state_.config_file);
        } else {
            synchronization_.start(state_.config_file, preview);
        }
    } catch (const std::exception& error) {
        sync_phase_ = SyncPhase::failed;
        sync_error_ = QString::fromUtf8(error.what());
        sync_result_ = exit_code::failure;
        if (monitoring) {
            log_message(
                LogOperation::monitoring,
                events::MessageKind::error,
                QT_TR_NOOP("Monitoring failed: %1"),
                sync_error_
            );
        } else {
            log_sync_result();
        }
    }
    refresh_actions();
    render_sync();
}

void MainWindow::stop_sync() {
    if (!synchronization_.running() && monitor_.status()) {
        const auto reply = *monitor_.status();
        if (reply.state == "stopping" || sync_stopping_ || monitor_.busy()) {
            return;
        }
        const auto confirmed = QMessageBox::question(
            this,
            tr("Stop external Monitor?"),
            tr("Stop Monitor PID %1 for %2? It is not owned by this window. "
               "Closing the window alone will leave it running.")
                .arg(
                    QString::number(reply.pid),
                    QString::fromStdString(reply.sync_directory.string())
                ),
            QMessageBox::Yes | QMessageBox::No,
            QMessageBox::No
        );
        if (confirmed == QMessageBox::Yes) {
            request_monitor(monitor::ControlCommand::stop, reply.target());
        }
        return;
    }
    if (!synchronization_.running() || sync_stopping_) {
        return;
    }
    sync_stopping_ = true;
    sync_phase_ = SyncPhase::stopping;
    log_message(
        sync_monitoring_ ? LogOperation::monitoring
        : sync_preview_  ? LogOperation::preview
                         : LogOperation::synchronization,
        events::MessageKind::information,
        QT_TR_NOOP("Stopping; waiting for the task to finish...")
    );
    synchronization_.cancel();
    refresh_actions();
    render_sync();
}

bool MainWindow::monitor_busy() const {
    return monitor_.status().has_value() || monitor_.owner().has_value() ||
           (monitor_.uncertain() && !monitor_.runtime_free());
}

void MainWindow::refresh_monitor() {
    if (!closing_ && !login_.running() &&
        state_.configuration_status == ConfigurationStatus::valid) {
        monitor_.refresh();
    }
}

void MainWindow::request_monitor(
    monitor::ControlCommand command, monitor::ControlTarget target
) {
    try {
        monitor_.request(command, std::move(target));
    } catch (const std::exception& error) {
        log_message(
            LogOperation::monitoring,
            events::MessageKind::error,
            QT_TR_NOOP("Monitor request failed: %1"),
            QString::fromUtf8(error.what())
        );
    }
}

void MainWindow::monitor_changed() {
    const bool owns_monitor = synchronization_.running() && sync_monitoring_;
    const bool attached = monitor_.status().has_value();
    const bool monitor_owner =
        monitor_.owner() && monitor_.owner()->operation == "monitor";
    if (!owns_monitor && !synchronization_.running() &&
        (attached || monitor_owner)) {
        if (!had_monitor_ || (attached && attached_monitor_instance_ !=
                                              monitor_.status()->instance_id)) {
            sync_delta_.reset();
            sync_download_.reset();
            sync_upload_.reset();
            sync_stage_.reset();
            sync_summary_.reset();
            sync_finished_at_ = {};
            if (attached) {
                log_message(
                    LogOperation::monitoring,
                    events::MessageKind::information,
                    QT_TR_NOOP(
                        "Connected to an external Monitor. Closing this window "
                        "will not stop it."
                    )
                );
            }
        }
        sync_monitoring_ = true;
        sync_preview_ = false;
        if (attached) {
            attached_monitor_instance_ = monitor_.status()->instance_id;
            const auto& reply = *monitor_.status();
            sync_phase_ = reply.state == "waiting"    ? SyncPhase::waiting
                          : reply.state == "stopping" ? SyncPhase::stopping
                          : reply.state == "starting" ? SyncPhase::preparing
                                                      : SyncPhase::working;
        } else {
            sync_phase_ =
                sync_stopping_ ? SyncPhase::stopping : SyncPhase::preparing;
        }
    }
    if (had_monitor_ && !owns_monitor && !monitor_busy() &&
        !synchronization_.running()) {
        attached_monitor_instance_.clear();
        sync_phase_ = SyncPhase::completed;
        sync_stopping_ = false;
        log_message(
            LogOperation::monitoring,
            events::MessageKind::information,
            QT_TR_NOOP("Monitor stopped; runtime resources are available.")
        );
    }
    had_monitor_ = ((attached || monitor_owner) && !owns_monitor) ||
                   (had_monitor_ && monitor_busy());
    if (!owns_monitor && !synchronization_.running() &&
        !monitor_.error().isEmpty()) {
        sync_phase_ = SyncPhase::monitor_unavailable;
    }
    if (last_monitor_error_ != monitor_.error()) {
        last_monitor_error_ = monitor_.error();
        if (!last_monitor_error_.isEmpty()) {
            log_message(
                LogOperation::monitoring,
                events::MessageKind::error,
                QT_TR_NOOP("Monitor status unavailable: %1"),
                last_monitor_error_
            );
        }
    }
    refresh_actions();
    render_sync();
}

void MainWindow::receive_sync_events(
    const std::vector<events::Event>& batch, std::size_t omitted
) {
    log_omitted_ += omitted;
    for (const auto& event : batch) {
        // This marks a terminal newline, not completion of a download batch.
        if (std::holds_alternative<events::EndDownloadProgressEvent>(event) ||
            std::holds_alternative<events::EndUploadProgressEvent>(event)) {
            continue;
        }
        if (const auto* phase = std::get_if<events::SyncPhaseEvent>(&event)) {
            sync_stage_ = phase->phase;
            if (!sync_stopping_) {
                switch (phase->phase) {
                case events::SyncPhase::preparing:
                    if (sync_monitoring_) {
                        sync_delta_.reset();
                        sync_download_.reset();
                        sync_upload_.reset();
                        sync_summary_.reset();
                    }
                    sync_phase_ = SyncPhase::preparing;
                    break;
                case events::SyncPhase::scanning:
                    if (!sync_delta_) {
                        sync_delta_ = events::DeltaProgressEvent{
                            0, 0, util::ProgressState::ongoing
                        };
                    }
                    sync_phase_ = SyncPhase::scanning;
                    break;
                case events::SyncPhase::scanning_local:
                    sync_phase_ = SyncPhase::scanning_local;
                    break;
                case events::SyncPhase::downloading:
                    sync_phase_ = SyncPhase::transferring;
                    break;
                case events::SyncPhase::uploading:
                    sync_phase_ = SyncPhase::uploading;
                    break;
                case events::SyncPhase::waiting:
                    if (sync_monitoring_) {
                        sync_phase_ = SyncPhase::waiting;
                        sync_finished_at_ = QDateTime::currentDateTime();
                    }
                    break;
                default:
                    sync_phase_ = SyncPhase::working;
                    break;
                }
            }
            continue;
        }
        if (const auto* context =
                std::get_if<events::SyncContextEvent>(&event)) {
            state_.sync_mode = context->direction;
            sync_preview_ = context->preview;
            continue;
        }
        if (const auto* summary =
                std::get_if<events::SyncSummaryEvent>(&event)) {
            sync_summary_ = *summary;
            continue;
        }
        if (const auto* upload =
                std::get_if<events::UploadProgressEvent>(&event)) {
            sync_upload_ = *upload;
            if (!sync_stopping_) {
                sync_phase_ = upload->state == util::ProgressState::ongoing
                                  ? SyncPhase::uploading
                                  : SyncPhase::working;
            }
            continue;
        }
        if (const auto* delta =
                std::get_if<events::DeltaProgressEvent>(&event)) {
            sync_delta_ = *delta;
            if (!sync_stopping_) {
                if (delta->state == util::ProgressState::ongoing) {
                    sync_download_.reset();
                    sync_phase_ = SyncPhase::scanning;
                } else {
                    sync_phase_ = SyncPhase::working;
                }
            }
        } else if (const auto* download =
                       std::get_if<events::DownloadProgressEvent>(&event)) {
            sync_download_ = *download;
            if (!sync_stopping_) {
                sync_phase_ = download->state == util::ProgressState::ongoing
                                  ? SyncPhase::transferring
                                  : SyncPhase::working;
            }
        } else if (const auto* state =
                       std::get_if<events::OperationStateEvent>(&event)) {
            if (!sync_stopping_ &&
                state->operation == events::OperationKind::synchronization &&
                state->state == events::OperationState::syncing) {
                sync_phase_ = SyncPhase::working;
            }
        } else {
            if (std::holds_alternative<events::DeltaSummaryEvent>(event) &&
                sync_phase_ == SyncPhase::scanning && !sync_stopping_) {
                sync_phase_ = SyncPhase::working;
            }
            append_log_event(
                sync_monitoring_ ? LogOperation::monitoring
                : sync_preview_  ? LogOperation::preview
                                 : LogOperation::synchronization,
                event
            );
        }
    }
    render_sync();
    refresh_actions();
    render_logs();
    if (std::ranges::any_of(batch, [](const auto& event) {
            if (std::holds_alternative<events::BlockedItemEvent>(event)) {
                return true;
            }
            const auto* message = std::get_if<events::MessageEvent>(&event);
            return message && message->kind == events::MessageKind::warning;
        })) {
        refresh_blocked_items();
    }
}

void MainWindow::refresh_blocked_items() {
    if (closing_) {
        return;
    }
    synchronization_.refresh_blocked_items(state_.config_file);
}

void MainWindow::render_blocked_items() {
    const auto* current = blocked_table_->currentItem();
    const auto selected =
        current ? current->data(0, Qt::UserRole).toString() : QString{};
    const QSignalBlocker blocker{blocked_table_};
    blocked_table_->clear();
    const auto search = blocked_filter_->text();
    const auto reason = blocked_reason_->currentData().toString().toStdString();
    const auto reason_filter =
        reason.empty() ? std::nullopt : std::optional<std::string_view>{reason};
    const auto text_matches = [&search](const std::string& text) {
        return QString::fromStdString(text).contains(
            search, Qt::CaseInsensitive
        );
    };
    for (const auto& item : blocked_items_) {
        if (!app::matches_blocked_item(item, reason_filter, text_matches)) {
            continue;
        }
        const auto path = QString::fromStdString(item.remote_path);
        const auto code = QString::fromStdString(item.reason_code);
        const auto message = QString::fromStdString(item.reason_message);
        auto* row = new QTreeWidgetItem{blocked_table_, {path, code}};
        const auto id = QString::fromStdString(item.remote_id);
        row->setData(0, Qt::UserRole, id);
        row->setToolTip(0, path);
        row->setToolTip(1, message);
        if (id == selected) {
            blocked_table_->setCurrentItem(row);
        }
    }
    blocked_group_->setTitle(
        tr("Blocked items (%1)").arg(blocked_items_.size())
    );
    blocked_status_->setText(
        !blocked_error_.isEmpty()
            ? tr("Cannot load blocked items: %1").arg(blocked_error_)
        : blocked_items_.empty()
            ? tr("No saved blocked items.")
            : tr("%1 of %2 items shown. Discoveries from previews, sync and "
                 "monitoring are saved here.")
                  .arg(blocked_table_->topLevelItemCount())
                  .arg(blocked_items_.size())
    );
    render_blocked_details();
}

void MainWindow::render_blocked_details() {
    const auto* row = blocked_table_->currentItem();
    if (!row) {
        blocked_details_->clear();
        blocked_details_->setPlaceholderText(
            tr("Select an item to view and copy its details.")
        );
        return;
    }
    const auto id = row->data(0, Qt::UserRole).toString().toStdString();
    const auto item =
        std::ranges::find(blocked_items_, id, &storage::BlockedItem::remote_id);
    if (item == blocked_items_.end()) {
        return;
    }
    blocked_details_->setPlainText(
        tr("Path: %1\nReason: %2\n%3\nItem ID: %4\nRecorded discoveries: %5")
            .arg(
                QString::fromStdString(item->remote_path),
                QString::fromStdString(item->reason_code),
                QString::fromStdString(item->reason_message),
                QString::fromStdString(item->remote_id)
            )
            .arg(item->attempt_count)
    );
    blocked_details_->appendPlainText(
        tr("Policy: %1").arg(blocked_policy_text(*item))
    );
}

QString
MainWindow::blocked_policy_text(const storage::BlockedItem& item) const {
    const auto& traits = sync::blocked_item_strategy(item.reason_code).traits();
    switch (traits.control) {
    case sync::BlockedPolicyControl::local_conflict_configuration:
        return tr(
            "Uses the configured local-conflict policy; changes are applied "
            "only after validation."
        );
    case sync::BlockedPolicyControl::remote_deletion_configuration:
        return tr(
            "Uses the configured remote-deletion safeguard; it is never "
            "bypassed automatically."
        );
    case sync::BlockedPolicyControl::fixed:
        break;
    }
    switch (traits.kind) {
    case sync::BlockedPolicyKind::unsupported_format:
        return tr(
            "Fixed policy: skip unsupported content and protect local copies. "
            "This policy cannot be changed."
        );
    case sync::BlockedPolicyKind::safety:
        return tr("Fixed safety policy: unsafe operations cannot be forced.");
    case sync::BlockedPolicyKind::resource:
        return tr(
            "Retry after resources recover; clear only after verified "
            "resolution."
        );
    case sync::BlockedPolicyKind::conflict:
    case sync::BlockedPolicyKind::remote_deletion:
        return tr(
            "Revalidate the conflict before retrying; unsafe operations cannot "
            "be forced."
        );
    case sync::BlockedPolicyKind::unknown:
        return tr(
            "Unrecognized reason: keep blocked, with no automatic retry or "
            "override."
        );
    }
    Q_UNREACHABLE();
}

QString MainWindow::log_event_text(const events::Event& event) const {
    if (const auto* message = std::get_if<events::MessageEvent>(&event)) {
        if (message->event == "pending_delta_resumed") {
            return tr(
                "Restoring saved remote changes; checking only subsequent "
                "changes and rebuilding the local plan..."
            );
        }
        if (message->event == "pending_delta_invalidated") {
            return tr(
                "Saved remote changes no longer match the account, filters, "
                "hash policy, unsupported formats or applied cursor; fetching "
                "remote changes again..."
            );
        }
        if (message->event == "sync_filter_changed") {
            return tr(
                "Synchronization filters or unsupported-format rules changed; "
                "fetching the full remote state..."
            );
        }
        if (message->event == "pending_delta_hierarchy_changed") {
            return tr(
                "Remote directory changes or unidentified deletions may "
                "affect saved paths; fetching the full remote state..."
            );
        }
        return QString::fromStdString(message->text);
    } else if (const auto* section =
                   std::get_if<events::SectionEvent>(&event)) {
        auto text = QString::fromStdString(section->title);
        for (const auto& field : section->fields) {
            const auto label = field.key == "unsupported_items"
                                   ? tr("unsupported format items:")
                                   : QString::fromStdString(field.label);
            text += "\n" + label + " " + QString::fromStdString(field.value);
        }
        return text;
    } else if (const auto* summary =
                   std::get_if<events::DeltaSummaryEvent>(&event)) {
        return tr("Remote changes: %1 files, %2 folders, %3 deletions.")
            .arg(
                number(summary->summary.files),
                number(summary->summary.directories),
                number(summary->summary.deletions)
            );
    } else if (const auto* blocked =
                   std::get_if<events::BlockedItemEvent>(&event)) {
        if (sync::blocked_item_strategy(blocked->reason_code).traits().kind ==
            sync::BlockedPolicyKind::unsupported_format) {
            return tr("Unsupported format: %1 [%2]\n"
                      "Skipped; existing local files are preserved.")
                .arg(
                    QString::fromStdString(blocked->path),
                    QString::fromStdString(blocked->reason_code)
                );
        }
        return tr("Blocked: %1 [%2]\n%3")
            .arg(
                QString::fromStdString(blocked->path),
                QString::fromStdString(blocked->reason_code),
                QString::fromStdString(blocked->reason_message)
            );
    }
    throw std::logic_error{"non-message event in synchronization history"};
}

void MainWindow::render_sync() {
    const auto locale = locale_for_language(
        localization_ ? localization_->language() : Language::automatic
    );
    const auto direction = state_.sync_mode == sync::SyncMode::upload_only
                               ? tr("Upload only")
                           : state_.sync_mode == sync::SyncMode::download_only
                               ? tr("Download only")
                               : tr("Bidirectional");
    const bool external =
        !(synchronization_.running() && sync_monitoring_) &&
        (!attached_monitor_instance_.empty() ||
         (monitor_.owner() && monitor_.owner()->operation == "monitor"));
    sync_context_->setText(
        external ? tr("Direction: not available from external Monitor; mode: "
                      "Automatic monitoring")
        : sync_monitoring_
            ? tr("Direction: %1; mode: Automatic monitoring").arg(direction)
            : tr("Direction: %1; mode: One-time").arg(direction)
    );
    QString monitor_text =
        monitor_.uncertain()
            ? tr("Monitor status unavailable. The task may still be running.")
            : tr("Monitor: not running.");
    if (monitor_.status()) {
        const auto& reply = *monitor_.status();
        monitor_text =
            tr("%1 Monitor: PID %2; directory: %3; completed rounds: %4; "
               "pending request: %5; last result: %6.")
                .arg(
                    external ? tr("External") : tr("GUI-owned"),
                    locale.toString(static_cast<qlonglong>(reply.pid)),
                    QString::fromStdString(reply.sync_directory.string()),
                    locale.toString(
                        static_cast<qulonglong>(reply.completed_rounds)
                    ),
                    reply.pending ? tr("Yes") : tr("No"),
                    reply.last_result ? QString::number(*reply.last_result)
                                      : tr("None")
                );
        if (external) {
            monitor_text +=
                tr(" External Monitor provides status snapshots, not live "
                   "transfer progress.");
        }
    } else if (monitor_.owner()) {
        monitor_text =
            tr("Runtime resources are busy: PID %1; operation: %2. Waiting for "
               "startup or cleanup; controls are unavailable.")
                .arg(
                    QString::number(monitor_.owner()->pid),
                    QString::fromStdString(monitor_.owner()->operation)
                );
    } else if (synchronization_.running() && sync_monitoring_) {
        monitor_text = tr("Starting GUI-owned monitoring...");
    }
    if (!monitor_.error().isEmpty()) {
        monitor_text += " " + tr("Monitor status unavailable: %1")
                                  .arg(short_detail(monitor_.error()));
    }
    monitor_status_->setText(monitor_text);
    QString status;
    switch (sync_phase_) {
    case SyncPhase::idle:
        status = state_.can_synchronize()
                     ? tr("Ready. Tasks use saved settings.")
                     : tr("Sign in and save valid settings to synchronize.");
        break;
    case SyncPhase::preparing:
        status = sync_preview_ ? tr("Preparing preview...")
                               : tr("Preparing synchronization...");
        break;
    case SyncPhase::working:
        status = sync_preview_ ? tr("Previewing changes...")
                               : tr("Synchronizing...");
        if (!sync_preview_ && sync_stage_ == events::SyncPhase::applying) {
            status = tr("Applying changes...");
        }
        break;
    case SyncPhase::scanning:
        status =
            tr("Scanning remote changes: %1 pages, %2 items.")
                .arg(
                    locale.toString(
                        static_cast<qulonglong>(sync_delta_->pages)
                    ),
                    locale.toString(static_cast<qulonglong>(sync_delta_->items))
                );
        break;
    case SyncPhase::scanning_local:
        status = tr("Scanning local changes...");
        break;
    case SyncPhase::transferring:
        status = tr("Downloading files...");
        break;
    case SyncPhase::uploading:
        status = tr("Uploading files...");
        break;
    case SyncPhase::waiting:
        status = tr("Monitoring; waiting for changes.");
        if (monitor_.status() && monitor_.status()->last_result &&
            *monitor_.status()->last_result != exit_code::success) {
            status +=
                *monitor_.status()->last_result == exit_code::blocked
                    ? " " +
                          tr("Last round incomplete: some items were blocked.")
                    : " " + tr("Last round failed or was cancelled (exit code "
                               "%1). Monitoring continues.")
                                .arg(*monitor_.status()->last_result);
        }
        break;
    case SyncPhase::monitor_unavailable:
        status =
            tr("Monitor status unavailable. The task may still be running.");
        break;
    case SyncPhase::stopping:
        status = tr("Stopping; waiting for the task to finish...");
        break;
    case SyncPhase::completed:
        status =
            sync_monitoring_ ? tr("Monitoring stopped.")
            : sync_preview_
                ? tr("Preview completed. No synchronized files were changed.")
                : tr("Synchronization completed.");
        break;
    case SyncPhase::cancelled:
        status = sync_monitoring_ ? tr("Monitoring stopped.")
                 : sync_preview_  ? tr("Preview cancelled.")
                                  : tr("Synchronization cancelled.");
        break;
    case SyncPhase::blocked:
        status =
            sync_preview_
                ? tr("Preview incomplete: some items were blocked.")
                : tr("Synchronization incomplete: some items were blocked.");
        break;
    case SyncPhase::failed:
        if (sync_monitoring_) {
            status =
                !sync_error_.isEmpty()
                    ? tr("Monitoring failed: %1").arg(short_detail(sync_error_))
                    : tr("Monitoring failed (exit code %1).").arg(sync_result_);
            break;
        }
        if (!sync_error_.isEmpty()) {
            status = (sync_preview_ ? tr("Preview failed: %1")
                                    : tr("Synchronization failed: %1"))
                         .arg(short_detail(sync_error_));
        } else {
            status =
                (sync_preview_
                     ? tr("Preview failed (exit code %1). See logs for "
                          "details.")
                     : tr("Synchronization failed (exit code %1). See details "
                          "in logs."))
                    .arg(sync_result_);
        }
        break;
    }
    sync_status_->setText(status);
    overview_sync_status_->setText(status);
    QString transfer;
    int percentage = 0;
    if (sync_download_) {
        const auto& download = *sync_download_;
        percentage = static_cast<int>(events::download_progress_percentage(
            download.completed_files,
            download.file_count,
            download.downloaded,
            download.total,
            download.state
        ));
        transfer = tr("Downloads: %1 / %2 files; %3 / %4; %5/s")
                       .arg(
                           locale.toString(
                               static_cast<qulonglong>(download.completed_files)
                           ),
                           locale.toString(
                               static_cast<qulonglong>(download.file_count)
                           ),
                           data_size(download.downloaded, locale),
                           data_size(download.total, locale),
                           data_size(download.metrics.bytes_per_second, locale)
                       );
        if (download.metrics.estimated_seconds_remaining &&
            synchronization_.running() &&
            sync_phase_ == SyncPhase::transferring) {
            transfer +=
                tr("; about %1 remaining")
                    .arg(duration(
                        *download.metrics.estimated_seconds_remaining, locale
                    ));
        }
    }
    transfer_status_->setText(transfer);
    transfer_status_->setVisible(!transfer.isEmpty());
    QString upload_text;
    const bool measurable_upload =
        sync_upload_ &&
        (sync_upload_->total != 0 || sync_upload_->file_count != 0 ||
         sync_upload_->state == util::ProgressState::completed);
    if (sync_upload_) {
        const auto& upload = *sync_upload_;
        upload_text =
            tr("Uploads: %1 / %2 files; %3 / %4; %5/s")
                .arg(
                    locale.toString(
                        static_cast<qulonglong>(upload.completed_files)
                    ),
                    locale.toString(static_cast<qulonglong>(upload.file_count)),
                    data_size(upload.uploaded, locale),
                    data_size(upload.total, locale),
                    data_size(upload.metrics.bytes_per_second, locale)
                );
        if (upload.metrics.estimated_seconds_remaining &&
            synchronization_.running() && sync_phase_ == SyncPhase::uploading) {
            upload_text +=
                tr("; about %1 remaining")
                    .arg(duration(
                        *upload.metrics.estimated_seconds_remaining, locale
                    ));
        }
        upload_progress_->setRange(0, 100);
        upload_progress_->setValue(
            static_cast<int>(events::download_progress_percentage(
                upload.completed_files,
                upload.file_count,
                upload.uploaded,
                upload.total,
                upload.state
            ))
        );
    }
    upload_progress_->setFormat("%p%");
    upload_progress_->setAccessibleName(tr("Upload progress"));
    upload_progress_->setVisible(
        measurable_upload &&
        !(sync_monitoring_ && (sync_phase_ == SyncPhase::waiting ||
                               sync_phase_ == SyncPhase::completed))
    );
    upload_status_->setText(upload_text);
    upload_status_->setVisible(!upload_text.isEmpty());
    QString summary;
    if (sync_summary_) {
        summary = tr("Round totals: %1 downloaded, %2 uploaded, %3 moved, %4 "
                     "deleted, %5 blocked.")
                      .arg(
                          number(sync_summary_->downloaded),
                          number(sync_summary_->uploaded),
                          number(sync_summary_->moved),
                          number(sync_summary_->deleted),
                          number(sync_summary_->blocked)
                      );
    }
    if (sync_finished_at_.isValid()) {
        summary +=
            tr(" Last finished: %1.")
                .arg(locale.toString(sync_finished_at_, QLocale::ShortFormat));
    }
    sync_summary_status_->setText(summary);
    sync_summary_status_->setVisible(!summary.isEmpty());
    const bool measurable_download =
        sync_download_ &&
        (sync_download_->total != 0 || sync_download_->file_count != 0 ||
         sync_download_->state == util::ProgressState::completed);
    const bool show_download =
        measurable_download &&
        (sync_phase_ == SyncPhase::transferring ||
         sync_phase_ == SyncPhase::stopping ||
         sync_phase_ == SyncPhase::cancelled ||
         sync_phase_ == SyncPhase::blocked || sync_phase_ == SyncPhase::failed);
    const bool show_progress =
        (!sync_monitoring_ && sync_phase_ == SyncPhase::completed) ||
        show_download;
    sync_progress_->setRange(0, 100);
    sync_progress_->setValue(
        sync_phase_ == SyncPhase::completed ? 100 : percentage
    );
    sync_progress_->setFormat("%p%");
    sync_progress_->setAccessibleName(
        show_download ? tr("Download progress") : tr("Synchronization progress")
    );
    sync_progress_->setVisible(show_progress);
    QString hint;
    if (synchronization_.running() && sync_phase_ != SyncPhase::waiting &&
        !show_progress &&
        !(sync_phase_ == SyncPhase::uploading && measurable_upload)) {
        if (sync_phase_ == SyncPhase::scanning) {
            hint = tr(
                "Remote totals are unknown; a percentage cannot be calculated."
            );
        } else if (sync_phase_ == SyncPhase::transferring) {
            hint =
                tr("Download totals are unknown; a percentage cannot be "
                   "calculated.");
        } else if (sync_phase_ == SyncPhase::uploading && !measurable_upload) {
            hint = tr(
                "Upload totals are unknown; a percentage cannot be calculated."
            );
        } else {
            hint = tr(
                "This phase has no percentage progress. See logs for details."
            );
        }
    }
    sync_progress_hint_->setText(hint);
    sync_progress_hint_->setVisible(!hint.isEmpty());
}

void MainWindow::append_log_event(
    LogOperation operation,
    const events::Event& event,
    const char* source,
    const QString& detail
) {
    const auto* message = std::get_if<events::MessageEvent>(&event);
    const auto kind = message ? message->kind
                      : std::holds_alternative<events::BlockedItemEvent>(event)
                          ? events::MessageKind::warning
                          : events::MessageKind::information;
    if (log_history_.size() == maximum_log_history) {
        log_history_.pop_front();
        ++log_omitted_;
    }
    log_history_.push_back(
        {next_log_sequence_++,
         QDateTime::currentDateTime(),
         operation,
         kind,
         event,
         source,
         detail}
    );
}

void MainWindow::log_message(
    LogOperation operation,
    events::MessageKind kind,
    const char* source,
    const QString& detail
) {
    append_log_event(
        operation, events::MessageEvent{kind, "gui", {}}, source, detail
    );
    render_logs();
}

void MainWindow::log_sync_result() {
    const auto operation =
        sync_preview_ ? LogOperation::preview : LogOperation::synchronization;
    switch (sync_phase_) {
    case SyncPhase::completed:
        log_message(
            operation,
            events::MessageKind::information,
            sync_preview_
                ? QT_TR_NOOP(
                      "Preview completed. No synchronized files were changed."
                  )
                : QT_TR_NOOP("Synchronization completed.")
        );
        break;
    case SyncPhase::cancelled:
        log_message(
            operation,
            events::MessageKind::information,
            sync_preview_ ? QT_TR_NOOP("Preview cancelled.")
                          : QT_TR_NOOP("Synchronization cancelled.")
        );
        break;
    case SyncPhase::blocked:
        log_message(
            operation,
            events::MessageKind::warning,
            sync_preview_
                ? QT_TR_NOOP("Preview incomplete: some items were blocked.")
                : QT_TR_NOOP(
                      "Synchronization incomplete: some items were blocked."
                  )
        );
        break;
    case SyncPhase::failed:
        if (!sync_error_.isEmpty()) {
            log_message(
                operation,
                events::MessageKind::error,
                sync_preview_ ? QT_TR_NOOP("Preview failed: %1")
                              : QT_TR_NOOP("Synchronization failed: %1"),
                sync_error_
            );
        } else {
            log_message(
                operation,
                events::MessageKind::error,
                sync_preview_
                    ? QT_TR_NOOP(
                          "Preview failed (exit code %1). See logs for details."
                      )
                    : QT_TR_NOOP(
                          "Synchronization failed (exit code %1). See details "
                          "in logs."
                      ),
                QString::number(sync_result_)
            );
        }
        break;
    default:
        throw std::logic_error{
            "synchronization result logged before completion"
        };
    }
}

void MainWindow::log_startup_errors() {
    if (state_.configuration_status == ConfigurationStatus::missing) {
        log_message(
            LogOperation::configuration,
            events::MessageKind::warning,
            QT_TR_NOOP(
                "Configuration file is missing. Using defaults; complete setup "
                "in Settings."
            )
        );
    } else if (state_.configuration_status == ConfigurationStatus::invalid) {
        log_message(
            LogOperation::configuration,
            events::MessageKind::error,
            QT_TR_NOOP(
                "Configuration could not be loaded:\n%1\n"
                "Import a valid TOML file, or repair this file and reload. "
                "The original file has not been changed."
            ),
            QString::fromStdString(state_.configuration_error)
        );
    }
    if (!state_.authentication_error.empty()) {
        log_message(
            LogOperation::account,
            events::MessageKind::error,
            QT_TR_NOOP("Authentication configuration needs attention:\n%1"),
            QString::fromStdString(state_.authentication_error)
        );
    }
    if (!state_.account_error.empty()) {
        log_message(
            LogOperation::account,
            events::MessageKind::error,
            QT_TR_NOOP(
                "Local account credentials could not be read:\n%1\n"
                "Repair the state directory or select another one in Settings, "
                "then reload."
            ),
            QString::fromStdString(state_.account_error)
        );
    }
    if (!state_.sync_error.empty()) {
        log_message(
            LogOperation::configuration,
            events::MessageKind::warning,
            QT_TR_NOOP(
                "Sync directory needs attention before synchronization:\n%1"
            ),
            QString::fromStdString(state_.sync_error)
        );
    }
}

void MainWindow::render_logs() {
    QStringList messages;
    QList<std::optional<std::size_t>> sequences;
    if (log_omitted_ != 0) {
        messages << tr("Earlier events omitted: %1").arg(number(log_omitted_));
        sequences.push_back(std::nullopt);
    }
    const int filter = log_level_->currentData().toInt();
    for (const auto& entry : log_history_) {
        if (filter != -1 && static_cast<int>(entry.kind) < filter) {
            continue;
        }
        auto text =
            entry.source ? tr(entry.source) : log_event_text(entry.event);
        if (entry.source && !entry.detail.isNull()) {
            text = text.arg(entry.detail);
        }
        switch (entry.kind) {
        case events::MessageKind::trace:
            text = tr("Trace: %1").arg(text);
            break;
        case events::MessageKind::debug:
            text = tr("Debug: %1").arg(text);
            break;
        case events::MessageKind::information:
            text = tr("Info: %1").arg(text);
            break;
        case events::MessageKind::warning:
            text = tr("Warning: %1").arg(text);
            break;
        case events::MessageKind::error:
            text = tr("Error: %1").arg(text);
            break;
        case events::MessageKind::critical:
            text = tr("Critical: %1").arg(text);
            break;
        }
        QString operation;
        switch (entry.operation) {
        case LogOperation::application:
            operation = tr("Application");
            break;
        case LogOperation::account:
            operation = tr("Account");
            break;
        case LogOperation::configuration:
            operation = tr("Configuration");
            break;
        case LogOperation::preview:
            operation = tr("Preview");
            break;
        case LogOperation::synchronization:
            operation = tr("Sync");
            break;
        case LogOperation::monitoring:
            operation = tr("Monitoring");
            break;
        }
        messages << QString{"[%1] [%2] %3"}.arg(
            entry.timestamp.toString("yyyy-MM-dd HH:mm:ss.zzz"), operation, text
        );
        sequences.push_back(entry.sequence);
    }
    const auto content = messages.join("\n");
    if (log_messages_->toPlainText() != content) {
        auto* scroll = log_messages_->verticalScrollBar();
        const auto position = scroll->value();
        const auto visible =
            log_messages_->cursorForPosition(QPoint{0, 0}).block();
        const auto line_offset = position - visible.firstLineNumber();
        auto header = visible;
        while (header.isValid() && !header.userData()) {
            header = header.previous();
        }
        const auto* data = header.isValid()
                               ? dynamic_cast<LogBlockData*>(header.userData())
                               : nullptr;
        const auto anchor = data ? std::optional{data->sequence} : std::nullopt;
        const auto block_offset = visible.blockNumber() - header.blockNumber();
        const auto horizontal = log_messages_->horizontalScrollBar()->value();
        log_messages_->clear();
        QTextCursor cursor{log_messages_->document()};
        cursor.beginEditBlock();
        QTextBlock restored;
        for (qsizetype index = 0; index < messages.size(); ++index) {
            if (index != 0) {
                cursor.insertBlock();
            }
            auto block = cursor.block();
            cursor.insertText(messages[index]);
            if (sequences[index]) {
                block.setUserData(new LogBlockData{*sequences[index]});
                if (anchor == sequences[index]) {
                    restored = block;
                }
            }
        }
        cursor.endEditBlock();
        if (log_autoscroll_->isChecked()) {
            scroll->setValue(scroll->maximum());
        } else if (restored.isValid()) {
            // History eviction changes line numbers; follow the retained event.
            for (int offset = 0;
                 offset < block_offset && restored.next().isValid() &&
                 !restored.next().userData();
                 ++offset) {
                restored = restored.next();
            }
            log_messages_->setTextCursor(QTextCursor{restored});
            log_messages_->ensureCursorVisible();
            scroll->setValue(restored.firstLineNumber() + line_offset);
        } else {
            scroll->setValue(anchor ? 0 : position);
        }
        log_messages_->horizontalScrollBar()->setValue(horizontal);
    }
    copy_logs_button_->setEnabled(!content.isEmpty());
    clear_logs_button_->setEnabled(!log_history_.empty() || log_omitted_ != 0);
}

void MainWindow::start_login() {
    if (!state_.can_sign_in() ||
        state_.account_status == AccountStatus::credentials_available ||
        login_.running() || synchronization_.running() || monitor_busy()) {
        return;
    }
    cancelling_ = false;
    settings_->setEnabled(false);
    login_button_->setEnabled(false);
    set_login_status(QT_TR_NOOP("Starting browser login..."));
    try {
        synchronization_.invalidate_preview();
        login_.start(state_.config_file);
        refresh_actions();
    } catch (const std::exception& error) {
        settings_->setEnabled(true);
        refresh_actions();
        set_login_status(
            QT_TR_NOOP("Cannot start login: %1"),
            QString::fromUtf8(error.what()),
            events::MessageKind::error
        );
    }
}

void MainWindow::cancel_login() {
    cancelling_ = true;
    login_url_->clear();
    login_url_is_instruction_ = false;
    authorization_url_.clear();
    refresh_actions();
    set_login_status(QT_TR_NOOP("Cancelling login..."));
    login_.cancel();
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

void MainWindow::logout() {
    if (login_.running() || synchronization_.running() || monitor_busy() ||
        state_.account_status != AccountStatus::credentials_available) {
        return;
    }
    try {
        bool missing = false;
        const auto config = load_configuration(state_.config_file, missing);
        const app::ProductionRuntimeFactory production;
        const app::RuntimeFactory factory{util::borrowed_proxy, production};
        const bool removed = app::logout_account(config, factory);
        synchronization_.invalidate_preview();
        state_ = load_app_state_view_model(state_.config_file);
        refresh_settings(true);
        set_login_status(
            removed ? QT_TR_NOOP("Signed out. Saved credentials removed.")
                    : QT_TR_NOOP("No saved authentication was present."),
            {},
            events::MessageKind::information
        );
    } catch (const std::exception& error) {
        set_login_status(
            QT_TR_NOOP("Cannot log out: %1"),
            QString::fromUtf8(error.what()),
            events::MessageKind::error
        );
    }
}

void MainWindow::open_login_url() {
    const QUrl url{authorization_url_, QUrl::StrictMode};
    if (!url.isValid() || url.scheme() != "https" || url.host().isEmpty() ||
        !url.userInfo().isEmpty()) {
        set_login_status(
            QT_TR_NOOP(
                "Cannot open browser: the login URL is not a valid HTTPS "
                "address."
            ),
            {},
            events::MessageKind::error
        );
        login_.cancel();
        return;
    }
    if (!QDesktopServices::openUrl(url)) {
        set_login_status(
            QT_TR_NOOP(
                "Cannot open the system browser. Copy the login URL below into "
                "your browser."
            ),
            {},
            events::MessageKind::warning
        );
        login_url_->setText(authorization_url_);
        login_url_is_instruction_ = false;
    }
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

void MainWindow::refresh_settings(bool preserve_edits) {
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
