#pragma once

#include "app_state_view_model.hpp"
#include "login_controller.hpp"
#include "sync_controller.hpp"
#include "monitor_controller.hpp"

#include <QMainWindow>
#include <QDateTime>
#include <deque>
#include <optional>

class QLabel;
class QLocale;
class QLineEdit;
class QPushButton;
class QCloseEvent;
class QEvent;
class QFormLayout;
class QTabWidget;
class QGroupBox;
class QProgressBar;
class QPlainTextEdit;
class QComboBox;
class QCheckBox;
class QTreeWidget;
class QTreeWidgetItem;
class QTimer;

namespace onedrive::gui {

class Localization;
class SettingsEditor;

[[nodiscard]] QIcon application_icon();

class MainWindow final : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(
        AppStateViewModel state,
        AuthenticationFunction authenticate = {},
        Localization* localization = nullptr,
        SynchronizationFunction synchronize = {},
        MonitoringFunction monitor = {}
    );

private:
    void create_overview_page();
    void create_sync_page();
    void create_logs_page();
    void create_files_page();
    void create_settings_page();
    void create_header_controls();
    void connect_sync_controller();
    void connect_login_controller();
    void closeEvent(QCloseEvent* event) override;
    void changeEvent(QEvent* event) override;
    void retranslate_ui();
    void set_login_status(
        const char* source,
        const QString& detail = {},
        events::MessageKind kind = events::MessageKind::information,
        bool record = true
    );
    void start_login();
    void cancel_login();
    void logout();
    void refresh_actions();
    void start_sync(bool preview);
    void start_monitoring();
    void start_task(bool preview, bool monitoring);
    void monitor_changed();
    void refresh_monitor();
    void request_monitor(
        monitor::ControlCommand command, monitor::ControlTarget target
    );
    [[nodiscard]] bool monitor_busy() const;
    void stop_sync();
    void receive_sync_events(
        const std::vector<events::Event>& batch, std::size_t omitted
    );
    void render_sync();
    void render_monitor_status(const QLocale& locale);
    void render_sync_status(const QLocale& locale);
    int render_download_status(const QLocale& locale);
    bool render_download_progress(int percentage);
    bool render_upload_progress(const QLocale& locale);
    void render_sync_summary(const QLocale& locale);
    void render_progress_hint(bool show_progress, bool measurable_upload);
    QString log_event_text(const events::Event& event) const;
    enum class LogOperation {
        application,
        account,
        configuration,
        preview,
        synchronization,
        monitoring
    };
    void append_log_event(
        LogOperation operation,
        const events::Event& event,
        const char* source = nullptr,
        const QString& detail = {}
    );
    void log_message(
        LogOperation operation,
        events::MessageKind kind,
        const char* source,
        const QString& detail = {}
    );
    void log_sync_result();
    void log_startup_errors();
    void render_logs();
    void render_file_history();
    void render_file_history_status();
    void render_file_history_row(QTreeWidgetItem* row);
    void filter_file_history();
    QString file_action_text(
        events::FileAction action, bool directory, bool recovered
    ) const;
    void refresh_blocked_items();
    void render_blocked_items();
    void render_blocked_details();
    [[nodiscard]] QString
    blocked_policy_text(const storage::BlockedItem& item) const;
    void open_login_url();
    void refresh_settings(bool preserve_edits = false);
    void refresh_startup_status();
    [[nodiscard]] bool has_unsaved_settings() const;
    [[nodiscard]] bool confirm_unsaved_settings();
    void reload_settings();
    bool save_settings(bool show_confirmation = true);
    void import_settings();
    void show_about();

    AppStateViewModel state_;
    Localization* localization_{};
    QTabWidget* tabs_{};
    QWidget* sync_page_{};
    QWidget* logs_page_{};
    QWidget* files_page_{};
    QTreeWidget* files_table_{};
    QLineEdit* files_filter_{};
    QLabel* files_status_{};
    QLabel* files_hint_{};
    std::size_t files_omitted_{};
    std::size_t files_limit_{config::default_file_history_limit};
    QFormLayout* overview_layout_{};
    QFormLayout* settings_layout_{};
    SettingsEditor* settings_editor_{};
    QLabel* theme_value_{};
    QPushButton* import_button_{};
    QPushButton* save_button_{};
    QPushButton* reload_button_{};
    QLabel* startup_status_{};
    QLabel* settings_status_{};
    QLabel* language_label_{};
    QComboBox* language_selector_{};
    QPushButton* about_button_{};
    QLabel* config_file_value_{};
    QLabel* account_status_value_{};
    QLabel* drive_type_value_{};
    QLabel* sync_directory_value_{};
    QLabel* state_directory_value_{};
    QLineEdit* sync_directory_edit_{};
    QLineEdit* state_directory_edit_{};
    LoginController login_;
    SyncController synchronization_;
    MonitorController monitor_;
    QTimer* monitor_timer_{};
    QWidget* settings_{};
    QPushButton* login_button_{};
    QPushButton* account_action_button_{};
    QLabel* login_status_{};
    QLabel* login_url_{};
    const char* login_status_source_{
        QT_TR_NOOP("Sign in to connect a OneDrive account.")
    };
    QString login_status_detail_;
    bool login_url_is_instruction_{false};
    QString authorization_url_;
    bool cancelling_{false};
    bool closing_{false};

    enum class SyncPhase {
        idle,
        preparing,
        working,
        scanning,
        scanning_local,
        transferring,
        uploading,
        waiting,
        monitor_unavailable,
        stopping,
        completed,
        cancelled,
        blocked,
        failed
    };
    SyncPhase sync_phase_{SyncPhase::idle};
    bool sync_preview_{};
    bool sync_monitoring_{};
    bool had_monitor_{};
    std::string attached_monitor_instance_;
    QString last_monitor_error_;
    bool sync_stopping_{};
    int sync_result_{};
    QString sync_error_;
    std::optional<events::DeltaProgressEvent> sync_delta_;
    std::optional<events::DownloadProgressEvent> sync_download_;
    std::optional<events::UploadProgressEvent> sync_upload_;
    std::optional<events::SyncPhase> sync_stage_;
    std::optional<events::SyncSummaryEvent> sync_summary_;
    std::optional<events::SyncSummaryEvent> monitor_totals_;
    QDateTime sync_finished_at_;
    struct LogEntry {
        std::size_t sequence;
        QDateTime timestamp;
        LogOperation operation;
        events::MessageKind kind;
        events::Event event;
        const char* source{};
        QString detail;
    };
    std::deque<LogEntry> log_history_;
    std::size_t next_log_sequence_{};
    std::size_t log_omitted_{};
    QComboBox* log_level_{};
    QCheckBox* log_autoscroll_{};
    QPushButton* copy_logs_button_{};
    QPushButton* clear_logs_button_{};
    QLabel* overview_sync_status_{};
    QGroupBox* sync_group_{};
    QPushButton* preview_button_{};
    QPushButton* sync_button_{};
    QPushButton* monitor_button_{};
    QPushButton* stop_sync_button_{};
    QLabel* sync_status_{};
    QLabel* monitor_status_{};
    QLabel* transfer_status_{};
    QLabel* upload_status_{};
    QLabel* sync_context_{};
    QLabel* sync_summary_status_{};
    QLabel* monitor_totals_status_{};
    QProgressBar* upload_progress_{};
    QProgressBar* sync_progress_{};
    QLabel* sync_progress_hint_{};
    QPlainTextEdit* log_messages_{};
    QGroupBox* blocked_group_{};
    QLineEdit* blocked_filter_{};
    QComboBox* blocked_reason_{};
    QPushButton* blocked_refresh_{};
    QLabel* blocked_status_{};
    QTreeWidget* blocked_table_{};
    QPlainTextEdit* blocked_details_{};
    std::vector<storage::BlockedItem> blocked_items_;
    QString blocked_error_;
};

} // namespace onedrive::gui
