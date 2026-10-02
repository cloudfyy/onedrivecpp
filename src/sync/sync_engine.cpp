#include "onedrive/sync/sync_engine.hpp"

#include "onedrive/cli/console.hpp"
#include "download_recovery.hpp"
#include "download_transaction.hpp"
#include "filesystem_metadata.hpp"
#include "local_filesystem.hpp"
#include "sync_plan.hpp"

#include <spdlog/spdlog.h>

#include <chrono>
#include <filesystem>
#include <format>
#include <optional>
#include <stdexcept>

namespace onedrive::sync {
namespace {

struct ExecutionSummary {
    std::size_t downloaded{0};
    std::size_t reused{0};
    std::size_t directories{0};
};

std::filesystem::path prepare_sync_root(
    const std::filesystem::path& configured_root
) {
    if (std::filesystem::is_symlink(
            std::filesystem::symlink_status(configured_root)
        )) {
        throw std::runtime_error(
            "configured synchronization directory is a symbolic link: " +
            configured_root.string()
        );
    }
    const bool created =
        std::filesystem::create_directories(configured_root);
    const auto root = std::filesystem::weakly_canonical(configured_root);
    if (created) {
        spdlog::debug("Created synchronization root '{}'", root.string());
    }
    return root;
}

void report_plan(
    const detail::SyncPlan& plan,
    const std::string& drive_id,
    const cli::Console& console
) {
    const auto upsert_count =
        plan.directory_count() + plan.download_count();
    console.message(
        cli::MessageKind::information,
        "remote_delta",
        std::format(
            "Remote delta contains {} changes ({} upserts, {} removals).",
            plan.change_count(),
            upsert_count,
            plan.removal_count()
        )
    );
    spdlog::info(
        "Remote delta prepared for drive '{}': {} upserts, {} removals",
        drive_id,
        upsert_count,
        plan.removal_count()
    );
    spdlog::info(
        "Synchronization plan for drive '{}': {} directories, {} downloads, "
        "{} bytes, {} deferred local removals",
        drive_id,
        plan.directory_count(),
        plan.download_count(),
        plan.download_bytes(),
        plan.removal_count()
    );
    if (plan.removal_count() != 0) {
        spdlog::warn(
            "{} remote deletions will not remove local files in this release",
            plan.removal_count()
        );
    }
    console.section(
        "synchronization_plan",
        "Synchronization plan:",
        {
            {
                .label = "create directories:",
                .key = "create_directories",
                .value = std::to_string(plan.directory_count()),
            },
            {
                .label = "download files:",
                .key = "download_files",
                .value = std::to_string(plan.download_count()),
            },
            {
                .label = "download bytes:",
                .key = "download_bytes",
                .value = std::to_string(plan.download_bytes()),
            },
            {
                .label = "local removals:",
                .key = "local_removals",
                .value = "0",
            },
        }
    );
}

void check_download_capacity(
    const std::filesystem::path& sync_directory,
    std::uintmax_t required
) {
    const auto space = std::filesystem::space(
        std::filesystem::exists(sync_directory) ?
            sync_directory :
            sync_directory.parent_path()
    );
    spdlog::debug(
        "Download capacity check: {} bytes required, {} bytes available",
        required,
        space.available
    );
    if (required > space.available) {
        throw std::runtime_error(
            std::format(
                "synchronization requires {} bytes, but only {} bytes are "
                "available",
                required,
                space.available
            )
        );
    }
}

ExecutionSummary execute_plan(
    detail::SyncPlan& plan,
    const std::filesystem::path& sync_root,
    const std::string& drive_id,
    graph::GraphClient& graph,
    storage::ItemStore& items,
    const detail::FilesystemMetadata& metadata,
    const cli::Console& console
) {
    for (std::size_t index = 0; index < plan.directory_count(); ++index) {
        const auto& item = plan.directory(index);
        detail::ensure_directory_tree(
            sync_root,
            detail::local_path_for(sync_root, item.remote_path)
        );
    }

    std::size_t downloaded_count = 0;
    std::size_t reused_count = 0;
    for (std::size_t index = 0; index < plan.download_count(); ++index) {
        const auto& item = plan.download(index);
        const auto destination =
            detail::local_path_for(sync_root, item.remote_path);
        detail::ensure_directory_tree(sync_root, destination.parent_path());
        if (std::filesystem::is_symlink(
                std::filesystem::symlink_status(destination)
            )) {
            throw std::runtime_error(
                "local file path is a symbolic link: " + destination.string()
            );
        }

        const auto* previous = items.find(drive_id, item.id);
        const bool exists = std::filesystem::exists(destination);
        const bool snapshot_matches =
            exists && previous != nullptr &&
            detail::local_snapshot_matches(*previous, destination);
        const bool current_remote_file =
            snapshot_matches && previous->etag == item.etag;
        if (exists && !current_remote_file && !snapshot_matches) {
            spdlog::warn(
                "Refusing to overwrite locally modified file '{}'",
                destination.string()
            );
            throw std::runtime_error(
                "local modification conflict: " + destination.string()
            );
        }

        auto& state = plan.state_for(item.id);
        if (current_remote_file) {
            spdlog::debug(
                "Reusing completed download for '{}'",
                item.remote_path
            );
            state.local_path = destination;
            state.local_size = static_cast<std::int64_t>(
                std::filesystem::file_size(destination)
            );
            state.local_modified_ticks = detail::modified_ticks(destination);
            ++reused_count;
        } else {
            spdlog::info(
                "Downloading '{}' ({} bytes)",
                item.remote_path,
                item.size
            );
            const auto expected_size =
                static_cast<std::uint64_t>(item.size);
            std::uint64_t last_reported_percentage = 0;
            console.download_progress(
                item.remote_path,
                index + 1,
                plan.download_count(),
                0,
                expected_size,
                false
            );
            try {
                state = detail::download_atomically(
                    graph,
                    items,
                    item,
                    state,
                    destination,
                    metadata,
                    [&](std::uint64_t downloaded, std::uint64_t reported_total) {
                        const auto total =
                            expected_size == 0 ? reported_total : expected_size;
                        if (total == 0 || downloaded >= total) {
                            return;
                        }
                        const auto percentage = static_cast<std::uint64_t>(
                            static_cast<long double>(downloaded) * 100.0L /
                            static_cast<long double>(total)
                        );
                        if (percentage < last_reported_percentage + 5) {
                            return;
                        }
                        last_reported_percentage = percentage;
                        console.download_progress(
                            item.remote_path,
                            index + 1,
                            plan.download_count(),
                            downloaded,
                            total,
                            false
                        );
                    }
                );
            } catch (...) {
                console.end_download_progress();
                throw;
            }
            console.download_progress(
                item.remote_path,
                index + 1,
                plan.download_count(),
                expected_size,
                expected_size,
                true
            );
            ++downloaded_count;
        }
    }
    spdlog::info(
        "Download execution completed: {} downloaded, {} reused, {} "
        "directories prepared",
        downloaded_count,
        reused_count,
        plan.directory_count()
    );
    return {
        .downloaded = downloaded_count,
        .reused = reused_count,
        .directories = plan.directory_count(),
    };
}

}  // namespace

SyncEngine::SyncEngine(
    const config::Config& config,
    graph::GraphClient& graph,
    storage::ItemStore& items,
    metrics::Metrics& metrics,
    const cli::Console* console
)
    : config_{config},
      graph_{graph},
      items_{items},
      metrics_{metrics},
      console_{console} {}

int SyncEngine::synchronize() const {
    const auto started_at = std::chrono::steady_clock::now();
    const auto record_result = [this, started_at](bool success) {
        metrics_.record_sync_run(
            success,
            std::chrono::steady_clock::now() - started_at
        );
    };

    try {
        cli::Console fallback_console;
        const auto& console =
            console_ == nullptr ? fallback_console : *console_;
        std::filesystem::path sync_root = config_.sync_directory;
        std::optional<detail::FilesystemMetadata> metadata;
        if (config_.dry_run) {
            console.section(
                "dry_run_configuration",
                "Dry run configuration:",
                {
                    {
                        .label = "sync directory:",
                        .key = "sync_directory",
                        .value = config_.sync_directory.string(),
                    },
                    {
                        .label = "state directory:",
                        .key = "state_directory",
                        .value = config_.state_directory.string(),
                    },
                    {
                        .label = "drive id:",
                        .key = "drive_id",
                        .value = config_.drive_id,
                    },
                    {
                        .label = "throttle retries:",
                        .key = "throttle_retries",
                        .value = std::to_string(
                            config_.graph_maximum_throttle_retries
                        ),
                    },
                    {
                        .label = "throttle delay:",
                        .key = "throttle_delay",
                        .value = std::format(
                            "{}-{} seconds",
                            config_.graph_initial_throttle_delay.count(),
                            config_.graph_maximum_throttle_delay.count()
                        ),
                    },
                    {
                        .label = "tracked items:",
                        .key = "tracked_items",
                        .value = std::to_string(items_.size()),
                    },
                }
            );
            const auto pending = items_.pending_downloads(config_.drive_id);
            if (!pending.empty()) {
                spdlog::info(
                    "Dry run found {} pending downloads; recovery is deferred",
                    pending.size()
                );
            }
        } else {
            sync_root = prepare_sync_root(config_.sync_directory);
            metadata.emplace(detail::FilesystemMetadata::detect(
                config_.filesystem_metadata,
                sync_root
            ));
            detail::recover_pending_downloads(
                items_,
                sync_root,
                config_.drive_id,
                *metadata
            );
        }

        const auto previous_delta_link = items_.delta_link(config_.drive_id);
        spdlog::debug(
            "Preparing Microsoft Graph delta query for drive '{}': {} tracked "
            "items, saved cursor {}",
            config_.drive_id,
            items_.size(),
            previous_delta_link ? "present" : "absent"
        );
        console.message(
            cli::MessageKind::information,
            "delta_query_started",
            "Fetching Microsoft Graph changes..."
        );
        bool replace_drive_items = !previous_delta_link.has_value();
        graph::DeltaResult delta;
        try {
            delta = graph_.list_delta(previous_delta_link);
        } catch (const graph::DeltaCursorInvalidError& error) {
            spdlog::warn(
                "{}; retrying with a full Microsoft Graph delta query",
                error.what()
            );
            console.message(
                cli::MessageKind::warning,
                "delta_cursor_invalid",
                "The saved Microsoft Graph cursor is no longer valid; "
                "fetching the full remote state..."
            );
            delta = graph_.list_delta(std::nullopt);
            replace_drive_items = true;
        }
        auto plan = detail::SyncPlan::build(
            std::move(delta),
            config_.drive_id,
            config_.sync_directory,
            replace_drive_items
        );
        report_plan(plan, config_.drive_id, console);

        if (config_.dry_run) {
            spdlog::debug(
                "Dry run left synchronization state unchanged for drive '{}'",
                config_.drive_id
            );
        } else {
            check_download_capacity(
                config_.sync_directory,
                plan.download_bytes()
            );
            console.message(
                cli::MessageKind::information,
                "execution_started",
                "Executing synchronization plan..."
            );
            const auto summary = execute_plan(
                plan,
                sync_root,
                config_.drive_id,
                graph_,
                items_,
                *metadata,
                console
            );
            console.section(
                "execution_summary",
                "Synchronization summary:",
                {
                    {
                        .label = "downloaded:",
                        .key = "downloaded",
                        .value = std::to_string(summary.downloaded),
                    },
                    {
                        .label = "reused:",
                        .key = "reused",
                        .value = std::to_string(summary.reused),
                    },
                    {
                        .label = "directories prepared:",
                        .key = "directories_prepared",
                        .value = std::to_string(summary.directories),
                    },
                }
            );
            spdlog::debug(
                "Persisting remote delta for drive '{}'",
                config_.drive_id
            );
            items_.apply_delta(plan.release_state_delta());
        }

        record_result(true);
        const auto elapsed = std::chrono::steady_clock::now() - started_at;
        if (config_.dry_run) {
            spdlog::info(
                "Synchronization dry run completed in {} milliseconds",
                std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count()
            );
            console.message(
                cli::MessageKind::success,
                "sync_completed",
                std::format(
                    "Synchronization dry run completed in {} milliseconds",
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        elapsed
                    ).count()
                )
            );
        } else {
            spdlog::info(
                "Synchronization state update completed in {} milliseconds",
                std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count()
            );
            console.message(
                cli::MessageKind::success,
                "sync_completed",
                std::format(
                    "Synchronization state update completed in {} milliseconds",
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        elapsed
                    ).count()
                )
            );
        }
        return 0;
    } catch (...) {
        record_result(false);
        const auto elapsed = std::chrono::steady_clock::now() - started_at;
        spdlog::warn(
            "Synchronization failed after {} milliseconds",
            std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count()
        );
        throw;
    }
}

}  // namespace onedrive::sync
