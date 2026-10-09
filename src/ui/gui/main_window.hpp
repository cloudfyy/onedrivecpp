#pragma once

#include "app_state_view_model.hpp"
#include "login_controller.hpp"

#include <QMainWindow>

class QLabel;
class QLineEdit;
class QPushButton;
class QCloseEvent;

namespace onedrive::gui {

class MainWindow final : public QMainWindow {
public:
    explicit MainWindow(
        AppStateViewModel state, AuthenticationFunction authenticate = {}
    );

private:
    void closeEvent(QCloseEvent* event) override;
    void start_login();
    void cancel_login();
    void open_login_url();
    void refresh_settings();
    void browse_for_directory(QLineEdit* field);
    void save_settings();
    void import_settings();

    AppStateViewModel state_;
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
    QString authorization_url_;
    bool cancelling_{false};
    bool closing_{false};
};

} // namespace onedrive::gui
