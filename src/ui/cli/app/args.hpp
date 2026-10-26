#pragma once

#include "onedrive/ui/cli/console.hpp"
#include "app/operation.hpp"

#include <filesystem>
#include <optional>
#include <string>

namespace onedrive::app::detail {

enum class AuthFlow {
    automatic,
    auth_code,
    device_code,
};

struct Arguments {
    std::filesystem::path config_path;
    AuthFlow auth_flow{AuthFlow::automatic};
    bool force_dry_run{false};
    bool force_large_delete{false};
    bool assume_yes{false};
    std::optional<std::string> log_level;
    std::optional<std::string> log_file;
    std::optional<cli::ColorMode> color_mode;
    std::optional<cli::Language> language;
    cli::OutputMode output_mode{cli::OutputMode::text};
    std::optional<cli::UiMode> ui_mode;
    std::optional<cli::TuiTheme> tui_theme;
    bool quiet{false};
    std::string remote_download_path;
    std::string restore_path;
    std::string site_query;
    std::string inspect_path;
    std::optional<std::string> inspect_file_status;
    std::optional<std::string> blocked_reason;
    std::string blocked_search;
    std::string verify_mode{"metadata"};
    Operation operation{Operation::synchronize};
};

struct ParseResult {
    Arguments arguments;
    std::optional<int> exit_code;
};

[[nodiscard]] ParseResult parse_arguments(int argc, char* argv[]);

} // namespace onedrive::app::detail
