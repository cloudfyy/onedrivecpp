#include "localization.hpp"

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QEvent>

#include <array>
#include <stdexcept>
#include <string_view>

namespace onedrive::gui {
namespace {

QString default_translation_directory() {
    const QDir application_directory{QCoreApplication::applicationDirPath()};
    auto local_directory =
        application_directory.filePath(QStringLiteral("translations"));
    if (QDir{local_directory}.exists()) {
        return local_directory;
    }
    return application_directory.filePath(
        QStringLiteral(ONEDRIVE_GUI_TRANSLATIONS_RELATIVE_DIR)
    );
}

struct GuiLanguageSpec {
    Language language;
    std::string_view code;
    QLocale::Language qt_language;
    QLocale::Script qt_script;
    const char* locale_name;
    const char* qtbase_qm;
    const char* app_qm;
};

constexpr std::array kGuiLanguageSpecs{
    GuiLanguageSpec{
        .language = Language::english,
        .code = "en",
        .qt_language = QLocale::English,
        .qt_script = QLocale::AnyScript,
        .locale_name = "en_US",
        .qtbase_qm = nullptr,
        .app_qm = nullptr,
    },
    GuiLanguageSpec{
        .language = Language::simplified_chinese,
        .code = "zh_CN",
        .qt_language = QLocale::Chinese,
        .qt_script = QLocale::SimplifiedChineseScript,
        .locale_name = "zh_CN",
        .qtbase_qm = "qtbase_zh_CN.qm",
        .app_qm = "onedrive_zh_CN.qm",
    },
};

[[nodiscard]] const GuiLanguageSpec*
match_locale_spec(const QLocale& locale) noexcept {
    for (const auto& spec : kGuiLanguageSpecs) {
        if (spec.qt_language != locale.language()) {
            continue;
        }
        if (spec.qt_script != QLocale::AnyScript &&
            spec.qt_script != locale.script()) {
            continue;
        }
        return &spec;
    }
    return nullptr;
}

} // namespace

QLocale locale_for_language(Language language) {
    if (language == Language::automatic) {
        return QLocale::system();
    }
    for (const auto& spec : kGuiLanguageSpecs) {
        if (spec.language == language) {
            return spec.qt_script == QLocale::AnyScript
                       ? QLocale{spec.qt_language}
                       : QLocale{QString::fromLatin1(spec.locale_name)};
        }
    }
    throw std::logic_error{"unknown GUI language"};
}

Language parse_startup_language(
    QCommandLineParser& parser, const QStringList& arguments
) {
    parser.setApplicationDescription("OneDrive desktop client");
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addOption(
        QCommandLineOption{
            "start-in-tray",
            "Start in the system tray; show the window if no tray is available."
        }
    );
    parser.addOption(
        QCommandLineOption{
            "language",
            "GUI language: auto (system locale), en (English), or zh_CN "
            "(Simplified Chinese).",
            "language",
            "en"
        }
    );
    if (!parser.parse(arguments)) {
        throw std::invalid_argument{parser.errorText().toStdString()};
    }
    if (!parser.positionalArguments().isEmpty()) {
        throw std::invalid_argument{
            "Unexpected positional arguments: " +
            parser.positionalArguments().join(' ').toStdString()
        };
    }
    const auto language = parser.value("language").toStdString();
    if (language == "auto") {
        return Language::automatic;
    }
    for (const auto& spec : kGuiLanguageSpecs) {
        if (language == spec.code) {
            return spec.language;
        }
    }
    throw std::invalid_argument{
        "Invalid GUI language '" + language + "'. Expected auto, en, or zh_CN."
    };
}

Localization::Localization(
    Language language, const QString& translation_directory
)
    : translation_directory_{
          translation_directory.isEmpty() ? default_translation_directory()
                                          : translation_directory
      } {
    set_language(language);
}

Localization::Localization(
    const QLocale& locale, const QString& translation_directory
)
    : Localization{
          match_locale_spec(locale) != nullptr
              ? match_locale_spec(locale)->language
              : Language::english,
          translation_directory
      } {
}

void Localization::set_language(Language language) {
    language_ = apply_locale(locale_for_language(language)) ? language
                                                            : Language::english;
    // Also notify widgets when auto and English use the same source strings.
    if (auto* application = QCoreApplication::instance()) {
        QEvent event{QEvent::LanguageChange};
        QCoreApplication::sendEvent(application, &event);
    }
}

Language Localization::language() const noexcept {
    return language_;
}

bool Localization::apply_locale(const QLocale& locale) {
    if (translator_) {
        QCoreApplication::removeTranslator(translator_.get());
    }
    if (qt_translator_) {
        QCoreApplication::removeTranslator(qt_translator_.get());
    }
    translator_.reset();
    qt_translator_.reset();
    loaded_language_.reset();
    const auto* spec = match_locale_spec(locale);
    if (spec == nullptr || spec->qtbase_qm == nullptr ||
        spec->app_qm == nullptr) {
        return true;
    }
    const QDir directory{translation_directory_};
    auto qt_translator = std::make_unique<QTranslator>();
    auto translator = std::make_unique<QTranslator>();
    const auto qt_catalog =
        directory.filePath(QString::fromLatin1(spec->qtbase_qm));
    const auto app_catalog =
        directory.filePath(QString::fromLatin1(spec->app_qm));
    if (!qt_translator->load(qt_catalog)) {
        qWarning().noquote() << "Cannot load GUI translation:" << qt_catalog
                             << "; using English.";
        return false;
    }
    if (!translator->load(app_catalog)) {
        qWarning().noquote() << "Cannot load GUI translation:" << app_catalog
                             << "; using English.";
        return false;
    }
    qt_translator_ = std::move(qt_translator);
    translator_ = std::move(translator);
    QCoreApplication::installTranslator(qt_translator_.get());
    QCoreApplication::installTranslator(translator_.get());
    loaded_language_ = spec->language;
    return true;
}

bool Localization::translation_loaded() const noexcept {
    return loaded_language_.has_value();
}

bool Localization::chinese_loaded() const noexcept {
    return loaded_language_ == Language::simplified_chinese;
}

QString account_status_text(AccountStatus status) {
    switch (status) {
    case AccountStatus::no_account:
        return QCoreApplication::translate(
            "AccountStatus", "No active account"
        );
    case AccountStatus::credentials_available:
        return QCoreApplication::translate(
            "AccountStatus", "Account credentials available"
        );
    case AccountStatus::no_credentials:
        return QCoreApplication::translate(
            "AccountStatus", "Active account has no saved credentials"
        );
    case AccountStatus::unavailable:
        return QCoreApplication::translate(
            "AccountStatus", "Account status unavailable"
        );
    case AccountStatus::invalid:
        return QCoreApplication::translate(
            "AccountStatus", "Local account credentials need attention"
        );
    }
    throw std::logic_error{"unknown account status"};
}

QString sync_mode_text(sync::SyncMode mode) {
    switch (mode) {
    case sync::SyncMode::bidirectional:
        return QCoreApplication::translate("Configuration", "Bidirectional");
    case sync::SyncMode::upload_only:
        return QCoreApplication::translate("Configuration", "Upload only");
    case sync::SyncMode::download_only:
        return QCoreApplication::translate("Configuration", "Download only");
    }
    throw std::logic_error{"unknown synchronization mode"};
}

QString delete_policy_text(sync::DeletePolicy policy) {
    switch (policy) {
    case sync::DeletePolicy::propagate:
        return QCoreApplication::translate(
            "Configuration", "Propagate deletions"
        );
    case sync::DeletePolicy::preserve:
        return QCoreApplication::translate(
            "Configuration", "Preserve destination files"
        );
    }
    throw std::logic_error{"unknown deletion policy"};
}

} // namespace onedrive::gui
