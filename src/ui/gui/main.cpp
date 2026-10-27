#include "main_window.hpp"
#include "system_appearance.hpp"
#include "localization.hpp"

#include "onedrive/config/config.hpp"
#include "onedrive/exit_code.hpp"
#include "onedrive/version.hpp"

#include <QApplication>
#include <QIcon>
#include <QMessageBox>
#include <QTextStream>

#include <exception>
#include <stdexcept>

int main(int argc, char* argv[]) {
    // Qt must register the desktop ID before any portal settings requests.
    QGuiApplication::setDesktopFileName("onedrive-cpp-gui");
    QApplication application{argc, argv};
    QCoreApplication::setApplicationName("onedrive-cpp-gui");
    QCoreApplication::setApplicationVersion(onedrive::build_info::version);
    QApplication::setWindowIcon(onedrive::gui::application_icon());
    QCommandLineParser parser;
    onedrive::gui::Language language{onedrive::gui::Language::automatic};
    try {
        language = onedrive::gui::parse_startup_language(
            parser, application.arguments()
        );
    } catch (const std::invalid_argument& error) {
        QTextStream{stderr} << QCoreApplication::applicationName() << ": "
                            << error.what() << '\n';
        return onedrive::exit_code::failure;
    }
    if (parser.isSet("help") || parser.isSet("help-all")) {
        parser.showHelp();
    }
    if (parser.isSet("version")) {
        parser.showVersion();
    }
    onedrive::gui::Localization localization{language};
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
            ),
            {},
            &localization
        };
        window.show_at_startup(parser.isSet("start-in-tray"));
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
