#include "localization_support.hpp"
#include "localization_cases.hpp"

using namespace onedrive;
using namespace onedrive::test::gui_localization;

int main(int argc, char* argv[]) {
    test::TemporaryDirectory settings;
    QSettings::setPath(
        QSettings::NativeFormat,
        QSettings::UserScope,
        QString::fromStdString(settings.path().string())
    );
    QApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
    QApplication application{argc, argv};
    const bool chinese = argc > 1 && std::string_view{argv[1]} == "chinese";
    try {
        {
            auto arguments = application.arguments();
            if (arguments.size() > 1) {
                arguments.removeAt(1);
            }
            QCommandLineParser parser;
            gui::Localization localization{
                gui::parse_startup_language(parser, arguments)
            };
            require(
                localization.chinese_loaded() == chinese,
                "startup locale selected the wrong translation"
            );
            test_states_and_messages(chinese);
            test_window_and_settings(chinese);
            test_login(chinese);
            test_browser_callback(chinese);
        }
        require(
            gui::MainWindow::tr("onedrive client") == "onedrive client",
            "destroyed translator remained installed"
        );
        {
            gui::Localization localization{QLocale{"zh_CN"}};
            require(
                localization.chinese_loaded() &&
                    gui::MainWindow::tr("onedrive client") == "OneDrive 客户端",
                "explicit Simplified Chinese translator did not load"
            );
        }
        gui::Localization fallback{QLocale{"en_US"}};
        require(
            !fallback.chinese_loaded() &&
                gui::MainWindow::tr("onedrive client") == "onedrive client",
            "English fallback was not restored"
        );
        const auto system_locale = QLocale::system();
        test_runtime_language(
            system_locale.language() == QLocale::Chinese &&
            system_locale.script() == QLocale::SimplifiedChineseScript
        );
        test_browser_language();
        test_login_refresh_language();
        test_about_button();
    } catch (const std::exception& error) {
        return test::fail(error.what());
    }

    return EXIT_SUCCESS;
}
