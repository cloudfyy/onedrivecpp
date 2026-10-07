#include "args.hpp"

#include "onedrive/version.hpp"

#include <CLI/CLI.hpp>

#include <cstdlib>
#include <iostream>
#include <map>

namespace onedrive::app::detail {
namespace {

std::filesystem::path default_config_path() {
    if (const char* home = std::getenv("HOME"); home != nullptr) {
        return std::filesystem::path{home} / ".config/onedrive-cpp/config.toml";
    }
    return "/etc/onedrive-cpp/onedrive-cpp.toml";
}

} // namespace

ParseResult parse_arguments(int argc, char* argv[]) {
    ParseResult result;
    auto& arguments = result.arguments;
    arguments.config_path = default_config_path();
    const std::map<std::string, cli::ColorMode> color_modes{
        {"auto", cli::ColorMode::automatic},
        {"always", cli::ColorMode::always},
        {"never", cli::ColorMode::never},
    };
    const std::map<std::string, cli::OutputMode> output_modes{
        {"text", cli::OutputMode::text},
        {"json", cli::OutputMode::json},
    };
    const std::map<std::string, cli::UiMode> ui_modes{
        {"auto", cli::UiMode::automatic},
        {"console", cli::UiMode::console},
        {"tui", cli::UiMode::tui},
    };
    const std::map<std::string, cli::TuiTheme> tui_themes{
        {"hacker", cli::TuiTheme::hacker},
        {"ocean", cli::TuiTheme::ocean},
        {"amber", cli::TuiTheme::amber},
        {"synthwave", cli::TuiTheme::synthwave},
    };

    CLI::App application{
        "A modern C++ OneDrive synchronization client", "onedrive-cpp"
    };
    application.set_version_flag(
        "--version", std::string{"onedrive-cpp "} + build_info::version
    );
    application.require_subcommand(1);

    auto* auth = application.add_subcommand(
        "auth", "Authorize with Microsoft using the device code flow"
    );
    auto* logout = application.add_subcommand(
        "logout", "Remove the locally stored refresh token"
    );
    auto* doctor = application.add_subcommand(
        "doctor", "Run local synchronization-state diagnostics"
    );
    auto* drives = application.add_subcommand(
        "drives", "List OneDrive drives available to the active account"
    );
    auto* shared = application.add_subcommand(
        "shared", "List items shared with the account and OneDrive shortcuts"
    );
    auto* sites = application.add_subcommand(
        "sites", "Find SharePoint sites and their document libraries"
    );
    auto* quota = application.add_subcommand(
        "quota", "Show storage quota for the configured drive"
    );
    auto* status = application.add_subcommand(
        "status", "Show read-only synchronization status"
    );
    auto* state = application.add_subcommand(
        "state", "Maintain saved synchronization state"
    );
    state->require_subcommand(1);
    auto* reset_cursor = state->add_subcommand(
        "reset-cursor",
        "Reset the cloud change cursor while preserving item state"
    );
    auto* clear_state = state->add_subcommand(
        "clear",
        "Clear all saved synchronization state for the configured drive"
    );
    auto* sync =
        application.add_subcommand("sync", "Synchronize OneDrive files");
    auto* download = application.add_subcommand(
        "download", "Download one remote file by its Drive-relative path"
    );
    auto* monitor = application.add_subcommand(
        "monitor", "Monitor for synchronization changes"
    );

    const auto add_common_options = [&](CLI::App& command) {
        command
            .add_option(
                "--config",
                arguments.config_path,
                "Path to the configuration file"
            )
            ->type_name("PATH");
        command
            .add_option(
                "--log-level",
                arguments.log_level,
                "Minimum log level (overrides configuration)"
            )
            ->check(
                CLI::IsMember(
                    {"trace",
                     "debug",
                     "info",
                     "warn",
                     "error",
                     "critical",
                     "off"},
                    CLI::ignore_case
                )
            );
        command
            .add_option(
                "--log-file",
                arguments.log_file,
                "Write rotating logs to this file (overrides configuration)"
            )
            ->type_name("PATH");
        command
            .add_option(
                "--color",
                arguments.color_mode,
                "Color output override: auto, always, or never"
            )
            ->transform(CLI::CheckedTransformer(color_modes));
        command
            .add_option(
                "--output", arguments.output_mode, "Output format: text or json"
            )
            ->transform(CLI::CheckedTransformer(output_modes))
            ->default_str("text");
        command.add_flag(
            "--quiet",
            arguments.quiet,
            "Suppress informational and success output"
        );
    };
    for (auto* command : {
             auth,
             logout,
             doctor,
             drives,
             shared,
             sites,
             quota,
             status,
             reset_cursor,
             clear_state,
             sync,
             download,
             monitor,
         }) {
        add_common_options(*command);
    }
    clear_state->add_flag(
        "--yes",
        arguments.assume_yes,
        "Confirm the state clear without an interactive prompt"
    );
    sync->add_flag(
        "--dry-run",
        arguments.force_dry_run,
        "Show synchronization inputs without changing remote files"
    );
    sync->add_flag(
        "--force-large-delete",
        arguments.force_large_delete,
        "Allow this sync to exceed the configured remote deletion limit"
    );
    for (auto* command : {auth, doctor, status, sync, download, monitor}) {
        command
            ->add_option(
                "--ui", arguments.ui_mode, "Interface: auto, console, or tui"
            )
            ->transform(CLI::CheckedTransformer(ui_modes))
            ->default_str("auto");
        command
            ->add_option(
                "--theme",
                arguments.tui_theme,
                "TUI theme: hacker, ocean, amber, or synthwave"
            )
            ->transform(CLI::CheckedTransformer(tui_themes))
            ->default_str("hacker");
    }
    download
        ->add_option(
            "REMOTE_PATH",
            arguments.remote_download_path,
            "Drive-relative path of the remote file"
        )
        ->required();
    download->add_flag(
        "--dry-run",
        arguments.force_dry_run,
        "Show the single-file download plan without changing local state"
    );
    sites
        ->add_option(
            "QUERY", arguments.site_query, "SharePoint site search query"
        )
        ->required();

    if (argc < 2) {
        std::cout << application.help();
        result.exit_code = 0;
        return result;
    }
    try {
        application.parse(argc, argv);
    } catch (const CLI::ParseError& error) {
        const int exit_code = application.exit(error);
        result.exit_code = exit_code == 0 ? 0 : 2;
        return result;
    }

    arguments.operation = *auth           ? Operation::authenticate
                          : *logout       ? Operation::logout
                          : *doctor       ? Operation::diagnose
                          : *drives       ? Operation::drives
                          : *shared       ? Operation::shared
                          : *sites        ? Operation::sites
                          : *quota        ? Operation::quota
                          : *status       ? Operation::status
                          : *reset_cursor ? Operation::reset_cursor
                          : *clear_state  ? Operation::clear_state
                          : *download     ? Operation::download
                          : *monitor      ? Operation::monitor
                                          : Operation::synchronize;
    return result;
}

} // namespace onedrive::app::detail
