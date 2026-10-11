#include "localization.hpp"

#include <QCoreApplication>

#include <stdexcept>

static void initialize_translation_resources() {
    Q_INIT_RESOURCE(onedrive_translations);
}

namespace onedrive::gui {

Localization::Localization(const QLocale& locale) {
    initialize_translation_resources();
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
