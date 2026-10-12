#pragma once

#include "app_state_view_model.hpp"
#include "login_controller.hpp"
#include "sync_controller.hpp"

#include <QMainWindow>
#include <deque>
#include <optional>

class QLabel;
class QLineEdit;
class QPushButton;
class QCloseEvent;
class QEvent;
class QFormLayout;
class QTabWidget;
class QMenu;
class QActionGroup;
class QAction;
class QGroupBox;
class QProgressBar;
class QPlainTextEdit;

namespace onedrive::gui {

class Localization;
class SettingsEditor;

class MainWindow final : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(
        AppStateViewModel state,
        AuthenticationFunction authenticate = {},
        Localization* localization = nullptr,
        SynchronizationFunction synchronize = {}
    );

private:
    void closeEvent(QCloseEvent* event) override;
    void changeEvent(QEvent* event) override;
    void retranslate_ui();
    void set_login_status(const char* source, const QString& detail = {});
    void start_login();
    void cancel_login();
    void logout();
    void refresh_actions();
    void start_sync(bool preview);
    void stop_sync();
    void receive_sync_events(
        const std::vector<events::Event>& batch, std::size_t omitted
    );
    void render_sync();
    QString sync_event_text(const events::Event& event) const;
    void open_login_url();
    void refresh_settings(bool preserve_edits = false);
    void refresh_startup_status();
    void reload_settings();
    void save_settings();
    void import_settings();
    void show_about();

    AppStateViewModel state_;
    Localization* localization_{};
    QTabWidget* tabs_{};
    QFormLayout* overview_layout_{};
    QFormLayout* settings_layout_{};
    SettingsEditor* settings_editor_{};
    QLabel* theme_value_{};
    QPushButton* import_button_{};
    QPushButton* save_button_{};
    QPushButton* reload_button_{};
    QLabel* startup_status_{};
    QLabel* settings_status_{};
    QMenu* language_menu_{};
    QActionGroup* language_actions_{};
    QMenu* help_menu_{};
    QAction* about_action_{};
    QLabel* config_file_value_{};
    QLabel* account_status_value_{};
    QLabel* sync_directory_value_{};
    QLabel* state_directory_value_{};
    QLineEdit* sync_directory_edit_{};
    QLineEdit* state_directory_edit_{};
    LoginController login_;
    SyncController synchronization_;
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
        transferring,
        stopping,
        completed,
        cancelled,
        blocked,
        failed
    };
    SyncPhase sync_phase_{SyncPhase::idle};
    bool sync_preview_{};
    bool sync_stopping_{};
    int sync_result_{};
    QString sync_error_;
    std::optional<events::DeltaProgressEvent> sync_delta_;
    std::optional<events::DownloadProgressEvent> sync_download_;
    std::deque<events::Event> sync_history_;
    std::size_t sync_omitted_{};
    QGroupBox* sync_group_{};
    QPushButton* preview_button_{};
    QPushButton* sync_button_{};
    QPushButton* stop_sync_button_{};
    QLabel* sync_status_{};
    QLabel* transfer_status_{};
    QProgressBar* sync_progress_{};
    QPlainTextEdit* sync_messages_{};
};

} // namespace onedrive::gui
