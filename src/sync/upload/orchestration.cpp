#include "sync/upload/orchestration.hpp"
#include "sync/upload/directory.hpp"
#include "sync/upload/file.hpp"
#include "sync/upload/planning.hpp"
#include "sync/upload/remote_delete.hpp"
#include "sync/upload/remote_move.hpp"

#include "onedrive/util/unique_file_descriptor.hpp"
#include "onedrive/events/observer.hpp"
#include "onedrive/util/path_security.hpp"
#include "util/typestate.hpp"
#include "sync/filesystem/operations.hpp"
#include "sync/filesystem/metadata.hpp"
#include "sync/filesystem/safe_sync_root.hpp"
#include "sync/filesystem/traversal.hpp"
#include "sync/filter/remote_path.hpp"
#include "sync/filter/selective.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <format>
#include <mutex>
#include <optional>
#include <stop_token>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <fcntl.h>
#include <unistd.h>

namespace onedrive::sync::detail {
namespace {

using util::StateTransaction;
using util::TransactionState;
using util::TransactionStateFor;
using util::transition_transaction;

std::vector<UploadCandidate> discover_uploads(
    const SafeSyncRoot& sync_root,
    const std::string& drive_id,
    storage::ItemStore& items,
    const SyncList* sync_list,
    std::size_t& blocked,
    const events::Observer& observer,
    bool cleanup_suppressions,
    const std::unordered_set<std::string>& skipped_local_paths = {}
) {
    std::unordered_map<std::string, storage::ItemState> tracked;
    for (auto item : items.drive_items(drive_id)) {
        if (cleanup_suppressions) {
            std::error_code identity_error;
            const auto status = std::filesystem::symlink_status(
                item.local_path,
                identity_error
            );
            const bool expected_type =
                !identity_error &&
                (item.directory ?
                     std::filesystem::is_directory(status) :
                     std::filesystem::is_regular_file(status));
            if (expected_type) {
                const auto identity = sync_root.identity(
                    item.local_path,
                    filesystem_item_kind(item.directory)
                );
                if (item.local_device != identity.device ||
                    item.local_inode != identity.inode) {
                    item.local_device = identity.device;
                    item.local_inode = identity.inode;
                    items.upsert(item);
                }
            } else if (
                identity_error &&
                identity_error !=
                    std::errc::no_such_file_or_directory
            ) {
                throw std::runtime_error(
                    "cannot inspect tracked local identity '" +
                    item.local_path.string() + "': " +
                    identity_error.message()
                );
            }
        }
        tracked.emplace(
            item.local_path.lexically_normal().string(),
            std::move(item)
        );
    }
    std::unordered_set<std::string> blocked_ids;
    std::unordered_set<std::string> blocked_paths;
    for (const auto& item : items.blocked_items(drive_id)) {
        blocked_ids.insert(item.remote_id);
        blocked_paths.insert(item.remote_path);
    }
    std::unordered_map<std::string, storage::PendingUpload>
        resource_blocked_uploads;
    for (auto pending : items.pending_uploads(drive_id)) {
        if (!pending.failure_code.empty() &&
            (!pending.snapshot_path.empty() || pending.directory)) {
            resource_blocked_uploads.emplace(
                pending.local_path.lexically_normal().string(),
                std::move(pending)
            );
        }
    }
    std::unordered_set<std::string> suppressed_paths;
    std::unordered_set<std::string> suppressed_directories;
    for (const auto& suppression : items.upload_suppressions(drive_id)) {
        std::error_code suppression_error;
        const auto status = std::filesystem::symlink_status(
            suppression.local_path,
            suppression_error
        );
        bool matches = false;
        if (!suppression_error &&
            std::filesystem::is_regular_file(status)) {
            const auto identity = sync_root.identity(
                suppression.local_path,
                FilesystemItemKind::file
            );
            matches =
                identity.device == suppression.source_device &&
                identity.inode == suppression.source_inode;
        } else if (
            suppression_error &&
            suppression_error !=
                std::errc::no_such_file_or_directory
        ) {
            throw std::runtime_error(
                "cannot inspect selectively retained local file '" +
                suppression.local_path.string() + "': " +
                suppression_error.message()
            );
        }
        if (matches) {
            suppressed_paths.insert(
                suppression.local_path.lexically_normal().string()
            );
            for (auto parent = suppression.local_path.parent_path();
                 parent != sync_root.path() &&
                 parent.lexically_relative(sync_root.path()).
                     native().starts_with("..") == false;
                 parent = parent.parent_path()) {
                suppressed_directories.insert(
                    parent.lexically_normal().string()
                );
            }
        } else if (cleanup_suppressions) {
            items.remove_upload_suppression(
                drive_id,
                suppression.local_path
            );
        }
    }
    std::vector<UploadCandidate> uploads;
    walk_directory_tree(
        sync_root.path(),
        {
            .open = "cannot scan synchronization directory for uploads",
            .inspect = "cannot inspect local upload candidate",
            .advance = "cannot continue local upload scan",
        },
        [&](const std::filesystem::directory_entry& entry,
            const std::filesystem::file_status& status) {
            const auto path = entry.path();
            if (std::filesystem::is_symlink(status)) {
                ++blocked;
                observer.message(
                    events::MessageKind::warning,
                    "local_upload_blocked",
                    "Refusing to upload symbolic link '" + path.string() + "'."
                );
                return TreeWalkAction::continue_walk;
            }
            const bool directory = std::filesystem::is_directory(status);
            const bool regular_file = std::filesystem::is_regular_file(status);
            if ((!directory && !regular_file) || reserved_local_name(path)) {
                return TreeWalkAction::skip_subtree;
            }
            const auto relative = path.lexically_relative(sync_root.path());
            const auto remote_path = relative.generic_string();
            const auto kind =
                directory ? SyncItemKind::directory : SyncItemKind::file;
            std::optional<std::uint64_t> file_size;
            if (regular_file) {
                std::error_code error;
                file_size = entry.file_size(error);
                if (error) {
                    throw std::runtime_error(
                        "cannot read local upload candidate size '" +
                        path.string() + "': " + error.message()
                    );
                }
            }
            if (skipped_local_paths.contains(path.lexically_normal().string()
                )) {
                return TreeWalkAction::skip_subtree;
            }
            if (relative.empty() || relative.native().starts_with("..") ||
                (sync_list != nullptr &&
                 sync_list->excludes(remote_path, kind, file_size))) {
                return TreeWalkAction::skip_subtree;
            }
            if (sync_list != nullptr &&
                !sync_list->includes(remote_path, kind, file_size)) {
                return TreeWalkAction::continue_walk;
            }
            const auto previous =
                tracked.find(path.lexically_normal().string());
            if (const auto failed = resource_blocked_uploads.find(
                    path.lexically_normal().string()
                );
                failed != resource_blocked_uploads.end()) {
                ++blocked;
                observer.message(
                    events::MessageKind::warning,
                    "local_upload_resource_blocked",
                    "Upload remains deferred for '" + remote_path + "' (" +
                        failed->second.failure_code + ", attempt " +
                        std::to_string(failed->second.failure_attempt_count) +
                        "): " + failed->second.failure_message
                );
                return directory ? TreeWalkAction::skip_subtree
                                 : TreeWalkAction::continue_walk;
            }
            if ((!directory &&
                 suppressed_paths.contains(path.lexically_normal().string())) ||
                (directory && suppressed_directories.contains(
                                  path.lexically_normal().string()
                              ))) {
                return TreeWalkAction::continue_walk;
            }
            if (blocked_paths.contains(remote_path) ||
                (previous != tracked.end() &&
                 blocked_ids.contains(previous->second.remote_id))) {
                return directory ? TreeWalkAction::skip_subtree
                                 : TreeWalkAction::continue_walk;
            }
            if (directory && previous != tracked.end() &&
                !previous->second.directory) {
                ++blocked;
                observer.message(
                    events::MessageKind::warning,
                    "local_upload_blocked",
                    "Refusing to replace tracked remote file '" + remote_path +
                        "' with a local directory."
                );
                return TreeWalkAction::skip_subtree;
            }
            if (previous != tracked.end() && previous->second.directory) {
                if (directory) {
                    return TreeWalkAction::continue_walk;
                }
                ++blocked;
                observer.message(
                    events::MessageKind::warning,
                    "local_upload_blocked",
                    "Refusing to replace tracked remote directory '" +
                        remote_path + "' with a local file."
                );
                return TreeWalkAction::continue_walk;
            }
            if (directory || previous == tracked.end() ||
                !local_snapshot_matches(previous->second, path)) {
                uploads.push_back({
                    .path = path,
                    .remote_path = remote_path,
                    .previous = previous == tracked.end()
                                    ? std::nullopt
                                    : std::optional{previous->second},
                    .directory = directory,
                });
            }
            return TreeWalkAction::continue_walk;
        }
    );
    std::ranges::stable_sort(
        uploads,
        [](const UploadCandidate& left, const UploadCandidate& right) {
            if (left.directory != right.directory) {
                return left.directory;
            }
            if (!left.directory) {
                return false;
            }
            const auto left_depth = std::ranges::distance(left.path);
            const auto right_depth = std::ranges::distance(right.path);
            if (left_depth != right_depth) {
                return left_depth < right_depth;
            }
            return left.remote_path < right.remote_path;
        }
    );
    return uploads;
}

}  // namespace

UploadSummary upload_local_changes(
    const SafeSyncRoot& sync_root,
    const std::string& drive_id,
    graph::GraphClient& graph,
    storage::ItemStore& items,
    const FilesystemMetadata& metadata,
    const SyncList* sync_list,
    const events::Observer& observer,
    SyncCapabilities capabilities,
    RemoteDeletionGuard deletion_guard,
    std::size_t upload_concurrency
) {
    if (upload_concurrency == 0) {
        throw std::invalid_argument(
            "upload concurrency must be greater than zero"
        );
    }
    UploadSummary summary;
    auto moves = discover_local_moves(
        sync_root,
        drive_id,
        items,
        sync_list
    );
    summary.planned_moves = moves.moves.size();
    auto deletion_plan = capabilities.plans_remote_deletions() ?
        discover_deletions(
            drive_id, items, sync_list, moves.moved_remote_ids
        ) :
        DeletionPlan{};
    summary.planned_deletions = deletion_plan.operations.size();
    summary.affected_deletions = deletion_plan.affected_items;
    summary.large_delete_blocked = enforce_remote_deletion_limit(
        deletion_plan, deletion_guard, observer, capabilities.execution_mode()
    );
    if (capabilities.previews()) {
        const auto uploads = discover_uploads(
            sync_root,
            drive_id,
            items,
            sync_list,
            summary.blocked,
            observer,
            false,
            moves.moved_local_paths
        );
        summary.planned = static_cast<std::size_t>(
            std::ranges::count(
                uploads,
                false,
                &UploadCandidate::directory
            )
        );
        summary.planned_directories =
            uploads.size() - summary.planned;
        return summary;
    }

    auto move_parents = discover_move_parent_uploads(
        sync_root,
        drive_id,
        items,
        moves.moves
    );
    summary.planned_directories = move_parents.size();
    for (const auto& parent : move_parents) {
        if (!upload_directory(
                parent,
                sync_root,
                drive_id,
                graph,
                items,
                metadata,
                observer,
                summary
            )) {
            return summary;
        }
    }
    for (const auto& move : moves.moves) {
        const auto previous = items.find(drive_id, move.remote_id);
        if (!previous) {
            throw std::runtime_error(
                "local move has no tracked item: " + move.remote_id
            );
        }
        execute_new_remote_move(
            sync_root, move, *previous, graph, items
        );
        moves.moved_remote_ids.erase(move.remote_id);
        ++summary.moved;
        observer.message(
            events::MessageKind::information,
            "local_move_uploaded",
            "Moved remote item to '" + move.destination_remote_path + "'."
        );
    }
    deletion_plan = capabilities.plans_remote_deletions() ?
        discover_deletions(
            drive_id, items, sync_list, moves.moved_remote_ids
        ) :
        DeletionPlan{};
    summary.planned_deletions = deletion_plan.operations.size();
    summary.affected_deletions = deletion_plan.affected_items;
    summary.large_delete_blocked = enforce_remote_deletion_limit(
        deletion_plan, deletion_guard, observer, capabilities.execution_mode()
    );
    auto uploads = discover_uploads(
        sync_root, drive_id, items, sync_list, summary.blocked, observer, true
    );
    summary.planned = static_cast<std::size_t>(std::ranges::count(
        uploads,
        false,
        &UploadCandidate::directory
    ));
    summary.planned_directories +=
        uploads.size() - summary.planned;
    std::unordered_map<std::string, std::uint64_t>
        previous_failure_attempts;
    for (const auto& pending : items.pending_uploads(drive_id)) {
        if (!pending.failure_code.empty()) {
            previous_failure_attempts.emplace(
                pending.remote_path,
                pending.failure_attempt_count
            );
        }
    }
    for (const auto& deletion : deletion_plan.operations) {
        execute_new_remote_delete(deletion, graph, items);
        ++summary.deleted;
        observer.message(
            events::MessageKind::information,
            "local_item_deleted",
            "Deleted remote item '" + deletion.remote_path + "'."
        );
    }
    std::size_t first_file = 0;
    while (first_file < uploads.size() &&
           uploads[first_file].directory) {
        if (!upload_directory(
                uploads[first_file],
                sync_root,
                drive_id,
                graph,
                items,
                metadata,
                observer,
                summary
            )) {
            return summary;
        }
        ++first_file;
    }

    std::atomic_size_t next_upload{first_file};
    std::stop_source stop;
    std::mutex result_mutex;
    std::exception_ptr first_error;
    const auto previous_failure_count =
        [&](std::string_view remote_path) {
            const auto previous =
                std::as_const(previous_failure_attempts).find(
                    std::string{remote_path}
                );
            return previous == previous_failure_attempts.end() ?
                std::uint64_t{0} :
                previous->second;
        };
    const auto upload_file = [&](const UploadCandidate& upload) {
        const auto result = execute_file_upload(
            upload,
            sync_root,
            drive_id,
            graph,
            items,
            metadata,
            previous_failure_count(upload.remote_path),
            stop.get_token()
        );
        const std::scoped_lock lock{result_mutex};
        switch (result.status) {
        case FileUploadStatus::skipped:
            return;
        case FileUploadStatus::uploaded:
            ++summary.uploaded;
            observer.message(
                events::MessageKind::information,
                "local_item_uploaded",
                "Uploaded local file '" + upload.remote_path + "'."
            );
            return;
        case FileUploadStatus::blocked:
            ++summary.blocked;
            observer.message(
                events::MessageKind::warning,
                "local_upload_resource_blocked",
                "Deferred upload '" + upload.remote_path +
                    "': " + result.message
            );
            return;
        }
    };
    const auto worker = [&] {
        while (!stop.stop_requested()) {
            const auto index =
                next_upload.fetch_add(1, std::memory_order_relaxed);
            if (index >= uploads.size()) {
                return;
            }
            try {
                upload_file(uploads[index]);
            } catch (...) {
                {
                    const std::scoped_lock lock{result_mutex};
                    if (!first_error) {
                        first_error = std::current_exception();
                    }
                }
                stop.request_stop();
                return;
            }
        }
    };

    const auto file_count = uploads.size() - first_file;
    const auto worker_count = std::min(upload_concurrency, file_count);
    std::vector<std::jthread> workers;
    workers.reserve(worker_count);
    try {
        for (std::size_t index = 0; index < worker_count; ++index) {
            workers.emplace_back(worker);
        }
    } catch (...) {
        stop.request_stop();
        for (auto& thread : workers) {
            thread.join();
        }
        throw;
    }
    for (auto& thread : workers) {
        thread.join();
    }
    if (first_error) {
        std::rethrow_exception(first_error);
    }
    return summary;
}

}  // namespace onedrive::sync::detail
