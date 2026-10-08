#include "args.hpp"

#include "onedrive/version.hpp"

#include <CLI/CLI.hpp>

#include <array>
#include <cstdlib>
#include <iostream>
#include <map>
#include <utility>

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

    auto* account = application.add_subcommand(
        "account", "Manage Microsoft account authentication"
    );
    account->require_subcommand(1);
    auto* account_login = account->add_subcommand(
        "login", "Authorize with Microsoft using the device code flow"
    );
    auto* account_logout = account->add_subcommand(
        "logout", "Remove the locally stored refresh token"
    );
    auto* inspect = application.add_subcommand(
        "inspect", "Inspect account, cloud, and synchronization state"
    );
    inspect->require_subcommand(1);
    auto* inspect_health = inspect->add_subcommand(
        "health", "Run local synchronization-state diagnostics"
    );
    auto* inspect_drives = inspect->add_subcommand(
        "drives", "List OneDrive drives available to the active account"
    );
    auto* inspect_shared = inspect->add_subcommand(
        "shared", "List items shared with the account and OneDrive shortcuts"
    );
    auto* inspect_sites = inspect->add_subcommand(
        "sites", "Find SharePoint sites and their document libraries"
    );
    auto* inspect_quota = inspect->add_subcommand(
        "quota", "Show storage quota for the configured drive"
    );
    auto* inspect_status = inspect->add_subcommand(
        "status", "Show read-only synchronization status"
    );
    auto* inspect_storage = inspect->add_subcommand(
        "storage", "Show local synchronization storage usage"
    );
    auto* inspect_partials = inspect->add_subcommand(
        "partials", "List resumable and invalid partial downloads"
    );
    auto* inspect_files = inspect->add_subcommand(
        "files", "Check downloaded files against saved synchronization state"
    );
    auto* inspect_verify = inspect->add_subcommand(
        "verify", "Verify downloaded file metadata or content"
    );
    auto* inspect_config = inspect->add_subcommand(
        "config", "Show effective non-secret configuration"
    );
    auto* state = application.add_subcommand(
        "state", "Maintain saved synchronization state"
    );
    state->require_subcommand(1);
    auto* reset_cursor = state->add_subcommand(
        "reset-cursor",
        "Reset the cloud change cursor while preserving item state"
    );
    auto* cleanup_state = state->add_subcommand(
        "cleanup", "Remove invalid and orphaned partial download state"
    );
    auto* migrate_state = state->add_subcommand(
        "migrate", "Back up and upgrade local state databases without syncing"
    );
    auto* clear_state = state->add_subcommand(
        "clear",
        "Clear all saved synchronization state for the configured drive"
    );
    auto* transfer =
        application.add_subcommand("transfer", "Transfer OneDrive files");
    transfer->require_subcommand(1);
    auto* transfer_sync =
        transfer->add_subcommand("sync", "Synchronize OneDrive files");
    auto* transfer_download = transfer->add_subcommand(
        "download", "Download one remote file by its Drive-relative path"
    );
    auto* transfer_watch = transfer->add_subcommand(
        "watch", "Watch for synchronization changes"
    );
    const std::array command_operations{
        std::pair{account_login, Operation::authenticate},
        std::pair{account_logout, Operation::logout},
        std::pair{inspect_health, Operation::diagnose},
        std::pair{inspect_drives, Operation::drives},
        std::pair{inspect_shared, Operation::shared},
        std::pair{inspect_sites, Operation::sites},
        std::pair{inspect_quota, Operation::quota},
        std::pair{inspect_status, Operation::status},
        std::pair{inspect_storage, Operation::storage},
        std::pair{inspect_partials, Operation::partials},
        std::pair{inspect_files, Operation::files},
        std::pair{inspect_verify, Operation::verify},
        std::pair{inspect_config, Operation::config},
        std::pair{reset_cursor, Operation::reset_cursor},
        std::pair{cleanup_state, Operation::cleanup_state},
        std::pair{migrate_state, Operation::migrate_state},
        std::pair{clear_state, Operation::clear_state},
        std::pair{transfer_sync, Operation::synchronize},
        std::pair{transfer_download, Operation::download},
        std::pair{transfer_watch, Operation::monitor},
    };

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
    for (const auto& [command, operation] : command_operations) {
        static_cast<void>(operation);
        add_common_options(*command);
    }
    clear_state->add_flag(
        "--yes",
        arguments.assume_yes,
        "Confirm the state clear without an interactive prompt"
    );
    cleanup_state->add_flag(
        "--dry-run",
        arguments.force_dry_run,
        "Show cleanup candidates without removing files or state"
    );
    cleanup_state->add_flag(
        "--yes",
        arguments.assume_yes,
        "Confirm removal of invalid partial files and state"
    );
    migrate_state->add_flag(
        "--dry-run",
        arguments.force_dry_run,
        "List database versions and upgrade candidates without changing them"
    );
    migrate_state->add_flag(
        "--yes", arguments.assume_yes, "Confirm database backups and upgrades"
    );
    transfer_sync->add_flag(
        "--dry-run",
        arguments.force_dry_run,
        "Show synchronization inputs without changing remote files"
    );
    transfer_sync->add_flag(
        "--force-large-delete",
        arguments.force_large_delete,
        "Allow this sync to exceed the configured remote deletion limit"
    );
    for (const auto& [command, operation] : command_operations) {
        if (!operation_capabilities(operation).supports_tui) {
            continue;
        }
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
    transfer_download
        ->add_option(
            "REMOTE_PATH",
            arguments.remote_download_path,
            "Drive-relative path of the remote file"
        )
        ->required();
    transfer_download->add_flag(
        "--dry-run",
        arguments.force_dry_run,
        "Show the single-file download plan without changing local state"
    );
    inspect_sites
        ->add_option(
            "QUERY", arguments.site_query, "SharePoint site search query"
        )
        ->required();
    inspect_files->add_option(
        "PATH",
        arguments.inspect_path,
        "Optional Drive-relative file or directory path"
    );
    inspect_files
        ->add_option(
            "--status",
            arguments.inspect_file_status,
            "Only show files with this status"
        )
        ->check(CLI::IsMember(
            {"ok", "missing", "modified", "type-changed", "outside-root"}
        ));
    inspect_verify->add_option(
        "PATH",
        arguments.inspect_path,
        "Optional Drive-relative file or directory path"
    );
    inspect_verify
        ->add_option(
            "--mode",
            arguments.verify_mode,
            "Verification mode: metadata or content"
        )
        ->check(CLI::IsMember({"metadata", "content"}))
        ->default_str("metadata");

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

    arguments.operation = *account_login       ? Operation::authenticate
                          : *account_logout    ? Operation::logout
                          : *inspect_health    ? Operation::diagnose
                          : *inspect_drives    ? Operation::drives
                          : *inspect_shared    ? Operation::shared
                          : *inspect_sites     ? Operation::sites
                          : *inspect_quota     ? Operation::quota
                          : *inspect_status    ? Operation::status
                          : *inspect_storage   ? Operation::storage
                          : *inspect_partials  ? Operation::partials
                          : *inspect_files     ? Operation::files
                          : *inspect_verify    ? Operation::verify
                          : *inspect_config    ? Operation::config
                          : *reset_cursor      ? Operation::reset_cursor
                          : *cleanup_state     ? Operation::cleanup_state
                          : *migrate_state     ? Operation::migrate_state
                          : *clear_state       ? Operation::clear_state
                          : *transfer_download ? Operation::download
                          : *transfer_watch    ? Operation::monitor
                                               : Operation::synchronize;
    return result;
}

} // namespace onedrive::app::detail
