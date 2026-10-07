#pragma once

#include "onedrive/cli/console.hpp"
#include "operation.hpp"

#include <filesystem>
#include <optional>
#include <string>

namespace onedrive::app::detail {

struct Arguments {
    std::filesystem::path config_path;
    bool force_dry_run{false};
    bool force_large_delete{false};
    bool assume_yes{false};
    std::optional<std::string> log_level;
    std::optional<std::string> log_file;
    std::optional<cli::ColorMode> color_mode;
    cli::OutputMode output_mode{cli::OutputMode::text};
    std::optional<cli::UiMode> ui_mode;
    std::optional<cli::TuiTheme> tui_theme;
    bool quiet{false};
    std::string remote_download_path;
    std::string site_query;
    std::string inspect_path;
    std::optional<std::string> inspect_file_status;
    Operation operation{Operation::synchronize};
};

struct ParseResult {
    Arguments arguments;
    std::optional<int> exit_code;
};

[[nodiscard]] ParseResult parse_arguments(int argc, char* argv[]);

} // namespace onedrive::app::detail
