#include "support/progress.hpp"
#include "localization_support.hpp"
#include "localization_cases.hpp"

using namespace onedrive;
using namespace onedrive::test::gui_localization;

int main(int argc, char* argv[]) {
    onedrive::test::ScenarioProgress progress{11};
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
            progress.run("startup_language", [&] {
                require(
                    localization.chinese_loaded() == chinese,
                    "startup locale selected the wrong translation"
                );
            });
            progress.run("states_and_messages", [&] {
                return test_states_and_messages(chinese);
            });
            progress.run("window_and_settings", [&] {
                return test_window_and_settings(chinese);
            });
            progress.run("login", [&] { return test_login(chinese); });
            progress.run("browser_callback", [&] {
                return test_browser_callback(chinese);
            });
        }
        progress.run("translator_lifetime", [] {
            require(
                gui::MainWindow::tr("onedrive client") == "onedrive client",
                "destroyed translator remained installed"
            );
        {
            gui::Localization localization{QLocale{"zh_CN"}};
            require(
                localization.translation_loaded() &&
                    localization.chinese_loaded() &&
                    gui::MainWindow::tr("onedrive client") ==
                        "OneDrive 客户端",
                "explicit Simplified Chinese translator did not load"
            );
        }
        {
            gui::Localization traditional{QLocale{"zh_TW"}};
            require(
                !traditional.translation_loaded() &&
                    !traditional.chinese_loaded() &&
                    traditional.language() == gui::Language::english &&
                    gui::MainWindow::tr("onedrive client") == "onedrive client",
                "Traditional Chinese locale unexpectedly matched Simplified "
                "Chinese translator"
            );
        }
        });
        gui::Localization fallback{QLocale{"en_US"}};
        progress.run("english_fallback", [&] {
            require(
                !fallback.chinese_loaded() &&
                    gui::MainWindow::tr("onedrive client") == "onedrive client",
                "English fallback was not restored"
            );
        });
        const auto system_locale = QLocale::system();
        progress.run("runtime_language", [&] {
            return test_runtime_language(
                system_locale.language() == QLocale::Chinese &&
                system_locale.script() == QLocale::SimplifiedChineseScript
            );
        });
        progress.run("browser_language", test_browser_language);
        progress.run("login_refresh_language", test_login_refresh_language);
        progress.run("about_button", test_about_button);
    } catch (const std::exception& error) {
        return test::fail(error.what());
    }

    return progress.finish();
}
