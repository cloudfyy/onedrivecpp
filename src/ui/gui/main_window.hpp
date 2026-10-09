#pragma once

#include "app_state_view_model.hpp"

#include <QMainWindow>

class QLabel;
class QLineEdit;

namespace onedrive::gui {

class MainWindow final : public QMainWindow {
public:
    explicit MainWindow(AppStateViewModel state);

private:
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
};

} // namespace onedrive::gui
