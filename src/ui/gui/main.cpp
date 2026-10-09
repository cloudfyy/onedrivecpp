#include "main_window.hpp"
#include "system_appearance.hpp"

#include "onedrive/config/config.hpp"

#include <QApplication>
#include <QMessageBox>

#include <exception>

int main(int argc, char* argv[]) {
    QApplication application{argc, argv};
    QCoreApplication::setApplicationName("OneDrive C++");
    onedrive::gui::SystemAppearance system_appearance{&application};

    try {
        onedrive::gui::MainWindow window{
            onedrive::gui::load_app_state_view_model(
                onedrive::config::default_config_path()
            )
        };
        window.show();
        return application.exec();
    } catch (const std::exception& error) {
        QMessageBox::critical(
            nullptr, "OneDrive C++", QString::fromUtf8(error.what())
        );
        return 1;
    }
}
