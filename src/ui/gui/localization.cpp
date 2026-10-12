#include "localization.hpp"

#include <QCoreApplication>
#include <QEvent>

#include <stdexcept>

static void initialize_translation_resources() {
    Q_INIT_RESOURCE(onedrive_translations);
}

namespace onedrive::gui {

QLocale locale_for_language(Language language) {
    switch (language) {
    case Language::automatic:
        return QLocale::system();
    case Language::english:
        return QLocale{QLocale::English};
    case Language::simplified_chinese:
        return QLocale{"zh_CN"};
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
            "language",
            "GUI language: auto (system locale), en (English), or zh_CN "
            "(Simplified Chinese).",
            "language",
            "auto"
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
    const auto language = parser.value("language");
    if (language == "auto") {
        return Language::automatic;
    }
    if (language == "en") {
        return Language::english;
    }
    if (language == "zh_CN") {
        return Language::simplified_chinese;
    }
    throw std::invalid_argument{
        "Invalid GUI language '" + language.toStdString() +
        "'. Expected auto, en, or zh_CN."
    };
}

Localization::Localization(Language language) {
    initialize_translation_resources();
    set_language(language);
}

Localization::Localization(const QLocale& locale) {
    initialize_translation_resources();
    language_ = locale.language() == QLocale::Chinese &&
                        locale.script() == QLocale::SimplifiedChineseScript
                    ? Language::simplified_chinese
                    : Language::english;
    apply_locale(locale);
}

void Localization::set_language(Language language) {
    apply_locale(locale_for_language(language));
    language_ = language;
    // Also notify widgets when auto and English use the same source strings.
    if (auto* application = QCoreApplication::instance()) {
        QEvent event{QEvent::LanguageChange};
        QCoreApplication::sendEvent(application, &event);
    }
}

Language Localization::language() const noexcept {
    return language_;
}

void Localization::apply_locale(const QLocale& locale) {
    if (chinese_loaded_) {
        QCoreApplication::removeTranslator(&translator_);
        QCoreApplication::removeTranslator(&qt_translator_);
        chinese_loaded_ = false;
    }
    // Only Simplified Chinese is supported; Traditional Chinese falls back
    // to English rather than unexpectedly displaying Simplified Chinese.
    if (locale.language() != QLocale::Chinese ||
        locale.script() != QLocale::SimplifiedChineseScript) {
        return;
    }
    if (!qt_translator_.load(":/i18n/qtbase_zh_CN.qm") ||
        !translator_.load(":/i18n/onedrive_zh_CN.qm")) {
        return;
    }
    QCoreApplication::installTranslator(&qt_translator_);
    QCoreApplication::installTranslator(&translator_);
    chinese_loaded_ = true;
}

bool Localization::chinese_loaded() const noexcept {
    return chinese_loaded_;
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
