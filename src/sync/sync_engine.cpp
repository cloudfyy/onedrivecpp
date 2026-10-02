#include "onedrive/sync/sync_engine.hpp"

#include "download_recovery.hpp"
#include "download_transaction.hpp"
#include "filesystem_metadata.hpp"
#include "local_filesystem.hpp"
#include "sync_plan.hpp"

#include <spdlog/spdlog.h>

#include <chrono>
#include <filesystem>
#include <format>
#include <iostream>
#include <optional>
#include <stdexcept>

namespace onedrive::sync {
namespace {

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
    const std::string& drive_id
) {
    const auto upsert_count =
        plan.directory_count() + plan.download_count();
    std::cout << "Remote delta contains " << plan.change_count()
              << " changes (" << upsert_count << " upserts, "
              << plan.removal_count() << " removals).\n";
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
    std::cout << "Synchronization plan:\n"
              << "  create directories: " << plan.directory_count() << '\n'
              << "  download files:     " << plan.download_count() << '\n'
              << "  download bytes:     " << plan.download_bytes() << '\n'
              << "  local removals:     0\n";
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

void execute_plan(
    detail::SyncPlan& plan,
    const std::filesystem::path& sync_root,
    const std::string& drive_id,
    graph::GraphClient& graph,
    storage::ItemStore& items,
    const detail::FilesystemMetadata& metadata
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
            state = detail::download_atomically(
                graph,
                items,
                item,
                state,
                destination,
                metadata
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
}

}  // namespace

SyncEngine::SyncEngine(
    const config::Config& config,
    graph::GraphClient& graph,
    storage::ItemStore& items,
    metrics::Metrics& metrics
)
    : config_{config}, graph_{graph}, items_{items}, metrics_{metrics} {}

int SyncEngine::synchronize() const {
    const auto started_at = std::chrono::steady_clock::now();
    const auto record_result = [this, started_at](bool success) {
        metrics_.record_sync_run(
            success,
            std::chrono::steady_clock::now() - started_at
        );
    };

    try {
        std::filesystem::path sync_root = config_.sync_directory;
        std::optional<detail::FilesystemMetadata> metadata;
        if (config_.dry_run) {
            std::cout << "Dry run configuration:\n"
                      << "  sync directory:  " << config_.sync_directory << '\n'
                      << "  state directory: " << config_.state_directory << '\n'
                      << "  drive id:        " << config_.drive_id << '\n'
                      << "  throttle retries: "
                      << config_.graph_maximum_throttle_retries << '\n'
                      << "  throttle delay:   "
                      << config_.graph_initial_throttle_delay.count() << '-'
                      << config_.graph_maximum_throttle_delay.count()
                      << " seconds\n"
                      << "  tracked items:   " << items_.size() << '\n';
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
        auto plan = detail::SyncPlan::build(
            graph_.list_delta(previous_delta_link),
            config_.drive_id,
            config_.sync_directory,
            !previous_delta_link.has_value()
        );
        report_plan(plan, config_.drive_id);

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
            execute_plan(
                plan,
                sync_root,
                config_.drive_id,
                graph_,
                items_,
                *metadata
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
        } else {
            spdlog::info(
                "Synchronization state update completed in {} milliseconds",
                std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count()
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
