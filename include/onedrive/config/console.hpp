#pragma once

#include <string_view>

namespace onedrive::config {

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

[[nodiscard]] ColorMode parse_color_mode(std::string_view value);
[[nodiscard]] UiMode parse_ui_mode(std::string_view value);
[[nodiscard]] TuiTheme parse_tui_theme(std::string_view value);

} // namespace onedrive::config
