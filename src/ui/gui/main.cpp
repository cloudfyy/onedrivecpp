#include "main_window.hpp"
#include "system_appearance.hpp"
#include "localization.hpp"

#include "onedrive/config/config.hpp"
#include "onedrive/exit_code.hpp"

#include <QApplication>
#include <QMessageBox>

#include <exception>

int main(int argc, char* argv[]) {
    QApplication application{argc, argv};
    QCoreApplication::setApplicationName("onedrive-cpp-gui");
    onedrive::gui::Localization localization;
    QApplication::setApplicationDisplayName(
        QCoreApplication::translate(
            "onedrive::gui::MainWindow", "onedrive client"
        )
    );
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
            nullptr,
            QApplication::applicationDisplayName(),
            QCoreApplication::translate(
                "Application", "Cannot start OneDrive client:\n%1"
            )
                .arg(QString::fromUtf8(error.what()))
        );
        return onedrive::exit_code::failure;
    }
}
