#include "onedrive/config/console.hpp"

#include <stdexcept>
#include <string>

namespace onedrive::config {

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

} // namespace onedrive::config
