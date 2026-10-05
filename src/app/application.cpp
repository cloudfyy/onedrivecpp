#include "onedrive/app/application.hpp"

#include "onedrive/account/account_state.hpp"
#include "onedrive/app/runtime_options.hpp"
#include "onedrive/app/runtime_factory.hpp"
#include "runtime_preflight.hpp"
#include "onedrive/auth/device_auth.hpp"
#include "onedrive/auth/token_store.hpp"
#include "onedrive/cli/console.hpp"
#include "onedrive/config/config.hpp"
#include "onedrive/graph/graph_client.hpp"
#include "onedrive/http/http_client.hpp"
#include "onedrive/logging/logging.hpp"
#include "onedrive/metrics/metrics.hpp"
#include "onedrive/monitor/monitor.hpp"
#include "onedrive/storage/item_store.hpp"
#include "monitor/termination_signal_mask.hpp"
#include "onedrive/sync/download/single_file.hpp"
#include "onedrive/sync/core/engine.hpp"
#include "onedrive/version.hpp"

#include <CLI/CLI.hpp>
#include <spdlog/spdlog.h>

#include <cstdlib>
#include <exception>
#include <filesystem>
#include <format>
#include <iostream>
#include <string>

namespace onedrive::app {
namespace {

std::filesystem::path default_config_path() {
    if (const char* home = std::getenv("HOME"); home != nullptr) {
        return std::filesystem::path{home} / ".config/onedrive-cpp/config.toml";
    }
    return "/etc/onedrive-cpp/onedrive-cpp.toml";
}

int authenticate(
    const config::Config& config,
    const RuntimeFactory& runtime_factory,
    const cli::Console& console
) {
    auto transport = runtime_factory.create_http_transport(config);
    auto client = runtime_factory.create_device_auth_client(config, *transport);

    auto device_code = client->request_device_code();
    if (!device_code) {
        spdlog::error(
            "Microsoft device authorization request failed: {}",
            device_code.error().message
        );
        return 1;
    }

    if (!device_code->message.empty()) {
        console.message(
            cli::MessageKind::information,
            "device_authorization",
            device_code->message
        );
    } else {
        console.message(
            cli::MessageKind::information,
            "device_authorization",
            "Open " + device_code->verification_uri + " and enter code " +
                device_code->user_code
        );
    }
    console.message(
        cli::MessageKind::information,
        "authorization_wait",
        "Waiting for authorization..."
    );

    auto tokens = client->poll_for_token(*device_code);
    if (!tokens) {
        spdlog::error(
            "Microsoft device authorization failed: {}",
            tokens.error().message
        );
        return 1;
    }

    const auto identity = graph::fetch_drive_identity(
        *transport,
        tokens->access_token,
        graph_options(config)
    );
    const auto paths = account::AccountState::activate(
        config.state_directory,
        identity,
        tokens->refresh_token
    );
    spdlog::info("Microsoft authentication succeeded");
    console.message(
        cli::MessageKind::success,
        "authentication_succeeded",
        "Authentication succeeded. Refresh token saved to " +
            (paths.token_directory / "refresh_token").string()
    );
    return 0;
}

}  // namespace

Application::Application(
    gsl::not_null<const RuntimeFactory*> runtime_factory
)
    : runtime_factory_{runtime_factory} {}

int Application::run(int argc, char* argv[]) {
    std::filesystem::path config_path = default_config_path();
    bool force_dry_run = false;
    bool clear_all_state = false;
    bool assume_yes = false;
    std::string log_level{"info"};
    std::string log_file;
    std::string color_mode{"auto"};
    std::string output_mode{"text"};
    bool quiet = false;
    std::string remote_download_path;

    CLI::App cli{
        "A modern C++ OneDrive synchronization client",
        "onedrive-cpp"
    };
    cli.set_version_flag(
        "--version",
        std::string{"onedrive-cpp "} + build_info::version
    );
    cli.require_subcommand(1);

    auto* auth_command = cli.add_subcommand(
        "auth",
        "Authorize with Microsoft using the device code flow"
    );
    auto* logout_command = cli.add_subcommand(
        "logout",
        "Remove the locally stored refresh token"
    );
    auto* reset_state_command = cli.add_subcommand(
        "reset-state",
        "Reset the Microsoft Graph delta cursor for the configured drive"
    );
    auto* sync_command = cli.add_subcommand("sync", "Synchronize OneDrive files");
    auto* download_command = cli.add_subcommand(
        "download",
        "Download one remote file by its Drive-relative path"
    );
    auto* monitor_command =
        cli.add_subcommand("monitor", "Monitor for synchronization changes");

    const auto add_common_options =
        [
            &config_path,
            &log_level,
            &log_file,
            &color_mode,
            &output_mode,
            &quiet
        ](CLI::App& command) {
        command
            .add_option("--config", config_path, "Path to the configuration file")
            ->type_name("PATH");
        command
            .add_option("--log-level", log_level, "Minimum log level")
            ->check(CLI::IsMember(
                {"trace", "debug", "info", "warn", "error", "critical", "off"},
                CLI::ignore_case
            ))
            ->capture_default_str();
        command
            .add_option("--log-file", log_file, "Also write rotating logs to this file")
            ->type_name("PATH");
        command
            .add_option(
                "--color",
                color_mode,
                "Color output: auto, always, or never"
            )
            ->check(CLI::IsMember({"auto", "always", "never"}))
            ->capture_default_str();
        command
            .add_option(
                "--output",
                output_mode,
                "Output format: text or json"
            )
            ->check(CLI::IsMember({"text", "json"}))
            ->capture_default_str();
        command.add_flag(
            "--quiet",
            quiet,
            "Suppress informational and success output"
        );
    };
    add_common_options(*auth_command);
    add_common_options(*logout_command);
    add_common_options(*reset_state_command);
    add_common_options(*sync_command);
    add_common_options(*download_command);
    add_common_options(*monitor_command);
    auto* clear_all_option = reset_state_command->add_flag(
        "--clear-all",
        clear_all_state,
        "Clear all saved synchronization state for the configured drive"
    );
    reset_state_command
        ->add_flag(
            "--yes",
            assume_yes,
            "Confirm --clear-all without an interactive prompt"
        )
        ->needs(clear_all_option);
    sync_command->add_flag(
        "--dry-run",
        force_dry_run,
        "Show synchronization inputs without changing remote files"
    );
    download_command
        ->add_option(
            "REMOTE_PATH",
            remote_download_path,
            "Drive-relative path of the remote file"
        )
        ->required();
    download_command->add_flag(
        "--dry-run",
        force_dry_run,
        "Show the single-file download plan without changing local state"
    );

    if (argc < 2) {
        std::cout << cli.help();
        return 0;
    }

    try {
        cli.parse(argc, argv);
    } catch (const CLI::ParseError& error) {
        const int exit_code = cli.exit(error);
        return exit_code == 0 ? 0 : 2;
    }

    const cli::Console console{
        {
            .color = cli::Console::parse_color_mode(color_mode),
            .output = cli::Console::parse_output_mode(output_mode),
            .quiet = quiet,
        }
    };
    std::optional<logging::Session> logging_session;
    try {
        logging_session.emplace(
            logging::Options{
                .level = log_level,
                .file = log_file.empty() ?
                            std::nullopt :
                            std::optional<std::filesystem::path>{log_file},
            }
        );
        auto config = config::Config::load(config_path);
        config.dry_run = config.dry_run || force_dry_run;
        const auto operation =
            *auth_command ? detail::Operation::authenticate :
            *logout_command ? detail::Operation::logout :
            *reset_state_command ? detail::Operation::reset_state :
            *download_command ? detail::Operation::download :
            *monitor_command ? detail::Operation::monitor :
                               detail::Operation::synchronize;
        std::optional<monitor::detail::TerminationSignalMask>
            monitor_signal_mask;
        if (operation == detail::Operation::monitor) {
            monitor_signal_mask.emplace();
        }
        const detail::RuntimePreflight runtime_preflight{config, operation};

        if (*auth_command) {
            spdlog::info("Starting Microsoft authentication");
            if (config::has_broad_auth_scope(config.auth_scope)) {
                constexpr std::string_view warning{
                    "WARNING: Authentication requests broad organizational "
                    "file or SharePoint write access. Keep "
                    "Files.ReadWrite.All and Sites.ReadWrite.All only when "
                    "the configured Drive requires them."
                };
                spdlog::warn("{}", warning);
                console.message(
                    cli::MessageKind::warning,
                    "broad_oauth_scopes",
                    std::string{warning}
                );
            }
            return authenticate(config, *runtime_factory_, console);
        }
        if (*logout_command) {
            spdlog::info("Removing locally saved authentication");
            const bool removed =
                account::AccountState::find_active_token_directory(
                    config.state_directory
                ) &&
                runtime_factory_->create_token_store(config)
                    ->remove_refresh_token();
            console.message(
                cli::MessageKind::success,
                "logout_completed",
                removed ? "Saved authentication removed." :
                          "No saved authentication was present."
            );
            return 0;
        }
        if (*reset_state_command) {
            auto graph = runtime_factory_->create_graph_client(config);
            const auto identity = graph->drive_identity();
            config.drive_id = identity.drive_id;
            const auto& confirmation_drive_reference =
                identity.configured_drive_id.empty() ?
                    identity.drive_id :
                    identity.configured_drive_id;
            const auto display_drive =
                confirmation_drive_reference == identity.drive_id ?
                    "'" + identity.drive_id + "'" :
                    "'" + confirmation_drive_reference + "' (" +
                        identity.drive_id + ")";
            if (clear_all_state && !assume_yes) {
                console.message(
                    cli::MessageKind::warning,
                    "full_state_clear_warning",
                    "WARNING: This will remove all saved item snapshots, the "
                    "Delta cursor, and pending-download recovery records for "
                    "drive " + display_drive + "."
                );
                console.message(
                    cli::MessageKind::warning,
                    "full_state_clear_warning",
                    "Local files will not be deleted, but the next sync may "
                    "report local modification conflicts."
                );
                if (console.output_mode() == cli::OutputMode::json) {
                    console.message(
                        cli::MessageKind::error,
                        "confirmation_required",
                        "Interactive confirmation is unavailable with JSON "
                        "output; use --yes to confirm explicitly."
                    );
                    return 1;
                }
                if (!console.confirm(
                        "full_state_clear_confirmation",
                        "Type the configured drive reference '" +
                            confirmation_drive_reference +
                            "' to confirm: ",
                        confirmation_drive_reference
                    )) {
                    spdlog::warn(
                        "Full synchronization state clear cancelled for drive "
                        "'{}': confirmation did not match",
                        config.drive_id
                    );
                    console.message(
                        cli::MessageKind::warning,
                        "full_state_clear_cancelled",
                        "Full state clear cancelled."
                    );
                    return 1;
                }
            }
            if (clear_all_state) {
                spdlog::warn(
                    "Clearing all synchronization state for drive '{}'",
                    config.drive_id
                );
                auto items =
                    runtime_factory_->create_item_store(config, identity);
                items->open();
                const auto cleared = items->clear(config.drive_id);
                spdlog::warn(
                    "Full synchronization state clear completed for drive '{}': "
                    "{} item snapshots, {} pending downloads, {} partial "
                    "downloads, {} pending uploads, {} pending moves, {} "
                    "upload suppressions, and {} blocked items "
                    "removed; saved cursor {}",
                    config.drive_id,
                    cleared.items,
                    cleared.pending_downloads,
                    cleared.partial_downloads,
                    cleared.pending_uploads,
                    cleared.pending_moves,
                    cleared.upload_suppressions,
                    cleared.blocked_items,
                    cleared.delta_link ? "removed" : "not present"
                );
                console.message(
                    cli::MessageKind::success,
                    "full_state_clear_completed",
                    std::format(
                        "Cleared all synchronization state for drive {}: {} "
                        "item snapshots, {} pending downloads, {} partial "
                        "downloads, {} pending uploads, {} pending moves, {} "
                        "upload suppressions, and {} blocked items "
                        "removed; saved cursor {}.",
                        display_drive,
                        cleared.items,
                        cleared.pending_downloads,
                        cleared.partial_downloads,
                        cleared.pending_uploads,
                        cleared.pending_moves,
                        cleared.upload_suppressions,
                        cleared.blocked_items,
                        cleared.delta_link ? "removed" : "not present"
                    )
                );
                console.message(
                    cli::MessageKind::warning,
                    "local_files_preserved",
                    "Local files were not deleted. The next sync may report "
                    "local modification conflicts."
                );
                return 0;
            }
            spdlog::info(
                "Resetting synchronization state for drive '{}'",
                config.drive_id
            );
            auto items = runtime_factory_->create_item_store(config, identity);
            items->open();
            const bool removed = items->reset(config.drive_id);
            spdlog::info(
                "Synchronization cursor reset completed for drive '{}': saved "
                "cursor {}; item snapshots, pending downloads, partial "
                "downloads, pending uploads, pending moves, upload "
                "suppressions, and blocked items preserved; next sync will "
                "use an initial delta query",
                config.drive_id,
                removed ? "removed" : "not present"
            );
            console.message(
                cli::MessageKind::success,
                "state_cursor_reset",
                "Reset synchronization cursor for drive " + display_drive +
                    ": saved cursor " +
                    (removed ? "removed" : "not present") + "."
            );
            console.message(
                cli::MessageKind::information,
                "state_preserved",
                "Item snapshots, pending downloads, partial downloads, pending "
                "uploads, pending moves, upload suppressions, and blocked "
                "items were preserved."
            );
            console.message(
                cli::MessageKind::information,
                "initial_delta_scheduled",
                "The next sync will perform a full Microsoft Graph delta query."
            );
            return 0;
        }
        spdlog::info(
            "Starting {}{}",
            *monitor_command ? "filesystem monitor" : "synchronization",
            config.dry_run ? " dry run" : ""
        );
        auto graph = runtime_factory_->create_graph_client(config);
        const auto identity = graph->drive_identity();
        config.drive_id = identity.drive_id;
        config.sync_directory =
            account::AccountState::drive_data_directory(
                config.sync_directory,
                identity
            );
        if (*download_command && config.dry_run) {
            return sync::plan_single_file_download(
                config,
                remote_download_path,
                *graph,
                console
            );
        }
        auto items = runtime_factory_->create_item_store(config, identity);
        items->open();
        if (*download_command) {
            spdlog::info(
                "Starting single-file download for '{}'",
                remote_download_path
            );
            return sync::download_single_file(
                config,
                remote_download_path,
                *graph,
                *items,
                console
            );
        }
        auto metrics = runtime_factory_->create_metrics();
        monitor::SyncCallback synchronize{
            [&config, &graph, &items, &metrics, &console] {
                return sync::SyncEngine{
                    config,
                    *graph,
                    *items,
                    *metrics,
                    &console
                }.synchronize();
            }
        };
        if (*monitor_command) {
            console.message(
                cli::MessageKind::success,
                "monitor_ready",
                "Monitoring local and Microsoft Graph changes for: " +
                    config.sync_directory.string()
            );
            console.message(
                cli::MessageKind::information,
                "monitor_status",
                std::format(
                    "Local changes settle for {} milliseconds; Graph is "
                    "polled every {} seconds.",
                    config.monitor_settle_delay.count(),
                    config.monitor_poll_interval.count()
                )
            );
            return runtime_factory_->create_monitor(
                config,
                std::move(synchronize)
            )->run();
        }
        return synchronize();
    } catch (const std::exception& error) {
        if (const auto logger = spdlog::default_logger()) {
            logger->error("{}", error.what());
        } else {
            std::cerr << "onedrive-cpp: " << error.what() << '\n';
        }
        return 1;
    }
}

}  // namespace onedrive::app
