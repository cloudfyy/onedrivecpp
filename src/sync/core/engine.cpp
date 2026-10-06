#include "onedrive/sync/core/engine.hpp"

#include "onedrive/cli/console.hpp"
#include "onedrive/util/path_security.hpp"
#include "sync/core/download_execution.hpp"
#include "sync/core/item_operation_coordinator.hpp"
#include "sync/core/local_move_execution.hpp"
#include "sync/core/operation_reporting.hpp"
#include "sync/core/remote_delete_execution.hpp"
#include "sync/core/plan.hpp"
#include "sync/core/transfer_order.hpp"
#include "sync/download/recovery.hpp"
#include "sync/download/space_coordinator.hpp"
#include "sync/download/target.hpp"
#include "sync/filesystem/local.hpp"
#include "sync/filesystem/metadata.hpp"
#include "sync/filesystem/safe_sync_root.hpp"
#include "sync/filter/remote_path.hpp"
#include "sync/filter/selective.hpp"
#include "sync/upload/local.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <exception>
#include <filesystem>
#include <format>
#include <optional>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <sys/stat.h>

namespace onedrive::sync {
namespace {

struct ExecutionSummary {
    std::size_t downloaded{0};
    std::size_t reused{0};
    std::size_t directories{0};
    std::size_t removed{0};
    std::size_t moved{0};
};

graph::RemoteItem remote_item(const storage::BlockedItem& item) {
    return {
        .id = item.remote_id,
        .name = item.name,
        .etag = item.etag,
        .ctag = item.ctag,
        .parent_id = item.parent_id,
        .remote_path = item.remote_path,
        .last_modified = item.last_modified,
        .size = item.size,
        .directory = item.directory,
        .deleted = item.deleted,
        .root = false,
        .malware = item.reason_code == "malware_detected",
        .content_hash = item.content_hash,
    };
}

void add_blocked_retries(
    graph::DeltaResult& delta,
    const std::vector<storage::BlockedItem>& blocked
) {
    std::unordered_set<std::string> changed_ids;
    changed_ids.reserve(delta.changes.size());
    for (const auto& item : delta.changes) {
        changed_ids.insert(item.id);
    }
    for (const auto& item : blocked) {
        if (!changed_ids.contains(item.remote_id)) {
            delta.changes.push_back(remote_item(item));
        }
    }
}

void add_full_refresh_deletions(
    graph::DeltaResult& delta,
    const std::vector<storage::ItemState>& tracked
) {
    std::unordered_set<std::string> remote_ids;
    remote_ids.reserve(delta.changes.size());
    for (const auto& item : delta.changes) {
        remote_ids.insert(item.id);
    }
    for (const auto& item : tracked) {
        if (!remote_ids.contains(item.remote_id)) {
            graph::RemoteItem deletion;
            deletion.id = item.remote_id;
            deletion.deleted = true;
            delta.changes.push_back(std::move(deletion));
        }
    }
}

void add_deleted_descendants(
    graph::DeltaResult& delta,
    const std::vector<storage::ItemState>& tracked
) {
    std::unordered_set<std::string> changed_ids;
    changed_ids.reserve(delta.changes.size());
    for (const auto& item : delta.changes) {
        changed_ids.insert(item.id);
    }
    std::vector<std::string> deleted_directories;
    for (const auto& change : delta.changes) {
        if (!change.deleted) {
            continue;
        }
        const auto previous = std::ranges::find(
            tracked,
            change.id,
            &storage::ItemState::remote_id
        );
        if (previous != tracked.end() && previous->directory) {
            deleted_directories.push_back(previous->remote_path);
        }
    }
    for (const auto& item : tracked) {
        const bool below_deleted_directory = std::ranges::any_of(
            deleted_directories,
            [&](const std::string& directory) {
                return detail::remote_path_is_descendant(
                    item.remote_path,
                    directory
                );
            }
        );
        if (changed_ids.contains(item.remote_id) ||
            !below_deleted_directory) {
            continue;
        }
        graph::RemoteItem deletion;
        deletion.id = item.remote_id;
        deletion.deleted = true;
        delta.changes.push_back(std::move(deletion));
        changed_ids.insert(item.remote_id);
    }
}

void add_moved_descendants(
    graph::DeltaResult& delta,
    const std::vector<storage::ItemState>& tracked
) {
    std::unordered_set<std::string> changed_ids;
    changed_ids.reserve(delta.changes.size());
    for (const auto& item : delta.changes) {
        changed_ids.insert(item.id);
    }

    std::vector<graph::RemoteItem> descendants;
    for (const auto& change : delta.changes) {
        if (change.deleted || !change.directory) {
            continue;
        }
        const auto previous = std::ranges::find(
            tracked,
            change.id,
            &storage::ItemState::remote_id
        );
        if (previous == tracked.end() ||
            previous->remote_path == change.remote_path) {
            continue;
        }
        for (const auto& item : tracked) {
            if (changed_ids.contains(item.remote_id) ||
                !detail::remote_path_is_descendant(
                    item.remote_path,
                    previous->remote_path
                )) {
                continue;
            }
            auto remote_path =
                change.remote_path +
                item.remote_path.substr(previous->remote_path.size());
            descendants.push_back({
                .id = item.remote_id,
                .name = item.name,
                .etag = item.etag,
                .ctag = item.ctag,
                .parent_id = item.parent_id,
                .remote_path = std::move(remote_path),
                .last_modified = item.last_modified,
                .size = item.size,
                .directory = item.directory,
                .content_hash = std::nullopt,
            });
            changed_ids.insert(item.remote_id);
        }
    }
    delta.changes.insert(
        delta.changes.end(),
        std::make_move_iterator(descendants.begin()),
        std::make_move_iterator(descendants.end())
    );
}

bool below_blocked_directory(
    std::string_view path,
    const std::vector<std::string>& blocked_directories
) {
    return std::ranges::any_of(
        blocked_directories,
        [path](const std::string& directory) {
            return detail::remote_path_is_descendant(path, directory);
        }
    );
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
            "Remote delta contains {} changes ({} upserts, {} removals, {} "
            "moves, {} blocked).",
            plan.change_count(),
            upsert_count,
            plan.removal_count(),
            plan.move_count(),
            plan.blocked_count()
        )
    );
    spdlog::info(
        "Remote delta prepared for drive '{}': {} upserts, {} removals, {} "
        "moves, {} blocked",
        drive_id,
        upsert_count,
        plan.removal_count(),
        plan.move_count(),
        plan.blocked_count()
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
        spdlog::info(
            "{} remote deletions are eligible for safe local execution",
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
                .label = "blocked items:",
                .key = "blocked_items",
                .value = std::to_string(plan.blocked_count()),
            },
            {
                .label = "local moves:",
                .key = "local_moves",
                .value = std::to_string(plan.move_count()),
            },
            {
                .label = "local removals:",
                .key = "local_removals",
                .value = std::to_string(plan.removal_count()),
            },
        }
    );
    for (std::size_t index = 0; index < plan.blocked_count(); ++index) {
        engine_detail::report_blocked(plan.blocked(index), console);
    }
}

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

}  // namespace

int SyncEngine::synchronize() const {
    const auto started_at = std::chrono::steady_clock::now();
    const auto record_result =
        [this, started_at](metrics::SyncRunOutcome outcome) {
        metrics_.record_sync_run(
            outcome,
            std::chrono::steady_clock::now() - started_at
        );
    };

    try {
        cli::Console fallback_console;
        const auto& console =
            console_ == nullptr ? fallback_console : *console_;
        std::filesystem::path sync_root =
            onedrive::util::normalized_absolute(config_->sync_directory);
        std::optional<detail::SafeSyncRoot> safe_root;
        std::optional<detail::FilesystemMetadata> metadata;
        if (config_->dry_run) {
            console.section(
                "dry_run_configuration",
                "Dry run configuration:",
                {
                    {
                        .label = "sync directory:",
                        .key = "sync_directory",
                        .value = config_->sync_directory.string(),
                    },
                    {
                        .label = "state directory:",
                        .key = "state_directory",
                        .value = config_->state_directory.string(),
                    },
                    {
                        .label = "drive id:",
                        .key = "drive_id",
                        .value = config_->drive_id,
                    },
                    {
                        .label = "throttle retries:",
                        .key = "throttle_retries",
                        .value = std::to_string(
                            config_->graph_maximum_throttle_retries
                        ),
                    },
                    {
                        .label = "throttle delay:",
                        .key = "throttle_delay",
                        .value = std::format(
                            "{}-{} seconds",
                            config_->graph_initial_throttle_delay.count(),
                            config_->graph_maximum_throttle_delay.count()
                        ),
                    },
                    {
                        .label = "download concurrency:",
                        .key = "download_concurrency",
                        .value = std::to_string(
                            config_->download_concurrency
                        ),
                    },
                    {
                        .label = "per-download rate:",
                        .key = "download_rate_limit",
                        .value = std::to_string(
                            config_->
                                download_maximum_rate_bytes_per_second
                        ),
                    },
                    {
                        .label = "total download rate:",
                        .key = "download_total_rate_limit",
                        .value = std::to_string(
                            config_->
                                download_maximum_total_rate_bytes_per_second
                        ),
                    },
                    {
                        .label = "upload concurrency:",
                        .key = "upload_concurrency",
                        .value = std::to_string(
                            config_->upload_concurrency
                        ),
                    },
                    {
                        .label = "upload chunk size:",
                        .key = "upload_chunk_size",
                        .value = std::to_string(
                            config_->upload_chunk_size_bytes
                        ),
                    },
                    {
                        .label = "per-upload rate:",
                        .key = "upload_rate_limit",
                        .value = std::to_string(
                            config_->
                                upload_maximum_rate_bytes_per_second
                        ),
                    },
                    {
                        .label = "total upload rate:",
                        .key = "upload_total_rate_limit",
                        .value = std::to_string(
                            config_->
                                upload_maximum_total_rate_bytes_per_second
                        ),
                    },
                    {
                        .label = "tracked items:",
                        .key = "tracked_items",
                        .value = std::to_string(items_.size()),
                    },
                }
            );
            const auto pending = items_.pending_downloads(config_->drive_id);
            if (!pending.empty()) {
                spdlog::info(
                    "Dry run found {} pending downloads; recovery is deferred",
                    pending.size()
                );
            }
            if (std::filesystem::is_directory(sync_root)) {
                safe_root.emplace(sync_root);
                metadata.emplace(
                    detail::FilesystemMetadata::from_detected_support(
                        config_->filesystem_metadata,
                        false
                    )
                );
            }
        } else {
            sync_root = detail::prepare_sync_root(
                sync_root,
                config_->sync_permissions
            );
            safe_root.emplace(sync_root);
            metadata.emplace(detail::FilesystemMetadata::detect(
                config_->filesystem_metadata,
                sync_root
            ));
            detail::recover_pending_downloads(
                items_,
                *safe_root,
                config_->drive_id,
                *metadata,
                config_->sync_permissions
            );
            if (config_->upload) {
                detail::recover_pending_remote_moves(
                    *safe_root,
                    config_->drive_id,
                    graph_,
                    items_,
                    console
                );
                detail::recover_pending_deletes(
                    config_->drive_id,
                    graph_,
                    items_,
                    console,
                    {
                        .maximum_affected_items =
                            config_->maximum_remote_deletions,
                        .force = config_->force_large_delete,
                    }
                );
                detail::recover_pending_uploads(
                    *safe_root,
                    config_->drive_id,
                    graph_,
                    items_,
                    *metadata,
                    console
                );
            }
        }

        const auto sync_list = config_->sync_list.has_value() ?
            std::optional{detail::SyncList::load(
                *config_->sync_list,
                config_->sync_root_files
            )} :
            std::nullopt;
        const std::string sync_filter_fingerprint =
            sync_list ? sync_list->fingerprint() : "";
        const auto previous_delta_link = items_.delta_link(config_->drive_id);
        const auto previous_sync_filter_fingerprint =
            items_.sync_filter_fingerprint(config_->drive_id);
        const bool sync_filter_changed =
            previous_delta_link.has_value() &&
            previous_sync_filter_fingerprint.value_or("") !=
                sync_filter_fingerprint;
        const auto query_delta_link = sync_filter_changed ?
            std::optional<std::string>{} :
            previous_delta_link;
        if (sync_filter_changed) {
            spdlog::info(
                "Selective synchronization rules changed; fetching the full "
                "remote state"
            );
            console.message(
                cli::MessageKind::information,
                "sync_filter_changed",
                "Selective synchronization rules changed; fetching the full "
                "remote state..."
            );
        }
        std::vector<std::string> snapshot_removals;
        if (sync_list) {
            spdlog::info(
                "Loaded {} selective synchronization rules from '{}' "
                "(root files: {})",
                sync_list->rule_count(),
                config_->sync_list->string(),
                config_->sync_root_files ? "included" : "rule-selected"
            );
        }
        spdlog::debug(
            "Preparing Microsoft Graph delta query for drive '{}': {} tracked "
            "items, saved cursor {}",
            config_->drive_id,
            items_.size(),
            query_delta_link ? "present" : "absent"
        );
        console.message(
            cli::MessageKind::information,
            "delta_query_started",
            "Fetching Microsoft Graph changes..."
        );
        auto apply_mode =
            query_delta_link.has_value() ?
                storage::DeltaApplyMode::merge :
                storage::DeltaApplyMode::replace;
        graph::DeltaResult delta;
        const auto delta_progress =
            [&console](std::size_t pages,
                       std::size_t items,
                       util::ProgressState state) {
                console.delta_progress(pages, items, state);
            };
        try {
            delta = graph_.list_delta(query_delta_link, delta_progress);
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
            delta = graph_.list_delta(std::nullopt, delta_progress);
            apply_mode = storage::DeltaApplyMode::replace;
        }
        const auto tracked_items =
            items_.drive_items(config_->drive_id);
        std::vector<storage::UploadSuppression> upload_suppressions;
        if (apply_mode == storage::DeltaApplyMode::replace) {
            add_full_refresh_deletions(
                delta,
                tracked_items
            );
        }
        add_deleted_descendants(delta, tracked_items);
        add_moved_descendants(delta, tracked_items);
        const auto previously_blocked =
            items_.blocked_items(config_->drive_id);
        if (apply_mode == storage::DeltaApplyMode::merge &&
            !previously_blocked.empty()) {
            add_blocked_retries(delta, previously_blocked);
            spdlog::debug(
                "Added {} blocked items to the synchronization retry plan",
                previously_blocked.size()
            );
        }
        if (sync_list) {
            std::unordered_set<std::string> blocked_ids;
            blocked_ids.reserve(previously_blocked.size());
            for (const auto& item : previously_blocked) {
                blocked_ids.insert(item.remote_id);
            }
            auto filtered = detail::filter_delta(
                std::move(delta),
                *sync_list,
                [&](std::string_view remote_id) {
                    return blocked_ids.contains(std::string{remote_id}) ||
                           items_.find(
                               config_->drive_id,
                               std::string{remote_id}
                           ).has_value();
                },
                apply_mode
            );
            spdlog::info(
                "Selective synchronization excluded {} remote changes",
                filtered.excluded
            );
            snapshot_removals = std::move(filtered.snapshot_removals);
            if (safe_root) {
                for (const auto& remote_id :
                     filtered.retained_remote_ids) {
                    const auto previous = std::ranges::find(
                        tracked_items,
                        remote_id,
                        &storage::ItemState::remote_id
                    );
                    if (previous == tracked_items.end() ||
                        previous->directory) {
                        continue;
                    }
                    std::error_code error;
                    const auto status = std::filesystem::symlink_status(
                        previous->local_path,
                        error
                    );
                    if (error ==
                        std::errc::no_such_file_or_directory) {
                        continue;
                    }
                    if (error) {
                        throw std::runtime_error(
                            "cannot inspect selectively retained local file '" +
                            previous->local_path.string() + "': " +
                            error.message()
                        );
                    }
                    if (!std::filesystem::is_regular_file(status)) {
                        continue;
                    }
                    const auto identity = safe_root->identity(
                        previous->local_path,
                        false
                    );
                    upload_suppressions.push_back({
                        .drive_id = config_->drive_id,
                        .remote_id = previous->remote_id,
                        .local_path = previous->local_path,
                        .source_device = identity.device,
                        .source_inode = identity.inode,
                    });
                }
            }
            delta = std::move(filtered.delta);
        }
        auto plan = detail::SyncPlan::build(
            std::move(delta),
            config_->drive_id,
            sync_root,
            apply_mode,
            sync_filter_fingerprint,
            std::move(snapshot_removals),
            tracked_items,
            std::move(upload_suppressions)
        );
        report_plan(plan, config_->drive_id, console);

        std::size_t blocked_count = plan.blocked_count();
        detail::UploadSummary upload_summary;
        if (config_->dry_run) {
            if (config_->upload && safe_root && metadata) {
                upload_summary = detail::upload_local_changes(
                    *safe_root,
                    config_->drive_id,
                    graph_,
                    items_,
                    *metadata,
                    sync_list ? &*sync_list : nullptr,
                    console,
                    true,
                    {
                        .maximum_affected_items =
                            config_->maximum_remote_deletions,
                        .force = config_->force_large_delete,
                    },
                    config_->upload_concurrency
                );
                blocked_count += upload_summary.blocked;
                console.section(
                    "upload_plan",
                    "Local upload plan:",
                    {
                        {
                            .label = "move remote items:",
                            .key = "move_remote_items",
                            .value =
                                std::to_string(upload_summary.planned_moves),
                        },
                        {
                            .label = "delete remote items:",
                            .key = "delete_remote_items",
                            .value =
                                std::to_string(upload_summary.planned_deletions
                                ),
                        },
                        {
                            .label = "affected tracked items:",
                            .key = "affected_remote_deletions",
                            .value =
                                std::to_string(upload_summary.affected_deletions
                                ),
                        },
                        {
                            .label = "large-delete limit:",
                            .key = "maximum_remote_deletions",
                            .value =
                                std::to_string(config_->maximum_remote_deletions
                                ),
                        },
                        {
                            .label = "large-delete blocked:",
                            .key = "large_delete_blocked",
                            .value = upload_summary.large_delete_blocked
                                         ? "true"
                                         : "false",
                        },
                        {
                            .label = "create directories:",
                            .key = "create_directories",
                            .value = std::to_string(
                                upload_summary.planned_directories
                            ),
                        },
                        {
                            .label = "upload files:",
                            .key = "upload_files",
                            .value = std::to_string(upload_summary.planned),
                        },
                        {
                            .label = "blocked:",
                            .key = "blocked",
                            .value = std::to_string(upload_summary.blocked),
                        },
                    }
                );
            }
            spdlog::debug(
                "Dry run left synchronization state unchanged for drive '{}'",
                config_->drive_id
            );
        } else {
            console.message(
                cli::MessageKind::information,
                "execution_started",
                "Executing synchronization plan..."
            );
            const auto summary = execute_plan(
                plan,
                *safe_root,
                config_->drive_id,
                graph_,
                items_,
                *metadata,
                console,
                config_->download_concurrency,
                config_->transfer_order,
                config_->local_conflict,
                config_->sync_permissions
            );
            spdlog::debug(
                "Persisting remote delta for drive '{}'",
                config_->drive_id
            );
            blocked_count = plan.blocked_count();
            items_.apply_delta(plan.release_state_delta());
            if (config_->upload) {
                upload_summary = detail::upload_local_changes(
                    *safe_root,
                    config_->drive_id,
                    graph_,
                    items_,
                    *metadata,
                    sync_list ? &*sync_list : nullptr,
                    console,
                    false,
                    {
                        .maximum_affected_items =
                            config_->maximum_remote_deletions,
                        .force = config_->force_large_delete,
                    },
                    config_->upload_concurrency
                );
                blocked_count += upload_summary.blocked;
            }
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
                    {
                        .label = "local moves:",
                        .key = "local_moves",
                        .value = std::to_string(summary.moved),
                    },
                    {
                        .label = "local removals:",
                        .key = "local_removals",
                        .value = std::to_string(summary.removed),
                    },
                    {
                        .label = "remote moves:",
                        .key = "remote_moves",
                        .value = std::to_string(upload_summary.moved),
                    },
                    {
                        .label = "remote removals:",
                        .key = "remote_removals",
                        .value = std::to_string(upload_summary.deleted),
                    },
                    {
                        .label = "remote items affected:",
                        .key = "remote_deletion_affected_items",
                        .value =
                            std::to_string(upload_summary.affected_deletions),
                    },
                    {
                        .label = "remote directories created:",
                        .key = "remote_directories_created",
                        .value =
                            std::to_string(upload_summary.created_directories),
                    },
                    {
                        .label = "uploaded:",
                        .key = "uploaded",
                        .value = std::to_string(upload_summary.uploaded),
                    },
                    {
                        .label = "blocked:",
                        .key = "blocked",
                        .value = std::to_string(blocked_count),
                    },
                }
            );
        }

        record_result(metrics::SyncRunOutcome::succeeded);
        const auto elapsed = std::chrono::steady_clock::now() - started_at;
        if (blocked_count != 0) {
            spdlog::warn(
                "Synchronization completed with {} blocked items in {} "
                "milliseconds",
                blocked_count,
                std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count()
            );
            console.message(
                cli::MessageKind::warning,
                "sync_completed_with_issues",
                std::format(
                    "Synchronization completed with {} blocked items in {} "
                    "milliseconds.",
                    blocked_count,
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        elapsed
                    ).count()
                )
            );
        } else if (config_->dry_run) {
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
        return blocked_count == 0 ? 0 : 2;
    } catch (...) {
        record_result(metrics::SyncRunOutcome::failed);
        const auto elapsed = std::chrono::steady_clock::now() - started_at;
        spdlog::warn(
            "Synchronization failed after {} milliseconds",
            std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count()
        );
        throw;
    }
}

}  // namespace onedrive::sync
