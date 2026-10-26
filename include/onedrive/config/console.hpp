#pragma once

#include <cstddef>

#include <string_view>

namespace onedrive::config {

inline constexpr std::size_t default_file_history_limit = 10'000;

enum class ColorMode {
    automatic,
    always,
    never,
};

enum class UiMode {
    automatic,
    console,
    tui,
};

enum class TuiTheme {
    hacker,
    ocean,
    amber,
    synthwave,
};

enum class Language {
    automatic,
    english,
    simplified_chinese,
};

[[nodiscard]] ColorMode parse_color_mode(std::string_view value);
[[nodiscard]] UiMode parse_ui_mode(std::string_view value);
[[nodiscard]] TuiTheme parse_tui_theme(std::string_view value);
[[nodiscard]] Language parse_language(std::string_view value);
[[nodiscard]] std::string_view language_name(Language language) noexcept;
[[nodiscard]] Language
resolve_locale_language(std::string_view locale) noexcept;
[[nodiscard]] Language resolve_language(Language language) noexcept;

} // namespace onedrive::config
