#pragma once

#include "app_state_view_model.hpp"

#include <QMainWindow>

class QLabel;

namespace onedrive::gui {

class MainWindow final : public QMainWindow {
public:
    explicit MainWindow(AppStateViewModel state);

private:
    QLabel* config_file_value_{};
    QLabel* account_status_value_{};
    QLabel* sync_directory_value_{};
    QLabel* state_directory_value_{};
};

}  // namespace onedrive::gui
