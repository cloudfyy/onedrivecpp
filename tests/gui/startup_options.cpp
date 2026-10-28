#include "support/progress.hpp"
#include "localization.hpp"
#include "onedrive/version.hpp"
#include "support/common.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QProcess>

#include <stdexcept>
#include <utility>

namespace {

using namespace onedrive;

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error{message};
    }
}

QLocale parse(const QStringList& options) {
    QCommandLineParser parser;
    QStringList arguments{"onedrive-cpp-gui"};
    arguments.append(options);
    return gui::locale_for_language(
        gui::parse_startup_language(parser, arguments)
    );
}

void test_locales() {
    {
        gui::Localization defaults;
        require(
            defaults.language() == gui::Language::english &&
                !defaults.translation_loaded(),
            "default localization loaded a non-English catalog"
        );
    }
    for (const auto& [option, expected_language] : {
             std::pair{"auto", gui::Language::automatic},
             std::pair{"en", gui::Language::english},
             std::pair{"zh_CN", gui::Language::simplified_chinese},
         }) {
        QCommandLineParser parser;
        require(
            gui::parse_startup_language(
                parser, {"onedrive-cpp-gui", "--language", option}
            ) == expected_language,
            "startup parsing lost the explicit language selection"
        );
        gui::Localization localization{expected_language};
        require(
            localization.language() == expected_language,
            "localization lost auto versus an explicit language override"
        );
    }
    require(
        parse({}) == gui::locale_for_language(gui::Language::english) &&
            parse({"--start-in-tray"}) ==
                gui::locale_for_language(gui::Language::english),
        "default startup did not use English"
    );
    require(
        parse({"--language", "auto"}) == QLocale::system() &&
            parse({"--language=auto"}) == QLocale::system(),
        "explicit auto did not preserve the system locale"
    );
    for (const auto& language : {QString{"en"}, QString{"zh_CN"}}) {
        for (const bool equals : {false, true}) {
            const auto options = equals ? QStringList{"--language=" + language}
                                        : QStringList{"--language", language};
            gui::Localization localization{parse(options)};
            const bool chinese = language == "zh_CN";
            require(
                localization.chinese_loaded() == chinese &&
                    QCoreApplication::translate(
                        "onedrive::gui::MainWindow", "onedrive client"
                    ) == (chinese ? "OneDrive 客户端" : "onedrive client"),
                "language override did not select the correct translator"
            );
        }
    }
}

void test_tray_options() {
    QCommandLineParser defaults;
    static_cast<void>(
        gui::parse_startup_language(defaults, {"onedrive-cpp-gui"})
    );
    require(
        !defaults.isSet("start-in-tray"), "normal launch requested tray startup"
    );
    for (const auto& language : {"auto", "en", "zh_CN"}) {
        for (const bool flag_first : {false, true}) {
            QCommandLineParser parser;
            const QStringList arguments =
                flag_first
                    ? QStringList{"onedrive-cpp-gui", "--start-in-tray", "--language", language}
                    : QStringList{
                          "onedrive-cpp-gui",
                          "--language",
                          language,
                          "--start-in-tray"
                      };
            const auto selected =
                gui::parse_startup_language(parser, arguments);
            require(
                parser.isSet("start-in-tray") &&
                    gui::locale_for_language(selected) ==
                        parse({"--language", language}),
                "tray startup was lost or changed language selection with "
                "reordered options"
            );
        }
    }
}

void test_external_catalogs() {
    test::TemporaryDirectory fixture;
    const auto directory = QString::fromStdString(fixture.path().string());
    gui::Localization localization{gui::Language::english, directory};
    const auto english = [&] {
        require(
            localization.language() == gui::Language::english &&
                !localization.translation_loaded() &&
                QCoreApplication::translate(
                    "onedrive::gui::MainWindow", "onedrive client"
                ) == "onedrive client" &&
                QCoreApplication::translate("QPlatformTheme", "Cancel") ==
                    "Cancel",
            "missing or invalid catalogs did not restore complete English"
        );
    };
    english();
    localization.set_language(gui::Language::simplified_chinese);
    english();

    const QDir source{QCoreApplication::applicationDirPath() + "/translations"};
    const QDir target{directory};
    require(
        QFile::copy(
            source.filePath("qtbase_zh_CN.qm"),
            target.filePath("qtbase_zh_CN.qm")
        ),
        "could not prepare external Qt catalog"
    );
    localization.set_language(gui::Language::simplified_chinese);
    english();
    const auto app_catalog = target.filePath("onedrive_zh_CN.qm");
    {
        QFile corrupt{app_catalog};
        require(
            corrupt.open(QIODevice::WriteOnly) &&
                corrupt.write("not a translation catalog") > 0,
            "could not prepare corrupt application catalog"
        );
    }
    localization.set_language(gui::Language::simplified_chinese);
    english();
    require(
        QFile::remove(app_catalog) &&
            QFile::copy(source.filePath("onedrive_zh_CN.qm"), app_catalog),
        "could not prepare external application catalog"
    );
    localization.set_language(gui::Language::simplified_chinese);
    require(
        localization.chinese_loaded() &&
            QCoreApplication::translate(
                "onedrive::gui::MainWindow", "onedrive client"
            ) == "OneDrive 客户端" &&
            QCoreApplication::translate("QPlatformTheme", "Cancel") == "取消",
        "external application and Qt catalogs did not load on demand"
    );
    localization.set_language(gui::Language::english);
    english();
    require(QFile::remove(app_catalog), "could not remove external catalog");
    localization.set_language(gui::Language::simplified_chinese);
    english();
}

void test_invalid_options() {
    const QStringList invalid_languages{
        "", "fr", "zh", "zh_TW", "zh-CN", "EN", "AUTO"
    };
    for (const auto& language : invalid_languages) {
        try {
            static_cast<void>(parse({"--language=" + language}));
            require(false, "invalid language was accepted");
        } catch (const std::invalid_argument& error) {
            require(
                QString::fromUtf8(error.what())
                    .contains("Expected auto, en, or zh_CN."),
                "invalid language error did not list accepted values"
            );
        }
    }
    for (const auto& options :
         {QStringList{"--unknown"},
          QStringList{"--start-in-tray=yes"},
          QStringList{"--language"},
          QStringList{"unexpected"},
          QStringList{"--", "unexpected"},
          QStringList{"--help", "unexpected"},
          QStringList{"--help", "--language=fr"}}) {
        try {
            static_cast<void>(parse(options));
            require(false, "invalid arguments were accepted");
        } catch (const std::invalid_argument& error) {
            require(
                !QString::fromUtf8(error.what()).isEmpty(),
                "invalid arguments produced no error"
            );
        }
    }
}

void test_process(
    const QStringList& arguments,
    bool success,
    const QByteArray& expected_output
) {
    QProcess process;
    process.start(ONEDRIVE_GUI_EXECUTABLE, arguments);
    require(process.waitForStarted(5000), "GUI executable did not start");
    if (!process.waitForFinished(5000)) {
        process.kill();
        process.waitForFinished(5000);
        require(false, "GUI command entered the event loop instead of exiting");
    }
    require(
        process.exitStatus() == QProcess::NormalExit &&
            (process.exitCode() == 0) == success,
        "GUI command returned the wrong exit status"
    );
    const auto output = success ? process.readAllStandardOutput()
                                : process.readAllStandardError();
    require(
        output.contains(expected_output),
        "GUI command did not print the expected help, version, or error"
    );
}

} // namespace

int main(int argc, char* argv[]) {
    onedrive::test::ScenarioProgress progress{19};
    QCoreApplication application{argc, argv};
    try {
        progress.run("locales", test_locales);
        progress.run("tray_options", test_tray_options);
        progress.run("external_catalogs", test_external_catalogs);
        progress.run("invalid_options", test_invalid_options);
        for (const auto& language : {"auto", "en", "zh_CN"}) {
            progress.run(
                std::string{"process (--language "} + language + " --help)",
                [&] {
                    return test_process(
                        {"--language", language, "--help"},
                        true,
                        "auto (system locale), en (English)"
                    );
                }
            );
        }
        progress.run("process (-h)", [&] {
            return test_process({"-h"}, true, "--start-in-tray");
        });
        progress.run("process (--help-all)", [&] {
            return test_process({"--help-all"}, true, "--language");
        });
        const auto version =
            QByteArray{"onedrive-cpp-gui "} + build_info::version + '\n';
        progress.run("process (--version)", [&] {
            return test_process({"--version"}, true, version);
        });
        progress.run("process (-v)", [&] {
            return test_process({"-v"}, true, version);
        });
        progress.run("process (--language=zh_CN --version)", [&] {
            return test_process(
                {"--language=zh_CN", "--version"}, true, version
            );
        });
        progress.run("process (--language=fr)", [&] {
            return test_process(
                {"--language=fr"}, false, "Expected auto, en, or zh_CN."
            );
        });
        progress.run("process (--language=)", [&] {
            return test_process(
                {"--language="}, false, "Expected auto, en, or zh_CN."
            );
        });
        progress.run("process (--help --language=fr)", [&] {
            return test_process(
                {"--help", "--language=fr"},
                false,
                "Expected auto, en, or zh_CN."
            );
        });
        progress.run("process (--unknown)", [&] {
            return test_process({"--unknown"}, false, "Unknown option");
        });
        progress.run("process (--language)", [&] {
            return test_process({"--language"}, false, "Missing value");
        });
        progress.run("process (unexpected)", [&] {
            return test_process(
                {"unexpected"}, false, "Unexpected positional arguments"
            );
        });
        progress.run("process (--help unexpected)", [&] {
            return test_process(
                {"--help", "unexpected"},
                false,
                "Unexpected positional arguments"
            );
        });
    } catch (const std::exception& error) {
        return test::fail(error.what());
    }
    return progress.finish();
}
