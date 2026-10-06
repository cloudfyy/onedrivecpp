#include "sync/core/plan_execute.hpp"

#include "onedrive/cli/console.hpp"
#include "sync/core/delta_plan.hpp"
#include "sync/core/downloads.hpp"
#include "sync/core/item_ops.hpp"
#include "sync/core/local_move.hpp"
#include "sync/core/reporting.hpp"
#include "sync/core/remote_delete.hpp"
#include "sync/core/transfer_order.hpp"
#include "sync/download/space.hpp"
#include "sync/download/target.hpp"
#include "sync/filesystem/metadata.hpp"
#include "sync/filesystem/operations.hpp"
#include "sync/filesystem/safe_sync_root.hpp"

#include <spdlog/spdlog.h>

#include <filesystem>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace onedrive::sync::engine_detail {

ExecutionSummary execute_plan(
    detail::SyncPlan& plan,
    const detail::SafeSyncRoot& safe_root,
    const std::string& drive_id,
    graph::GraphClient& graph,
    storage::ItemStore& items,
    const detail::FilesystemMetadata& metadata,
    const cli::Console& console,
    std::size_t download_concurrency,
    config::TransferOrder transfer_order,
    config::LocalConflictPolicy local_conflict,
    config::SyncPermissionsMode permissions
) {
    const auto& sync_root = safe_root.path();
    detail::ItemOperationCoordinator operations;
    const auto removed_count = engine_detail::execute_removals(
        plan,
        safe_root,
        drive_id,
        items,
        operations,
        console
    );
    auto move_summary = engine_detail::execute_moves(
        plan,
        safe_root,
        drive_id,
        items,
        operations,
        console,
        permissions
    );
    std::unordered_set<std::string> blocked_ids =
        std::move(move_summary.blocked);
    std::vector<std::string> blocked_directories;
    for (std::size_t index = 0; index < plan.blocked_count(); ++index) {
        const auto& blocked = plan.blocked(index);
        blocked_ids.insert(blocked.remote_id);
        if (blocked.directory) {
            blocked_directories.push_back(blocked.remote_path);
        }
    }

    std::size_t prepared_directory_count = 0;
    for (std::size_t index = 0; index < plan.directory_count(); ++index) {
        const auto& item = plan.directory(index);
        if (blocked_ids.contains(item.id)) {
            continue;
        }
        if (below_blocked_directory(item.remote_path, blocked_directories)) {
            plan.block(
                item,
                "blocked_by_parent",
                "a parent remote directory is blocked"
            );
            blocked_directories.push_back(item.remote_path);
            engine_detail::report_blocked(
                plan.blocked(plan.blocked_count() - 1),
                console
            );
            continue;
        }
        try {
            auto& state = plan.state_for(item.id);
            safe_root.ensure_directory_tree(
                state.local_path,
                permissions
            );
            const auto identity =
                safe_root.identity(state.local_path, true);
            state.local_device = identity.device;
            state.local_inode = identity.inode;
            ++prepared_directory_count;
        } catch (const detail::LocalPathConflictError& error) {
            plan.block(item, "local_path_conflict", error.what());
            blocked_directories.push_back(item.remote_path);
            engine_detail::report_blocked(
                plan.blocked(plan.blocked_count() - 1),
                console
            );
        } catch (const detail::SafePathConflictError& error) {
            plan.block(item, "local_path_conflict", error.what());
            blocked_directories.push_back(item.remote_path);
            engine_detail::report_blocked(
                plan.blocked(plan.blocked_count() - 1),
                console
            );
        }
    }

    std::size_t downloaded_count = 0;
    std::size_t reused_count = 0;
    std::vector<engine_detail::DownloadTask> download_tasks;
    download_tasks.reserve(plan.download_count());
    for (std::size_t index = 0; index < plan.download_count(); ++index) {
        const auto& item = plan.download(index);
        if (blocked_ids.contains(item.id)) {
            continue;
        }
        if (below_blocked_directory(item.remote_path, blocked_directories)) {
            plan.block(
                item,
                "blocked_by_parent",
                "a parent remote directory is blocked"
            );
            engine_detail::report_blocked(
                plan.blocked(plan.blocked_count() - 1),
                console
            );
            continue;
        }
        auto& state = plan.state_for(item.id);
        const auto destination = state.local_path;
        try {
            safe_root.ensure_directory_tree(
                destination.parent_path(),
                permissions
            );
        } catch (const detail::LocalPathConflictError& error) {
            plan.block(item, "local_path_conflict", error.what());
            engine_detail::report_blocked(
                plan.blocked(plan.blocked_count() - 1),
                console
            );
            continue;
        } catch (const detail::SafePathConflictError& error) {
            plan.block(item, "local_path_conflict", error.what());
            engine_detail::report_blocked(
                plan.blocked(plan.blocked_count() - 1),
                console
            );
            continue;
        }
        if (std::filesystem::is_symlink(
                std::filesystem::symlink_status(destination)
            )) {
            plan.block(
                item,
                "local_path_conflict",
                "local file path is a symbolic link: " + destination.string()
            );
            engine_detail::report_blocked(
                plan.blocked(plan.blocked_count() - 1),
                console
            );
            continue;
        }

        const auto previous = items.find(drive_id, item.id);
        if (move_summary.reusable_files.contains(item.id)) {
            ++reused_count;
            continue;
        }
        const auto target_status =
            detail::inspect_download_target(previous, item, destination);
        bool preserve_local = target_status.preserve_local;
        if (preserve_local &&
            local_conflict == config::LocalConflictPolicy::block) {
            spdlog::warn(
                "Refusing to overwrite locally modified file '{}'",
                destination.string()
            );
            plan.block(
                item,
                "local_modification",
                "local modification conflict: " + destination.string()
            );
            engine_detail::report_blocked(
                plan.blocked(plan.blocked_count() - 1),
                console
            );
            continue;
        }

        if (target_status.current_remote_file) {
            spdlog::debug(
                "Reusing completed download for '{}'",
                item.remote_path
            );
            state.local_path = destination;
            state.local_size = static_cast<std::int64_t>(
                std::filesystem::file_size(destination)
            );
            state.local_modified_ticks = detail::modified_ticks(destination);
            const auto identity =
                safe_root.identity(destination, false);
            state.local_device = identity.device;
            state.local_inode = identity.inode;
            ++reused_count;
        } else {
            detail::LocalFileBaseline baseline;
            try {
                baseline =
                    detail::capture_local_file_baseline(destination);
            } catch (const detail::LocalModificationConflictError& error) {
                plan.block(item, "local_modification", error.what());
                engine_detail::report_blocked(
                    plan.blocked(plan.blocked_count() - 1),
                    console
                );
                continue;
            } catch (const detail::LocalPathConflictError& error) {
                plan.block(item, "local_path_conflict", error.what());
                engine_detail::report_blocked(
                    plan.blocked(plan.blocked_count() - 1),
                    console
                );
                continue;
            }
            if (baseline.existed &&
                (!previous ||
                 baseline.size != previous->local_size ||
                 baseline.modified_ticks !=
                     previous->local_modified_ticks)) {
                if (local_conflict ==
                    config::LocalConflictPolicy::backup) {
                    preserve_local = true;
                    spdlog::info(
                        "Preparing safeBackup for local conflict '{}'",
                        destination.string()
                    );
                } else {
                    const std::string reason =
                        "local file changed before downloading: " +
                        destination.string();
                    plan.block(item, "local_modification", reason);
                    engine_detail::report_blocked(
                        plan.blocked(plan.blocked_count() - 1),
                        console
                    );
                    continue;
                }
            }
            spdlog::info(
                "Downloading '{}' ({} bytes)",
                item.remote_path,
                item.size
            );
            download_tasks.push_back({
                .item = item,
                .state = state,
                .destination = destination,
                .destination_baseline = std::move(baseline),
                .preserve_local = preserve_local,
            });
        }
    }
    detail::order_transfers(
        download_tasks,
        transfer_order,
        [](const engine_detail::DownloadTask& task) {
            return task.item.size;
        },
        [](const engine_detail::DownloadTask& task) -> const std::string& {
            return task.item.name;
        }
    );

    spdlog::info(
        "Executing {} downloads with concurrency {}",
        download_tasks.size(),
        download_concurrency
    );
    std::uintmax_t transfer_bytes = 0;
    for (const auto& task : download_tasks) {
        transfer_bytes += static_cast<std::uintmax_t>(task.item.size);
    }
    detail::DownloadSpaceCoordinator space{
        sync_root,
        detail::download_safety_reserve(transfer_bytes)
    };
    auto downloads = engine_detail::download_files(
        download_tasks,
        download_concurrency,
        graph,
        items,
        operations,
        space,
        metadata,
        console,
        safe_root,
        local_conflict
    );
    for (const auto& error : downloads.errors) {
        if (error) {
            console.end_download_progress();
            std::rethrow_exception(error);
        }
    }
    for (std::size_t index = 0; index < download_tasks.size(); ++index) {
        const auto& conflict = downloads.conflicts[index];
        if (conflict.has_value()) {
            plan.block(
                download_tasks[index].item,
                "local_modification",
                conflict.value()
            );
            engine_detail::report_blocked(
                plan.blocked(plan.blocked_count() - 1),
                console
            );
            continue;
        }
        auto state = std::move(downloads.states[index]);
        if (!state) {
            console.end_download_progress();
            throw std::runtime_error(
                "download execution stopped before all earlier tasks completed"
            );
        }
        plan.state_for(download_tasks[index].item.id) =
            std::move(state).value();
        ++downloaded_count;
    }
    spdlog::info(
        "Download execution completed: {} downloaded, {} reused, {} "
        "directories prepared",
        downloaded_count,
        reused_count,
        prepared_directory_count
    );
    return {
        .downloaded = downloaded_count,
        .reused = reused_count,
        .directories = prepared_directory_count,
        .removed = removed_count,
        .moved = move_summary.moved,
    };
}

}  // namespace onedrive::sync::engine_detail
