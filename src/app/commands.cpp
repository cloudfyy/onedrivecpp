#include "commands.hpp"
#include "discover.hpp"
#include "info.hpp"

#include "onedrive/account/account_state.hpp"
#include "onedrive/app/options.hpp"
#include "onedrive/auth/device_auth.hpp"
#include "onedrive/auth/token_store.hpp"
#include "onedrive/graph/graph_client.hpp"
#include "onedrive/http/http_client.hpp"
#include "onedrive/metrics/metrics.hpp"
#include "onedrive/monitor/monitor.hpp"
#include "onedrive/storage/item_database.hpp"
#include "onedrive/storage/item_store.hpp"
#include "onedrive/sync/download/single_file.hpp"
#include "onedrive/sync/core/engine.hpp"
#include <spdlog/spdlog.h>

#include <format>
#include <string>
#include <utility>

namespace onedrive::app::detail {
namespace {

void show_database_result(
    const cli::Console& console, const storage::DatabaseIntegrityResult& result
) {
    console.section(
        "database_integrity",
        "Synchronization state database:",
        {
            {.label = "path:", .key = "path", .value = result.path.string()},
            {
                .label = "status:",
                .key = "status",
                .value = result.healthy              ? "healthy"
                         : result.migration_required ? "upgrade-required"
                                                     : "unhealthy",
            },
            {
                .label = "version:",
                .key = "schema_version",
                .value = result.schema_version
                             ? std::to_string(*result.schema_version)
                             : "unknown",
            },
            {
                .label = "latest:",
                .key = "latest_schema_version",
                .value = std::to_string(result.latest_schema_version),
            },
            {.label = "detail:", .key = "detail", .value = result.detail},
        }
    );
}

int migrate_state(
    const config::Config& config,
    const Arguments& arguments,
    const cli::Console& console
) {
    const auto results =
        storage::diagnose_state_databases(config.state_directory);
    bool invalid = false;
    bool pending = false;
    for (const auto& result : results) {
        show_database_result(console, result);
        invalid = invalid || (!result.healthy && !result.migration_required);
        pending = pending || result.migration_required;
    }
    if (invalid) {
        console.message(
            cli::MessageKind::error,
            "database_migration_failed",
            "Migration refused: one or more databases are unhealthy or have "
            "unsupported versions. No databases were upgraded."
        );
        return 1;
    }
    if (!pending || arguments.force_dry_run || config.dry_run) {
        console.message(
            cli::MessageKind::success,
            "database_migration_preview",
            pending ? "Dry run: listed databases require an upgrade; no "
                      "databases or backups were changed."
                    : "No databases require an upgrade."
        );
        return 0;
    }
    if (!arguments.assume_yes) {
        if (console.output_mode() == cli::OutputMode::json) {
            console.message(
                cli::MessageKind::error,
                "confirmation_required",
                "Database migration requires --yes with JSON output."
            );
            return 1;
        }
        if (!console.confirm(
                "database_migration_confirmation",
                "Back up and upgrade all listed outdated databases? "
                "Type 'migrate' to confirm: ",
                "migrate"
            )) {
            console.message(
                cli::MessageKind::warning,
                "database_migration_cancelled",
                "Database migration cancelled."
            );
            return 1;
        }
    }
    for (const auto& result : results) {
        if (!result.migration_required) {
            continue;
        }
        try {
            const auto backup = storage::migrate_state_database(result.path);
            console.section(
                "database_migrated",
                "Database upgrade completed:",
                {
                    {.label = "path:",
                     .key = "path",
                     .value = result.path.string()},
                    {.label = "from:",
                     .key = "previous_schema_version",
                     .value = std::to_string(*result.schema_version)},
                    {.label = "version:",
                     .key = "schema_version",
                     .value = std::to_string(result.latest_schema_version)},
                    {.label = "backup:",
                     .key = "backup_path",
                     .value = backup.string()},
                }
            );
        } catch (const std::exception& error) {
            console.message(
                cli::MessageKind::error,
                "database_migration_failed",
                "Cannot upgrade '" + result.path.string() + "': " + error.what()
            );
            return 1;
        }
    }
    console.message(
        cli::MessageKind::success,
        "database_migration_completed",
        "Database upgrades completed; no local or remote files were "
        "synchronized."
    );
    return 0;
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
            "Microsoft device authorization failed: {}", tokens.error().message
        );
        return 1;
    }

    const auto identity = graph::fetch_drive_identity(
        *transport, tokens->access_token, graph_options(config)
    );
    const auto paths = account::AccountState::activate(
        config.state_directory, identity, tokens->refresh_token
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

} // namespace

int execute_command(
    const Arguments& arguments,
    config::Config config,
    const RuntimeFactory& runtime_factory,
    const cli::Console& console
) {
    const auto operation = arguments.operation;
    if (operation == Operation::migrate_state) {
        return migrate_state(config, arguments, console);
    }
    if (operation == Operation::authenticate) {
        spdlog::info("Starting Microsoft authentication");
        if (config::has_broad_auth_scope(config.auth_scope)) {
            constexpr std::string_view warning{
                "WARNING: Authentication requests broad organizational "
                "file or SharePoint access. Keep Files.ReadWrite.All, "
                "Sites.Read.All, and Sites.ReadWrite.All only when the "
                "configured Drive or discovery workflow requires them."
            };
            spdlog::warn("{}", warning);
            console.message(
                cli::MessageKind::warning,
                "broad_oauth_scopes",
                std::string{warning}
            );
        }
        return authenticate(config, runtime_factory, console);
    }
    if (operation == Operation::logout) {
        spdlog::info("Removing locally saved authentication");
        const bool removed =
            account::AccountState::find_active_token_directory(
                config.state_directory
            ) &&
            runtime_factory.create_token_store(config)->remove_refresh_token();
        console.message(
            cli::MessageKind::success,
            "logout_completed",
            removed ? "Saved authentication removed."
                    : "No saved authentication was present."
        );
        return 0;
    }
    if (operation == Operation::diagnose) {
        const auto results =
            storage::diagnose_state_databases(config.state_directory);
        if (results.empty()) {
            console.message(
                cli::MessageKind::success,
                "database_integrity",
                "No synchronization state databases were found."
            );
            return 0;
        }

        bool healthy = true;
        bool invalid = false;
        for (const auto& result : results) {
            show_database_result(console, result);
            healthy = healthy && result.healthy;
            invalid =
                invalid || (!result.healthy && !result.migration_required);
        }
        console.message(
            healthy   ? cli::MessageKind::success
            : invalid ? cli::MessageKind::error
                      : cli::MessageKind::warning,
            healthy   ? "database_integrity_passed"
            : invalid ? "database_integrity_failed"
                      : "database_upgrade_required",
            healthy ? "All synchronization state databases passed full "
                      "integrity and schema checks."
            : invalid
                ? "One or more synchronization state databases failed "
                  "integrity or schema checks."
                : "Database upgrade required; run 'state migrate --dry-run'."
        );
        return healthy ? 0 : 1;
    }
    if (operation == Operation::drives) {
        return show_drives(config, runtime_factory, console);
    }
    if (operation == Operation::shared) {
        return show_shared(config, runtime_factory, console);
    }
    if (operation == Operation::sites) {
        return show_sites(
            config, runtime_factory, console, arguments.site_query
        );
    }
    if (operation == Operation::quota) {
        return show_quota(config, runtime_factory, console);
    }
    if (operation == Operation::status) {
        return show_status(config, runtime_factory, console);
    }
    if (operation == Operation::storage) {
        return show_storage(config, runtime_factory, console);
    }
    if (operation == Operation::partials) {
        return show_partials(config, runtime_factory, console);
    }
    if (operation == Operation::files) {
        return show_files(
            config,
            runtime_factory,
            console,
            arguments.inspect_path,
            arguments.inspect_file_status
        );
    }
    if (operation == Operation::verify) {
        return verify_files(
            config,
            runtime_factory,
            console,
            arguments.inspect_path,
            arguments.verify_mode
        );
    }
    if (operation == Operation::config) {
        return show_config(config, arguments.config_path, console);
    }
    if (operation == Operation::cleanup_state) {
        return cleanup_state(
            config,
            runtime_factory,
            console,
            arguments.force_dry_run,
            arguments.assume_yes
        );
    }
    if (operation == Operation::reset_cursor ||
        operation == Operation::clear_state) {
        const bool clear_all = operation == Operation::clear_state;
        auto graph = runtime_factory.create_graph_client(config);
        const auto identity = graph->drive_identity();
        config.drive_id = identity.drive_id;
        const auto& confirmation_drive_reference =
            identity.configured_drive_id.empty() ? identity.drive_id
                                                 : identity.configured_drive_id;
        const auto display_drive =
            confirmation_drive_reference == identity.drive_id
                ? "'" + identity.drive_id + "'"
                : "'" + confirmation_drive_reference + "' (" +
                      identity.drive_id + ")";
        if (clear_all && !arguments.assume_yes) {
            console.message(
                cli::MessageKind::warning,
                "full_state_clear_warning",
                "WARNING: This will remove all saved item snapshots, the "
                "Delta cursor, and pending-download recovery records for "
                "drive " +
                    display_drive + "."
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
                        confirmation_drive_reference + "' to confirm: ",
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
        if (clear_all) {
            spdlog::warn(
                "Clearing all synchronization state for drive '{}'",
                config.drive_id
            );
            auto items = runtime_factory.create_item_store(config, identity);
            items->open();
            const auto cleared = items->clear(config.drive_id);
            spdlog::warn(
                "Full synchronization state clear completed for drive "
                "'{}': "
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
            "Resetting synchronization state for drive '{}'", config.drive_id
        );
        auto items = runtime_factory.create_item_store(config, identity);
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
                ": saved cursor " + (removed ? "removed" : "not present") + "."
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
        operation == Operation::monitor ? "filesystem monitor"
                                        : "synchronization",
        config.dry_run ? " dry run" : ""
    );
    auto graph = runtime_factory.create_graph_client(config);
    const auto identity = graph->drive_identity();
    config.drive_id = identity.drive_id;
    config.sync_data_directory = account::AccountState::drive_data_directory(
        config.sync_data_directory, identity
    );
    if (operation == Operation::download && config.dry_run) {
        return sync::plan_single_file_download(
            config, arguments.remote_download_path, *graph, console
        );
    }
    auto items = runtime_factory.create_item_store(config, identity);
    items->open();
    if (operation == Operation::download) {
        spdlog::info(
            "Starting single-file download for '{}'",
            arguments.remote_download_path
        );
        return sync::download_single_file(
            config, arguments.remote_download_path, *graph, *items, console
        );
    }
    auto metrics = runtime_factory.create_metrics(config, identity);
    monitor::SyncCallback synchronize{
        [&config, &graph, &items, &metrics, &console] {
            return sync::SyncEngine{config, *graph, *items, *metrics, &console}
                .synchronize();
        }
    };
    if (operation == Operation::monitor) {
        console.message(
            cli::MessageKind::success,
            "monitor_ready",
            "Monitoring local and Microsoft Graph changes for: " +
                config.sync_data_directory.string()
        );
        console.message(
            cli::MessageKind::information,
            "monitor_status",
            config.monitor_websocket_enabled
                ? std::format(
                      "Local changes settle for {} milliseconds; remote "
                      "WebSocket notifications trigger Delta "
                      "synchronization, with Graph polling every {} seconds "
                      "as fallback.",
                      config.monitor_settle_delay.count(),
                      config.monitor_poll_interval.count()
                  )
                : std::format(
                      "Local changes settle for {} milliseconds; remote "
                      "WebSocket notifications are disabled, and Graph is "
                      "polled every {} seconds.",
                      config.monitor_settle_delay.count(),
                      config.monitor_poll_interval.count()
                  )
        );
        return runtime_factory
            .create_monitor(config, std::move(synchronize), *graph)
            ->run(console.ui_mode() == cli::UiMode::tui);
    }
    return synchronize();
}

} // namespace onedrive::app::detail
