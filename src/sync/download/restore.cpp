#include "onedrive/sync/download/restore.hpp"

#include "onedrive/exit_code.hpp"
#include "onedrive/sync/blocked_policy.hpp"
#include "onedrive/sync/cancellation.hpp"
#include "onedrive/util/mount.hpp"
#include "onedrive/util/path_security.hpp"
#include "sync/core/downloads.hpp"
#include "sync/core/item_ops.hpp"
#include "sync/download/space.hpp"
#include "sync/download/target.hpp"
#include "sync/filesystem/metadata.hpp"
#include "sync/filesystem/operations.hpp"
#include "sync/filesystem/safe_sync_root.hpp"
#include "sync/filter/remote_path.hpp"
#include "sync/filter/selective.hpp"

#include <algorithm>
#include <exception>
#include <filesystem>
#include <format>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace onedrive::sync {
namespace {

bool selected(std::string_view path, std::string_view scope) {
    return scope.empty() || path == scope ||
           detail::remote_path_is_descendant(path, scope);
}

bool overlaps(std::string_view left, std::string_view right) {
    return left == right || detail::remote_path_is_descendant(left, right) ||
           detail::remote_path_is_descendant(right, left);
}

bool within(
    const std::filesystem::path& path, const std::filesystem::path& ancestor
) {
    const auto relative = path.lexically_relative(ancestor);
    return !relative.empty() && !relative.is_absolute() &&
           *relative.begin() != "..";
}

bool missing(const std::filesystem::path& path) {
    std::error_code error;
    const auto status = std::filesystem::symlink_status(path, error);
    if (error && error != std::errc::no_such_file_or_directory) {
        throw std::filesystem::filesystem_error(
            "cannot inspect restore destination", path, error
        );
    }
    return status.type() == std::filesystem::file_type::not_found;
}

} // namespace

int restore_missing_files(
    const config::Config& config,
    const std::string& remote_path,
    graph::GraphClient& graph,
    storage::ItemStore& items,
    const events::Observer& observer,
    const std::stop_token& stop_token
) {
    throw_if_cancelled(stop_token);
    util::require_sync_mount(
        config.sync_data_directory, config.sync_data_mount_point
    );
    const auto root = util::normalized_absolute(config.sync_data_directory);
    if (!remote_path.empty()) {
        static_cast<void>(detail::local_path_for(root, remote_path));
    }
    observer.sync_context(SyncMode::download_only, config.dry_run);
    observer.sync_phase(events::SyncPhase::preparing);
    const auto filter = detail::SyncList::configured({
        .rules_path = config.sync_list,
        .sync_root = root,
        .include_root_files = config.sync_root_files,
        .nosync_enabled = config.nosync_enabled,
        .dotfiles = config.dotfiles,
        .maximum_file_size_bytes = config.maximum_file_size_bytes,
    });
    // Include unchanged files without consuming the synchronization cursor.
    const auto delta = graph.list_delta(
        std::nullopt,
        [&](std::size_t pages, std::size_t count, util::ProgressState state) {
            observer.delta_progress(pages, count, state);
        },
        stop_token
    );
    throw_if_cancelled(stop_token);
    const auto pending_downloads = items.pending_downloads(config.drive_id);
    const auto pending_uploads = items.pending_uploads(config.drive_id);
    const auto pending_moves = items.pending_moves(config.drive_id);
    const auto pending_remote_moves =
        items.pending_remote_moves(config.drive_id);
    const auto blocked = items.blocked_items(config.drive_id);
    const auto pending_deletes = items.pending_deletes(config.drive_id);
    std::vector<engine_detail::DownloadTask> downloads;
    std::vector<storage::ItemState> directories;
    std::unordered_set<std::string> remote_ids;
    std::unordered_set<std::string> destinations;
    std::uint64_t total_bytes = 0;
    std::size_t skipped = 0;
    std::size_t conflicts = 0;
    bool found_scope = remote_path.empty();
    const auto report_conflict = [&](const std::string& path,
                                     const std::string& reason) {
        ++conflicts;
        observer.blocked_item(path, "restore_conflict", reason);
    };
    for (const auto& item : delta.changes) {
        throw_if_cancelled(stop_token);
        if (item.root || !selected(item.remote_path, remote_path)) {
            continue;
        }
        found_scope = true;
        if (item.deleted) {
            report_conflict(
                item.remote_path,
                "The cloud item is deleted; restore it from the OneDrive "
                "recycle bin first."
            );
            continue;
        }
        remote_ids.insert(item.id);
        const auto destination = detail::local_path_for(root, item.remote_path);
        if (item.size < 0) {
            throw std::runtime_error(
                "negative remote file size in restore plan"
            );
        }
        const auto kind = item.directory ? detail::SyncItemKind::directory
                                         : detail::SyncItemKind::file;
        const auto size =
            item.directory
                ? std::optional<std::uint64_t>{}
                : std::optional{static_cast<std::uint64_t>(item.size)};
        if (filter.excludes(item.remote_path, kind, size) ||
            !filter.includes(item.remote_path, kind, size)) {
            ++skipped;
            continue;
        }
        if (!destinations.insert(destination.string()).second) {
            throw std::runtime_error(
                "multiple cloud items have the same restore destination: " +
                destination.string()
            );
        }
        util::reject_symlink_components(
            destination.parent_path(), "restore parent"
        );
        if (!missing(destination)) {
            const auto status = std::filesystem::symlink_status(destination);
            if ((item.directory && std::filesystem::is_directory(status)) ||
                (!item.directory && std::filesystem::is_regular_file(status))) {
                ++skipped;
            } else {
                report_conflict(
                    item.remote_path,
                    "Local destination has a conflicting type."
                );
            }
            continue;
        }
        if (item.malware || !item.unsupported_format.empty()) {
            report_conflict(
                item.remote_path, "Cloud item is unsafe or unsupported."
            );
            continue;
        }
        if (const auto* blocker =
                blocked_recovery(blocked, item.id, item.remote_path)) {
            report_conflict(item.remote_path, blocker->reason_message);
            continue;
        }
        const bool pending =
            std::ranges::any_of(
                pending_downloads,
                [&](const auto& operation) {
                    return operation.item.remote_id == item.id ||
                           overlaps(
                               operation.item.remote_path, item.remote_path
                           );
                }
            ) ||
            std::ranges::any_of(
                pending_uploads,
                [&](const auto& operation) {
                    return overlaps(operation.remote_path, item.remote_path);
                }
            ) ||
            std::ranges::any_of(
                pending_moves,
                [&](const auto& operation) {
                    return operation.remote_id == item.id ||
                           operation.source_path == destination ||
                           operation.destination_path == destination ||
                           (operation.directory &&
                            (within(destination, operation.source_path) ||
                             within(destination, operation.destination_path)));
                }
            ) ||
            std::ranges::any_of(
                pending_remote_moves, [&](const auto& operation) {
                    return operation.remote_id == item.id ||
                           overlaps(
                               operation.source_remote_path, item.remote_path
                           ) ||
                           overlaps(
                               operation.destination_remote_path,
                               item.remote_path
                           );
                }
            );
        if (pending) {
            report_conflict(
                item.remote_path,
                "An overlapping installation, upload or move journal requires "
                "resolution before restoration; it was preserved."
            );
            continue;
        }
        auto state = detail::item_state_for(config.drive_id, item, destination);
        observer.section(
            "restore_item",
            item.directory ? "Restore missing directory:"
                           : "Restore missing file:",
            {
                {.label = "remote path:",
                 .key = "remote_path",
                 .value = item.remote_path},
                {.label = "local path:",
                 .key = "local_path",
                 .value = destination.string()},
                {.label = "size:",
                 .key = "size",
                 .value = std::to_string(item.size)},
            }
        );
        if (item.directory) {
            directories.push_back(std::move(state));
        } else {
            if (static_cast<std::uint64_t>(item.size) >
                std::numeric_limits<std::uint64_t>::max() - total_bytes) {
                throw std::overflow_error("restore download size overflow");
            }
            total_bytes += static_cast<std::uint64_t>(item.size);
            downloads.push_back({
                .item = item,
                .state = std::move(state),
                .destination = destination,
                .destination_baseline = {},
            });
        }
    }
    for (const auto& previous : items.drive_items(config.drive_id)) {
        const auto kind = previous.directory ? detail::SyncItemKind::directory
                                             : detail::SyncItemKind::file;
        const auto size =
            previous.directory || previous.size < 0
                ? std::optional<std::uint64_t>{}
                : std::optional{static_cast<std::uint64_t>(previous.size)};
        if (!previous.remote_path.empty() &&
            selected(previous.remote_path, remote_path) &&
            !filter.excludes(previous.remote_path, kind, size) &&
            filter.includes(previous.remote_path, kind, size) &&
            !remote_ids.contains(previous.remote_id) &&
            missing(detail::local_path_for(root, previous.remote_path))) {
            found_scope = true;
            report_conflict(
                previous.remote_path,
                "The previously synchronized item is unavailable at the "
                "selected cloud path; check for moves or deletions in "
                "OneDrive and its recycle bin."
            );
        }
    }
    if (!found_scope) {
        throw std::runtime_error(
            "restore path was not found in the cloud: " + remote_path
        );
    }
    std::vector<storage::PendingDelete> cancelled_deletes;
    for (const auto& deletion : pending_deletes) {
        if (std::ranges::any_of(
                downloads,
                [&](const auto& task) {
                    return deletion.remote_id == task.item.id ||
                           deletion.remote_path == task.item.remote_path ||
                           (deletion.directory &&
                            selected(
                                task.item.remote_path, deletion.remote_path
                            ));
                }
            ) ||
            std::ranges::any_of(directories, [&](const auto& state) {
                return deletion.remote_id == state.remote_id ||
                       deletion.remote_path == state.remote_path ||
                       (deletion.directory &&
                        selected(state.remote_path, deletion.remote_path));
            })) {
            cancelled_deletes.push_back(deletion);
        }
    }
    observer.section(
        "restore_plan",
        "Missing-file restoration plan:",
        {
            {.label = "files:",
             .key = "files",
             .value = std::to_string(downloads.size())},
            {.label = "directories:",
             .key = "directories",
             .value = std::to_string(directories.size())},
            {.label = "bytes:",
             .key = "bytes",
             .value = std::to_string(total_bytes)},
            {.label = "skipped:",
             .key = "skipped",
             .value = std::to_string(skipped)},
            {.label = "blocked:",
             .key = "blocked",
             .value = std::to_string(conflicts)},
            {.label = "pending cloud deletes to cancel:",
             .key = "cancelled_deletes",
             .value = std::to_string(cancelled_deletes.size())},
        }
    );
    if (config.dry_run || (downloads.empty() && directories.empty())) {
        return conflicts == 0 ? exit_code::success : exit_code::blocked;
    }
    const auto sync_root =
        detail::prepare_sync_root(root, config.sync_permissions);
    detail::SafeSyncRoot safe_root{sync_root};
    const auto metadata = detail::FilesystemMetadata::detect(
        config.filesystem_metadata, sync_root
    );
    for (const auto& deletion : cancelled_deletes) {
        items.remove_pending_delete(config.drive_id, deletion.remote_id);
    }
    std::ranges::sort(directories, {}, &storage::ItemState::remote_path);
    for (auto& state : directories) {
        throw_if_cancelled(stop_token);
        safe_root.ensure_directory_tree(
            state.local_path, config.sync_permissions
        );
        const auto identity = safe_root.identity(
            state.local_path, detail::FilesystemItemKind::directory
        );
        state.local_device = identity.device;
        state.local_inode = identity.inode;
        items.upsert(state);
    }
    for (const auto& task : downloads) {
        safe_root.ensure_directory_tree(
            task.destination.parent_path(), config.sync_permissions
        );
    }
    detail::ItemOperationCoordinator operations;
    detail::DownloadSpaceCoordinator space{
        sync_root, detail::download_safety_reserve(total_bytes)
    };
    const auto batch = engine_detail::download_files(
        downloads,
        {},
        config.download_concurrency,
        graph,
        items,
        operations,
        space,
        metadata,
        observer,
        safe_root,
        config::LocalConflictPolicy::block,
        stop_token
    );
    std::size_t restored = 0;
    for (std::size_t index = 0; index < downloads.size(); ++index) {
        if (batch.errors[index]) {
            std::rethrow_exception(batch.errors[index]);
        }
        if (batch.conflicts[index]) {
            report_conflict(
                downloads[index].item.remote_path, *batch.conflicts[index]
            );
        }
        if (batch.states[index]) {
            ++restored;
            items.remove_blocked_item(
                config.drive_id, downloads[index].item.id
            );
        } else if (!batch.conflicts[index]) {
            throw std::runtime_error(
                "restoration did not complete for '" +
                downloads[index].item.remote_path + "'"
            );
        }
    }
    observer.message(
        events::MessageKind::information,
        "restore_completed",
        std::format(
            "Restored {} files and {} directories; {} blocked. No cloud "
            "files were changed and the synchronization cursor was preserved.",
            restored,
            directories.size(),
            conflicts
        )
    );
    return conflicts == 0 ? exit_code::success : exit_code::blocked;
}

} // namespace onedrive::sync
