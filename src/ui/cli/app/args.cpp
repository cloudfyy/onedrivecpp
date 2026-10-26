#include "args.hpp"

#include "onedrive/config/config.hpp"
#include "onedrive/exit_code.hpp"
#include "onedrive/version.hpp"
#include "ui/cli/localization.hpp"

#include <CLI/CLI.hpp>

#include <array>
#include <cstdlib>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace onedrive::app::detail {
namespace {

cli::Language resolve_help_language(
    int argc, char* argv[], const std::filesystem::path& default_config
) {
    std::filesystem::path config_path = default_config;
    std::optional<cli::Language> cli_language;
    bool wants_help = argc < 2;
    for (int index = 1; index < argc; ++index) {
        const std::string_view arg{argv[index]};
        if (arg == "--") {
            break;
        }
        if (arg == "-h" || arg == "--help") {
            wants_help = true;
            continue;
        }
        std::optional<std::string_view> language_value;
        if (arg == "--language" && index + 1 < argc) {
            language_value = std::string_view{argv[index + 1]};
        } else if (arg.starts_with("--language=")) {
            language_value = arg.substr(std::string_view{"--language="}.size());
        }
        if (language_value) {
            try {
                cli_language = cli::Console::parse_language(*language_value);
            } catch (const std::invalid_argument&) {
            }
            continue;
        }
        if (arg == "--config" && index + 1 < argc) {
            config_path = argv[index + 1];
        } else if (arg.starts_with("--config=")) {
            config_path = arg.substr(std::string_view{"--config="}.size());
        }
    }
    if (cli_language) {
        return cli::resolve_language(*cli_language);
    }
    if (wants_help) {
        try {
            const auto loaded = config::Config::load(config_path);
            if (loaded.console_language != cli::Language::automatic) {
                return cli::resolve_language(loaded.console_language);
            }
        } catch (const std::exception&) {
        }
    }
    return cli::resolve_language(cli::Language::automatic);
}

} // namespace

ParseResult parse_arguments(int argc, char* argv[]) {
    ParseResult result;
    auto& arguments = result.arguments;
    arguments.config_path = config::default_config_path();
    const auto help_language =
        resolve_help_language(argc, argv, arguments.config_path);
    const auto tr = [help_language](std::string_view text) {
        return std::string{cli::detail::translate_text(text, help_language)};
    };
    const std::map<std::string, cli::ColorMode> color_modes{
        {"auto", cli::ColorMode::automatic},
        {"always", cli::ColorMode::always},
        {"never", cli::ColorMode::never},
    };
    const std::map<std::string, cli::Language> languages{
        {"auto", cli::Language::automatic},
        {"en", cli::Language::english},
        {"zh_CN", cli::Language::simplified_chinese},
        {"zh-CN", cli::Language::simplified_chinese},
        {"zh_cn", cli::Language::simplified_chinese},
        {"zh-cn", cli::Language::simplified_chinese},
        {"zh", cli::Language::simplified_chinese},
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
    const std::map<std::string, AuthFlow> auth_flows{
        {"auto", AuthFlow::automatic},
        {"auth-code", AuthFlow::auth_code},
        {"device-code", AuthFlow::device_code},
    };

    CLI::App application{
        tr("A modern C++ OneDrive synchronization client"), "onedrive-cpp"
    };
    const auto application_version =
        std::string{"onedrive-cpp "} + build_info::version;
    application.set_help_flag(
        "-h,--help", tr("Print this help message and exit")
    );
    application.set_version_flag(
        "--version",
        application_version,
        tr("Display program version information and exit")
    );
    if (cli::detail::is_chinese(help_language)) {
        auto formatter = application.get_formatter();
        formatter->label("Usage", "用法");
        formatter->label("OPTIONS", "选项");
        formatter->label("Options", "选项");
        formatter->label("SUBCOMMANDS", "子命令");
        formatter->label("Subcommands", "子命令");
        formatter->label("Positionals", "位置参数");
        formatter->label("REQUIRED", "必填");
    }
    application.formatter_fn(
        [formatter = application.get_formatter(), application_version](
            const CLI::App* command, std::string name, CLI::AppFormatMode mode
        ) {
            return application_version + "\n\n" +
                   formatter->make_help(command, std::move(name), mode);
        }
    );
    application.require_subcommand(1);

    auto* account = application.add_subcommand(
        "account", tr("Manage Microsoft account authentication")
    );
    account->require_subcommand(1);
    auto* account_login = account->add_subcommand(
        "login", tr("Authorize with Microsoft using the device code flow")
    );
    auto* account_logout = account->add_subcommand(
        "logout", tr("Remove the locally stored refresh token")
    );
    auto* inspect = application.add_subcommand(
        "inspect", tr("Inspect account, cloud, and synchronization state")
    );
    inspect->require_subcommand(1);
    auto* inspect_health = inspect->add_subcommand(
        "health", tr("Run local synchronization-state diagnostics")
    );
    auto* inspect_drives = inspect->add_subcommand(
        "drives", tr("List OneDrive drives available to the active account")
    );
    auto* inspect_shared = inspect->add_subcommand(
        "shared",
        tr("List items shared with the account and OneDrive shortcuts")
    );
    auto* inspect_sites = inspect->add_subcommand(
        "sites", tr("Find SharePoint sites and their document libraries")
    );
    auto* inspect_quota = inspect->add_subcommand(
        "quota", tr("Show storage quota for the configured drive")
    );
    auto* inspect_status = inspect->add_subcommand(
        "status", tr("Show read-only synchronization status")
    );
    auto* inspect_blocked = inspect->add_subcommand(
        "blocked",
        tr("List saved blocked items without connecting to OneDrive")
    );
    auto* inspect_storage = inspect->add_subcommand(
        "storage", tr("Show local synchronization storage usage")
    );
    auto* inspect_partials = inspect->add_subcommand(
        "partials", tr("List resumable and invalid partial downloads")
    );
    auto* inspect_files = inspect->add_subcommand(
        "files",
        tr("Check downloaded files against saved synchronization state")
    );
    auto* inspect_verify = inspect->add_subcommand(
        "verify", tr("Verify downloaded file metadata or content")
    );
    auto* inspect_config = inspect->add_subcommand(
        "config", tr("Show effective non-secret configuration")
    );
    auto* state = application.add_subcommand(
        "state", tr("Maintain saved synchronization state")
    );
    state->require_subcommand(1);
    auto* reset_cursor = state->add_subcommand(
        "reset-cursor",
        tr("Reset the cloud change cursor while preserving item state")
    );
    auto* cleanup_state = state->add_subcommand(
        "cleanup", tr("Remove invalid and orphaned partial download state")
    );
    auto* migrate_state = state->add_subcommand(
        "migrate",
        tr("Back up and upgrade local state databases without syncing")
    );
    auto* clear_state = state->add_subcommand(
        "clear",
        tr("Clear all saved synchronization state for the configured drive")
    );
    auto* transfer = application.add_subcommand(
        "transfer", tr("Transfer OneDrive files")
    );
    transfer->require_subcommand(1);
    auto* transfer_sync =
        transfer->add_subcommand("sync", tr("Synchronize OneDrive files"));
    auto* transfer_download = transfer->add_subcommand(
        "download", tr("Download one remote file by its Drive-relative path")
    );
    auto* transfer_restore = transfer->add_subcommand(
        "restore",
        tr("Restore missing local files without changing cloud content")
    );
    auto* transfer_watch = transfer->add_subcommand(
        "watch", tr("Watch for synchronization changes")
    );
    auto* monitor = application.add_subcommand(
        "monitor", tr("Explicitly control the running local Monitor")
    );
    monitor->require_subcommand(1);
    auto* monitor_status = monitor->add_subcommand(
        "status", tr("Query the running Monitor without connecting to OneDrive")
    );
    auto* monitor_trigger = monitor->add_subcommand(
        "trigger",
        tr("Request a synchronization check (acknowledgement, not completion)")
    );
    auto* monitor_stop = monitor->add_subcommand(
        "stop",
        tr("Request cancellation and shutdown (acknowledgement, not completion)"
        )
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
        std::pair{inspect_blocked, Operation::blocked},
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
        std::pair{transfer_restore, Operation::restore},
        std::pair{transfer_watch, Operation::monitor},
        std::pair{monitor_status, Operation::monitor_status},
        std::pair{monitor_trigger, Operation::monitor_trigger},
        std::pair{monitor_stop, Operation::monitor_stop},
    };

    const auto add_language_option = [&](CLI::App& command) {
        command
            .add_option(
                "--language",
                arguments.language,
                tr("Display language: auto, en, or zh_CN")
            )
            ->transform(CLI::CheckedTransformer(languages).description(""))
            ->type_name("{auto,en,zh_CN}")
            ->default_str("auto");
    };
    for (auto* group :
         {&application, account, inspect, state, transfer, monitor}) {
        add_language_option(*group);
    }

    const auto add_common_options = [&](CLI::App& command) {
        command
            .add_option(
                "--config",
                arguments.config_path,
                tr("Path to the configuration file")
            )
            ->type_name("PATH");
        command
            .add_option(
                "--log-level",
                arguments.log_level,
                tr("Minimum log level (overrides configuration)")
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
                tr("Write rotating logs to this file (overrides configuration)")
            )
            ->type_name("PATH");
        command
            .add_option(
                "--color",
                arguments.color_mode,
                tr("Color output override: auto, always, or never")
            )
            ->transform(CLI::CheckedTransformer(color_modes));
        add_language_option(command);
        command
            .add_option(
                "--output",
                arguments.output_mode,
                tr("Output format: text or json")
            )
            ->transform(CLI::CheckedTransformer(output_modes))
            ->default_str("text");
        command.add_flag(
            "--quiet",
            arguments.quiet,
            tr("Suppress informational and success output")
        );
    };
    for (const auto& [command, operation] : command_operations) {
        static_cast<void>(operation);
        add_common_options(*command);
    }
    account_login
        ->add_option(
            "--auth-flow",
            arguments.auth_flow,
            tr("Authentication flow: auto, auth-code, or device-code")
        )
        ->transform(CLI::CheckedTransformer(auth_flows).description(""))
        ->type_name("{auto,auth-code,device-code}")
        ->default_str("auto");
    clear_state->add_flag(
        "--yes",
        arguments.assume_yes,
        tr("Confirm the state clear without an interactive prompt")
    );
    cleanup_state->add_flag(
        "--dry-run",
        arguments.force_dry_run,
        tr("Show cleanup candidates without removing files or state")
    );
    cleanup_state->add_flag(
        "--yes",
        arguments.assume_yes,
        tr("Confirm removal of invalid partial files and state")
    );
    migrate_state->add_flag(
        "--dry-run",
        arguments.force_dry_run,
        tr("List database versions and upgrade candidates without changing them"
        )
    );
    migrate_state->add_flag(
        "--yes",
        arguments.assume_yes,
        tr("Confirm database backups and upgrades")
    );
    transfer_sync->add_flag(
        "--dry-run",
        arguments.force_dry_run,
        tr("Show synchronization inputs without changing remote files")
    );
    transfer_sync->add_flag(
        "--force-large-delete",
        arguments.force_large_delete,
        tr("Allow this sync to exceed the configured remote deletion limit")
    );
    for (const auto& [command, operation] : command_operations) {
        if (!operation_capabilities(operation).supports_tui) {
            continue;
        }
        command
            ->add_option(
                "--ui", arguments.ui_mode, tr("Interface: auto, console, or tui")
            )
            ->transform(CLI::CheckedTransformer(ui_modes).description(""))
            ->type_name("{auto,console,tui}")
            ->default_str("auto");
        command
            ->add_option(
                "--theme",
                arguments.tui_theme,
                tr("TUI theme: hacker, ocean, amber, or synthwave")
            )
            ->transform(CLI::CheckedTransformer(tui_themes).description(""))
            ->type_name("{hacker,ocean,amber,synthwave}")
            ->default_str("hacker");
    }
    transfer_download
        ->add_option(
            "REMOTE_PATH",
            arguments.remote_download_path,
            tr("Drive-relative path of the remote file")
        )
        ->required();
    transfer_download->add_flag(
        "--dry-run",
        arguments.force_dry_run,
        tr("Show the single-file download plan without changing local state")
    );
    transfer_restore->add_option(
        "REMOTE_PATH",
        arguments.restore_path,
        tr("Optional Drive-relative file or directory; omit for the sync scope")
    );
    transfer_restore->add_flag(
        "--dry-run",
        arguments.force_dry_run,
        tr("List missing local files without downloading or changing sync state"
        )
    );
    inspect_sites->add_option(
        "QUERY",
        arguments.site_query,
        tr("Optional SharePoint site search query, URL, or hostname:/sites/path"
        )
    );
    inspect_blocked->add_option(
        "--reason",
        arguments.blocked_reason,
        tr("Only show this exact reason code")
    );
    inspect_blocked->add_option(
        "--search",
        arguments.blocked_search,
        tr("Search paths, reason codes, and messages (ASCII case-insensitive)")
    );
    inspect_files->add_option(
        "PATH",
        arguments.inspect_path,
        tr("Optional Drive-relative file or directory path")
    );
    inspect_files
        ->add_option(
            "--status",
            arguments.inspect_file_status,
            tr("Only show files with this status")
        )
        ->check(CLI::IsMember(
            {"ok", "missing", "modified", "type-changed", "outside-root"}
        ));
    inspect_verify->add_option(
        "PATH",
        arguments.inspect_path,
        tr("Optional Drive-relative file or directory path")
    );
    inspect_verify
        ->add_option(
            "--mode",
            arguments.verify_mode,
            tr("Verification mode: metadata or content")
        )
        ->check(CLI::IsMember({"metadata", "content"}))
        ->default_str("metadata");

    if (argc < 2) {
        std::cout << application.help();
        result.exit_code = exit_code::success;
        return result;
    }
    try {
        application.parse(argc, argv);
    } catch (const CLI::RequiredError& error) {
        if (auto extras = application.remaining(true); !extras.empty()) {
            static_cast<void>(
                application.exit(CLI::ExtrasError{std::move(extras)})
            );
        } else {
            static_cast<void>(application.exit(error));
        }
        result.exit_code = exit_code::usage_error;
        return result;
    } catch (const CLI::ParseError& error) {
        const int parsed_exit_code = application.exit(error);
        result.exit_code = parsed_exit_code == exit_code::success ?
            exit_code::success : exit_code::usage_error;
        return result;
    }

    for (const auto& [command, operation] : command_operations) {
        if (*command) {
            arguments.operation = operation;
            break;
        }
    }
    return result;
}

} // namespace onedrive::app::detail
