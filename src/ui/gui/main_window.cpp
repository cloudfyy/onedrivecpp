#include "main_window.hpp"

#include <QFormLayout>
#include <QLabel>
#include <QWidget>

namespace onedrive::gui {
namespace {

QLabel* make_value_label(const QString& text) {
    auto* label = new QLabel{text};
    label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    label->setWordWrap(true);
    return label;
}

}  // namespace

MainWindow::MainWindow(AppStateViewModel state)
    : config_file_value_{make_value_label(
          QString::fromStdString(state.config_file.string())
      )},
      account_status_value_{
          make_value_label(QString::fromStdString(state.account_status))
      },
      sync_directory_value_{make_value_label(
          QString::fromStdString(state.sync_directory.string())
      )},
      state_directory_value_{make_value_label(
          QString::fromStdString(state.state_directory.string())
      )} {
    setWindowTitle("OneDrive C++");
    resize(760, 360);

    auto* content = new QWidget{this};
    auto* layout = new QFormLayout{content};
    layout->addRow("Account", account_status_value_);
    layout->addRow("Configuration file", config_file_value_);
    layout->addRow("Sync directory", sync_directory_value_);
    layout->addRow("State directory", state_directory_value_);
    setCentralWidget(content);
}

}  // namespace onedrive::gui
