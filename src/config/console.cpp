#include "onedrive/config/console.hpp"

#include <cctype>
#include <cstdlib>
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
    if (value == "auto") {
        return Language::automatic;
    }
    if (value == "en" || equals_ignore_case(value, "en_US") ||
        equals_ignore_case(value, "en-US")) {
        return Language::english;
    }
    if (equals_ignore_case(value, "zh_CN") ||
        equals_ignore_case(value, "zh-CN") ||
        equals_ignore_case(value, "zh_Hans") ||
        equals_ignore_case(value, "zh-Hans") ||
        equals_ignore_case(value, "zh")) {
        return Language::simplified_chinese;
    }
    throw std::invalid_argument("invalid language: " + std::string{value});
}

std::string_view language_name(Language language) noexcept {
    switch (language) {
    case Language::automatic:
        return "auto";
    case Language::english:
        return "en";
    case Language::simplified_chinese:
        return "zh_CN";
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
    if (equals_ignore_case(locale, "zh_CN") ||
        equals_ignore_case(locale, "zh-CN") ||
        equals_ignore_case(locale, "zh_SG") ||
        equals_ignore_case(locale, "zh-SG") ||
        equals_ignore_case(locale, "zh_Hans") ||
        equals_ignore_case(locale, "zh-Hans") ||
        starts_with_ignore_case(locale, "zh_Hans_") ||
        starts_with_ignore_case(locale, "zh-Hans-")) {
        return Language::simplified_chinese;
    }
    return Language::english;
}

Language resolve_language(Language language) noexcept {
    if (language != Language::automatic) {
        return language;
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
