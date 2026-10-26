#pragma once

#include "app_state_view_model.hpp"
#include "onedrive/sync/capabilities.hpp"

#include <QCommandLineParser>
#include <QLocale>
#include <QString>
#include <QStringList>
#include <QTranslator>

#include <optional>

namespace onedrive::gui {

enum class Language { automatic, english, simplified_chinese };

[[nodiscard]] QLocale locale_for_language(Language language);
[[nodiscard]] Language parse_startup_language(
    QCommandLineParser& parser, const QStringList& arguments
);

class Localization {
public:
    explicit Localization(Language language = Language::automatic);
    explicit Localization(const QLocale& locale);
    void set_language(Language language);
    [[nodiscard]] Language language() const noexcept;
    [[nodiscard]] bool translation_loaded() const noexcept;
    [[nodiscard]] bool chinese_loaded() const noexcept;

private:
    void apply_locale(const QLocale& locale);

    QTranslator qt_translator_;
    QTranslator translator_;
    Language language_{Language::automatic};
    std::optional<Language> loaded_language_;
};

[[nodiscard]] QString account_status_text(AccountStatus status);
[[nodiscard]] QString sync_mode_text(sync::SyncMode mode);
[[nodiscard]] QString delete_policy_text(sync::DeletePolicy policy);

} // namespace onedrive::gui
