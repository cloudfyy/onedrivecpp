#include "onedrive/config/console.hpp"

#include <array>
#include <cctype>
#include <cstdlib>
#include <span>
#include <stdexcept>
#include <string>

namespace onedrive::config {
namespace {

[[nodiscard]] bool equals_ignore_case(
    std::string_view left, std::string_view right
) noexcept {
    if (left.size() != right.size()) {
        return false;
    }
    for (std::size_t index = 0; index < left.size(); ++index) {
        if (std::tolower(static_cast<unsigned char>(left[index])) !=
            std::tolower(static_cast<unsigned char>(right[index]))) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool starts_with_ignore_case(
    std::string_view value, std::string_view prefix
) noexcept {
    return value.size() >= prefix.size() &&
           equals_ignore_case(value.substr(0, prefix.size()), prefix);
}

struct LanguageSpec {
    Language language;
    std::string_view canonical_name;
    std::span<const std::string_view> config_aliases;
    std::span<const std::string_view> exact_locales;
    std::span<const std::string_view> prefix_locales;
};

constexpr std::array<std::string_view, 1> kAutoAliases{"auto"};
constexpr std::array<std::string_view, 3> kEnglishAliases{
    "en", "en_US", "en-US"
};
constexpr std::array<std::string_view, 2> kEnglishExactLocales{"en", "C"};
constexpr std::array<std::string_view, 2> kEnglishPrefixLocales{"en_", "en-"};

constexpr std::array<std::string_view, 5> kSimplifiedChineseAliases{
    "zh_CN", "zh-CN", "zh_Hans", "zh-Hans", "zh"
};
constexpr std::array<std::string_view, 6> kSimplifiedChineseExactLocales{
    "zh_CN", "zh-CN", "zh_SG", "zh-SG", "zh_Hans", "zh-Hans"
};
constexpr std::array<std::string_view, 2> kSimplifiedChinesePrefixLocales{
    "zh_Hans_", "zh-Hans-"
};

constexpr std::array kLanguageSpecs{
    LanguageSpec{
        .language = Language::automatic,
        .canonical_name = "auto",
        .config_aliases = kAutoAliases,
        .exact_locales = {},
        .prefix_locales = {},
    },
    LanguageSpec{
        .language = Language::english,
        .canonical_name = "en",
        .config_aliases = kEnglishAliases,
        .exact_locales = kEnglishExactLocales,
        .prefix_locales = kEnglishPrefixLocales,
    },
    LanguageSpec{
        .language = Language::simplified_chinese,
        .canonical_name = "zh_CN",
        .config_aliases = kSimplifiedChineseAliases,
        .exact_locales = kSimplifiedChineseExactLocales,
        .prefix_locales = kSimplifiedChinesePrefixLocales,
    },
};

} // namespace

ColorMode parse_color_mode(std::string_view value) {
    if (value == "auto") {
        return ColorMode::automatic;
    }
    if (value == "always") {
        return ColorMode::always;
    }
    if (value == "never") {
        return ColorMode::never;
    }
    throw std::invalid_argument{"invalid color mode"};
}

UiMode parse_ui_mode(std::string_view value) {
    if (value == "auto") {
        return UiMode::automatic;
    }
    if (value == "console") {
        return UiMode::console;
    }
    if (value == "tui") {
        return UiMode::tui;
    }
    throw std::invalid_argument("invalid UI mode: " + std::string{value});
}

TuiTheme parse_tui_theme(std::string_view value) {
    if (value == "hacker") {
        return TuiTheme::hacker;
    }
    if (value == "ocean") {
        return TuiTheme::ocean;
    }
    if (value == "amber") {
        return TuiTheme::amber;
    }
    if (value == "synthwave") {
        return TuiTheme::synthwave;
    }
    throw std::invalid_argument("invalid TUI theme: " + std::string{value});
}

Language parse_language(std::string_view value) {
    for (const auto& spec : kLanguageSpecs) {
        for (const auto alias : spec.config_aliases) {
            if (equals_ignore_case(value, alias)) {
                return spec.language;
            }
        }
    }
    throw std::invalid_argument("invalid language: " + std::string{value});
}

std::string_view language_name(Language language) noexcept {
    for (const auto& spec : kLanguageSpecs) {
        if (spec.language == language) {
            return spec.canonical_name;
        }
    }
    return "auto";
}

Language resolve_locale_language(std::string_view locale) noexcept {
    if (const auto colon = locale.find(':'); colon != std::string_view::npos) {
        locale = locale.substr(0, colon);
    }
    if (const auto modifier = locale.find('@');
        modifier != std::string_view::npos) {
        locale = locale.substr(0, modifier);
    }
    if (const auto encoding = locale.find('.');
        encoding != std::string_view::npos) {
        locale = locale.substr(0, encoding);
    }
    for (const auto& spec : kLanguageSpecs) {
        for (const auto exact : spec.exact_locales) {
            if (equals_ignore_case(locale, exact)) {
                return spec.language;
            }
        }
        for (const auto prefix : spec.prefix_locales) {
            if (starts_with_ignore_case(locale, prefix)) {
                return spec.language;
            }
        }
    }
    return Language::english;
}

Language resolve_language(Language language) noexcept {
    if (language != Language::automatic) {
        return language;
    }
    if (const char* override_lang = std::getenv("ONEDRIVE_CPP_LANG");
        override_lang != nullptr && *override_lang != '\0') {
        try {
            const auto parsed = parse_language(override_lang);
            if (parsed != Language::automatic) {
                return parsed;
            }
        } catch (const std::invalid_argument&) {
            return resolve_locale_language(override_lang);
        }
    }
    for (const char* variable : {"LC_ALL", "LC_MESSAGES", "LANG", "LANGUAGE"}) {
        if (const char* value = std::getenv(variable);
            value != nullptr && *value != '\0') {
            return resolve_locale_language(value);
        }
    }
    return Language::english;
}

} // namespace onedrive::config
