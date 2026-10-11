#pragma once

#include "app_state_view_model.hpp"
#include "onedrive/sync/capabilities.hpp"

#include <QLocale>
#include <QString>
#include <QTranslator>

namespace onedrive::gui {

class Localization {
public:
    explicit Localization(const QLocale& locale = QLocale::system());
    [[nodiscard]] bool chinese_loaded() const noexcept;

private:
    QTranslator qt_translator_;
    QTranslator translator_;
    bool chinese_loaded_{false};
};

[[nodiscard]] QString account_status_text(AccountStatus status);
[[nodiscard]] QString sync_mode_text(sync::SyncMode mode);
[[nodiscard]] QString delete_policy_text(sync::DeletePolicy policy);

} // namespace onedrive::gui
