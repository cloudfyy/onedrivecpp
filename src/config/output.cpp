#include "config/parse.hpp"

#include <algorithm>
#include <array>
#include <filesystem>
#include <stdexcept>
#include <string>

namespace onedrive::config::detail {

void load_output_options(
    Config& config,
    const toml::table& root,
    const std::filesystem::path& config_path
) {
    if (const auto* console = optional_table(root, "console", "console")) {
        validate_keys(*console, {"color", "ui", "theme"}, "console");
        if (const auto value = optional_value<std::string>(
                *console, "color", "console.color", "a string"
            )) {
            try {
                config.console_color = cli::Console::parse_color_mode(*value);
            } catch (const std::invalid_argument&) {
                throw std::runtime_error(
                    "invalid TOML configuration value for 'console.color'"
                );
            }
        }
        if (const auto value = optional_value<std::string>(
                *console, "ui", "console.ui", "a string"
            )) {
            try {
                config.console_ui = cli::Console::parse_ui_mode(*value);
            } catch (const std::invalid_argument&) {
                throw std::runtime_error(
                    "invalid TOML configuration value for 'console.ui'"
                );
            }
        }
        if (const auto value = optional_value<std::string>(
                *console, "theme", "console.theme", "a string"
            )) {
            try {
                config.console_theme =
                    cli::Console::parse_tui_theme(*value);
            } catch (const std::invalid_argument&) {
                throw std::runtime_error(
                    "invalid TOML configuration value for 'console.theme'"
                );
            }
        }
    }

    if (const auto* logging = optional_table(root, "logging", "logging")) {
        validate_keys(*logging, {"level", "file"}, "logging");
        if (const auto value = optional_value<std::string>(
                *logging, "level", "logging.level", "a string"
            )) {
            constexpr std::array levels{
                "trace", "debug", "info", "warn",
                "error", "critical", "off",
            };
            if (std::ranges::find(levels, *value) == levels.end()) {
                throw std::runtime_error(
                    "invalid TOML configuration value for 'logging.level'"
                );
            }
            config.logging.level = *value;
        }
        if (const auto value = optional_value<std::string>(
                *logging, "file", "logging.file", "a string"
            )) {
            if (value->empty()) {
                throw std::runtime_error(
                    "logging.file must not be empty"
                );
            }
            auto path = std::filesystem::path{*value};
            if (path.is_relative()) {
                path = config_path.parent_path() / path;
            }
            config.logging.file = path.lexically_normal();
        }
    }
}

}  // namespace onedrive::config::detail
