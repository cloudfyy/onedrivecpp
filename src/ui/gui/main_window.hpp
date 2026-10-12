#pragma once

#include "app_state_view_model.hpp"
#include "login_controller.hpp"

#include <QMainWindow>

class QLabel;
class QLineEdit;
class QPushButton;
class QCloseEvent;
class QEvent;
class QFormLayout;
class QTabWidget;
class QMenu;
class QActionGroup;

namespace onedrive::gui {

class Localization;

class MainWindow final : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(
        AppStateViewModel state,
        AuthenticationFunction authenticate = {},
        Localization* localization = nullptr
    );

private:
    void closeEvent(QCloseEvent* event) override;
    void changeEvent(QEvent* event) override;
    void retranslate_ui();
    void set_login_status(const char* source, const QString& detail = {});
    void start_login();
    void cancel_login();
    void open_login_url();
    void refresh_settings();
    void browse_for_directory(QLineEdit* field);
    void save_settings();
    void import_settings();

    AppStateViewModel state_;
    Localization* localization_{};
    QTabWidget* tabs_{};
    QFormLayout* overview_layout_{};
    QFormLayout* settings_layout_{};
    QLabel* theme_value_{};
    QPushButton* import_button_{};
    QPushButton* save_button_{};
    QMenu* language_menu_{};
    QActionGroup* language_actions_{};
    QLabel* config_file_value_{};
    QLabel* account_status_value_{};
    QLabel* sync_directory_value_{};
    QLabel* state_directory_value_{};
    QLineEdit* sync_directory_edit_{};
    QLineEdit* state_directory_edit_{};
    LoginController login_;
    QWidget* settings_{};
    QPushButton* login_button_{};
    QPushButton* cancel_login_button_{};
    QPushButton* browser_button_{};
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
};

} // namespace onedrive::gui
