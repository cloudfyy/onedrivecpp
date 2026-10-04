#include "onedrive/sync/sync_engine.hpp"

#include "onedrive/cli/console.hpp"
#include "onedrive/path_security.hpp"
#include "download_recovery.hpp"
#include "download_integrity.hpp"
#include "download_progress.hpp"
#include "download_space_coordinator.hpp"
#include "download_transaction.hpp"
#include "filesystem_metadata.hpp"
#include "item_operation_coordinator.hpp"
#include "local_filesystem.hpp"
#include "local_upload.hpp"
#include "safe_sync_root.hpp"
#include "selective_sync.hpp"
#include "sync_plan.hpp"
#include "transfer_order.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <exception>
#include <filesystem>
#include <format>
#include <mutex>
#include <optional>
#include <stop_token>
#include <stdexcept>
#include <thread>
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

struct DownloadTask {
    graph::RemoteItem item;
    storage::ItemState state;
    std::filesystem::path destination;
    detail::LocalFileBaseline destination_baseline;
    bool preserve_local{false};
};

struct DownloadBatch {
    std::vector<std::optional<storage::ItemState>> states;
    std::vector<std::optional<std::string>> conflicts;
    std::vector<std::exception_ptr> errors;
};

DownloadBatch download_files(
    const std::vector<DownloadTask>& tasks,
    std::size_t concurrency,
    graph::GraphClient& graph,
    storage::ItemStore& items,
    detail::ItemOperationCoordinator& operations,
    detail::DownloadSpaceCoordinator& space,
    const detail::FilesystemMetadata& metadata,
    const cli::Console& console,
    const detail::SafeSyncRoot& sync_root,
    config::LocalConflictPolicy local_conflict
) {
    DownloadBatch batch{
        .states = std::vector<std::optional<storage::ItemState>>(
            tasks.size()
        ),
        .conflicts = std::vector<std::optional<std::string>>(tasks.size()),
        .errors = std::vector<std::exception_ptr>(tasks.size()),
    };
    if (tasks.empty()) {
        return batch;
    }

    std::atomic_size_t next_task{0};
    std::stop_source stop;
    std::mutex console_mutex;
    std::vector<std::uint64_t> task_downloaded(tasks.size());
    std::vector<bool> task_completed(tasks.size());
    std::uint64_t downloaded_bytes = 0;
    std::uint64_t total_bytes = 0;
    for (const auto& task : tasks) {
        total_bytes += static_cast<std::uint64_t>(task.item.size);
    }
    std::size_t completed_files = 0;
    unsigned last_reported_percentage = 0;
    detail::DownloadProgressEstimator progress_estimator;
    const auto report_progress =
        [&](std::size_t index, std::uint64_t downloaded, bool completed) {
            const std::scoped_lock lock{console_mutex};
            const auto expected_size =
                static_cast<std::uint64_t>(tasks[index].item.size);
            const auto current = std::min(downloaded, expected_size);
            if (current > task_downloaded[index]) {
                downloaded_bytes += current - task_downloaded[index];
                task_downloaded[index] = current;
            }
            if (completed && !task_completed[index]) {
                task_completed[index] = true;
                ++completed_files;
            }
            const auto byte_percentage =
                total_bytes == 0 ?
                    0U :
                    static_cast<unsigned>(
                        static_cast<long double>(downloaded_bytes) * 100.0L /
                        static_cast<long double>(total_bytes)
                    );
            const auto file_percentage = static_cast<unsigned>(
                completed_files * 100 / tasks.size()
            );
            const auto percentage = std::max(
                byte_percentage,
                file_percentage
            );
            const bool all_completed = completed_files == tasks.size();
            if (!all_completed &&
                percentage < last_reported_percentage + 1) {
                return;
            }
            last_reported_percentage = percentage;
            const auto metrics = progress_estimator.sample(
                downloaded_bytes,
                total_bytes
            );
            console.download_progress(
                completed_files,
                tasks.size(),
                downloaded_bytes,
                total_bytes,
                all_completed,
                metrics
            );
        };
    const auto worker = [&](const std::stop_token& thread_stop) {
        while (!thread_stop.stop_requested() && !stop.stop_requested()) {
            const std::size_t index =
                next_task.fetch_add(1, std::memory_order_relaxed);
            if (index >= tasks.size()) {
                return;
            }
            const auto& task = tasks[index];
            auto baseline = task.destination_baseline;
            bool preserve_local = task.preserve_local;
            const auto expected_size =
                static_cast<std::uint64_t>(task.item.size);
            try {
                auto operation = operations.acquire(
                    task.state.drive_id,
                    task.item.id
                );
                auto destination_operation =
                    operations.acquire_destination(task.destination);
                if (!detail::local_file_matches_baseline(
                        task.destination,
                        baseline
                    )) {
                    if (local_conflict ==
                        config::LocalConflictPolicy::block) {
                        throw detail::LocalModificationConflictError(
                            "local file changed before the destination "
                            "download lock was acquired: " +
                            task.destination.string()
                        );
                    }
                    baseline = detail::capture_local_file_baseline(
                        task.destination
                    );
                    preserve_local = baseline.existed;
                }
                batch.states[index].emplace(detail::commit_download(
                    items,
                    sync_root,
                    metadata,
                    detail::prepare_download(
                        graph,
                        items,
                        task.item,
                        task.state,
                        task.destination,
                        baseline,
                        metadata,
                        space,
                        stop.get_token(),
                        [&](std::uint64_t downloaded,
                            std::uint64_t reported_total) {
                            const auto total =
                                expected_size == 0 ?
                                    reported_total :
                                    expected_size;
                            if (total == 0 || downloaded >= total) {
                                return;
                            }
                            report_progress(index, downloaded, false);
                        }
                    ),
                    {
                        .local_conflict = local_conflict,
                        .preserve_local = preserve_local,
                        .backup_created =
                            [&](const std::filesystem::path& backup) {
                                const std::scoped_lock lock{console_mutex};
                                console.message(
                                    cli::MessageKind::warning,
                                    "local_conflict_backed_up",
                                    "Preserved local conflict as '" +
                                        backup.string() + "'."
                                );
                            },
                    }
                ));
                report_progress(index, expected_size, true);
            } catch (const detail::DownloadSpaceCancelledError&) {
                return;
            } catch (const graph::DownloadCancelledError&) {
                return;
            } catch (const detail::LocalModificationConflictError& error) {
                batch.conflicts[index] = error.what();
            } catch (...) {
                batch.errors[index] = std::current_exception();
                space.cancel();
                stop.request_stop();
            }
        }
    };

    std::vector<std::jthread> workers;
    const auto worker_count = std::min(concurrency, tasks.size());
    workers.reserve(worker_count);
    try {
        for (std::size_t index = 0; index < worker_count; ++index) {
            workers.emplace_back(worker);
        }
    } catch (...) {
        space.cancel();
        stop.request_stop();
        for (auto& thread : workers) {
            thread.request_stop();
        }
        for (auto& thread : workers) {
            thread.join();
        }
        throw;
    }
    for (auto& thread : workers) {
        thread.join();
    }
    return batch;
}

graph::RemoteItem remote_item(const storage::BlockedItem& item) {
    return {
        .id = item.remote_id,
        .name = item.name,
        .etag = item.etag,
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
                return item.remote_path.size() > directory.size() &&
                       item.remote_path.starts_with(directory) &&
                       item.remote_path[directory.size()] == '/';
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
                item.remote_path.size() <= previous->remote_path.size() ||
                !item.remote_path.starts_with(previous->remote_path) ||
                item.remote_path[previous->remote_path.size()] != '/') {
                continue;
            }
            auto remote_path =
                change.remote_path +
                item.remote_path.substr(previous->remote_path.size());
            descendants.push_back({
                .id = item.remote_id,
                .name = item.name,
                .etag = item.etag,
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

void report_blocked(
    const storage::BlockedItem& item,
    const cli::Console& console
) {
    spdlog::warn(
        "Blocked remote item '{}': {} ({})",
        item.remote_path,
        item.reason_message,
        item.reason_code
    );
    console.blocked_item(
        item.remote_path,
        item.reason_code,
        item.reason_message
    );
}

bool below_blocked_directory(
    std::string_view path,
    const std::vector<std::string>& blocked_directories
) {
    return std::ranges::any_of(
        blocked_directories,
        [path](const std::string& directory) {
            return path.size() > directory.size() &&
                   path.starts_with(directory) &&
                   path[directory.size()] == '/';
        }
    );
}

std::size_t execute_removals(
    detail::SyncPlan& plan,
    const detail::SafeSyncRoot& safe_root,
    const std::string& drive_id,
    storage::ItemStore& items,
    detail::ItemOperationCoordinator& operations,
    const cli::Console& console
) {
    struct Removal {
        graph::RemoteItem item;
        std::optional<storage::ItemState> previous;
    };
    std::vector<Removal> removals;
    removals.reserve(plan.removal_count());
    for (std::size_t index = 0; index < plan.removal_count(); ++index) {
        const auto& item = plan.removal(index);
        removals.push_back({
            .item = item,
            .previous = items.find(drive_id, item.id),
        });
    }
    std::ranges::sort(
        removals,
        [](const Removal& left, const Removal& right) {
            if (!left.previous || !right.previous) {
                return left.previous.has_value();
            }
            const auto left_depth = std::ranges::count(
                left.previous->remote_path,
                '/'
            );
            const auto right_depth = std::ranges::count(
                right.previous->remote_path,
                '/'
            );
            if (left_depth != right_depth) {
                return left_depth > right_depth;
            }
            return !left.previous->directory &&
                   right.previous->directory;
        }
    );

    std::size_t removed = 0;
    for (const auto& removal : removals) {
        if (!removal.previous) {
            plan.complete_removal(removal.item.id);
            continue;
        }
        const auto& previous = *removal.previous;
        auto block = [&](std::string code, std::string message) {
            plan.block_removal(
                previous,
                std::move(code),
                std::move(message)
            );
            report_blocked(
                plan.blocked(plan.blocked_count() - 1),
                console
            );
        };
        try {
            auto operation = operations.acquire(drive_id, previous.remote_id);
            auto destination_operation =
                operations.acquire_destination(previous.local_path);
            static_cast<void>(
                safe_root.relative_path(previous.local_path)
            );
            std::error_code error;
            const auto status =
                std::filesystem::symlink_status(previous.local_path, error);
            if (error == std::errc::no_such_file_or_directory ||
                !std::filesystem::exists(status)) {
                plan.complete_removal(previous.remote_id);
                continue;
            }
            if (error) {
                throw std::runtime_error(
                    "cannot inspect local deletion target '" +
                    previous.local_path.string() + "': " + error.message()
                );
            }
            if (std::filesystem::is_symlink(status) ||
                (previous.directory &&
                 !std::filesystem::is_directory(status)) ||
                (!previous.directory &&
                 !std::filesystem::is_regular_file(status))) {
                block(
                    "local_path_conflict",
                    "remote deletion target has an unexpected local type: " +
                        previous.local_path.string()
                );
                continue;
            }
            if (!previous.directory &&
                !detail::local_snapshot_matches(
                    previous,
                    previous.local_path
                )) {
                block(
                    "local_modification",
                    "local file changed after the last synchronized snapshot: " +
                        previous.local_path.string()
                );
                continue;
            }
            try {
                if (safe_root.remove(
                        previous.local_path,
                        previous.directory
                    )) {
                    ++removed;
                    console.message(
                        cli::MessageKind::information,
                        "local_item_removed",
                        "Removed remotely deleted local item '" +
                            previous.local_path.string() + "'."
                    );
                }
                plan.complete_removal(previous.remote_id);
            } catch (const detail::SafePathConflictError& exception) {
                block("local_path_conflict", exception.what());
            }
        } catch (const detail::SafePathConflictError& exception) {
            block("local_path_conflict", exception.what());
        }
    }
    return removed;
}

struct MoveSummary {
    std::size_t moved{0};
    std::unordered_set<std::string> reusable_files;
    std::unordered_set<std::string> blocked;
};

bool remote_file_content_unchanged(
    const graph::RemoteItem& item,
    const storage::ItemState& previous,
    const std::filesystem::path& path
) {
    if (!item.content_hash) {
        return item.size == previous.size &&
               item.last_modified == previous.last_modified;
    }
    detail::DownloadHashes hashes;
    if (item.content_hash->algorithm == FileHashAlgorithm::sha256) {
        hashes.sha256 = detail::content_fingerprint(path);
    } else {
        hashes.quick_xor = detail::quick_xor_hash(path);
    }
    try {
        detail::verify_download_integrity(item, hashes);
        return true;
    } catch (const detail::DownloadIntegrityError&) {
        return false;
    }
}

MoveSummary execute_moves(
    detail::SyncPlan& plan,
    const detail::SafeSyncRoot& safe_root,
    const std::string& drive_id,
    storage::ItemStore& items,
    detail::ItemOperationCoordinator& operations,
    const cli::Console& console,
    bool private_permissions
) {
    struct Move {
        graph::RemoteItem item;
        storage::ItemState previous;
    };
    std::vector<Move> moves;
    for (std::size_t index = 0; index < plan.move_count(); ++index) {
        const auto& item = plan.move(index);
        if (auto previous = items.find(drive_id, item.id);
            previous.has_value()) {
            moves.push_back({
                .item = item,
                .previous = std::move(previous).value(),
            });
        }
    }
    std::ranges::sort(
        moves,
        {},
        [](const Move& move) {
            return std::ranges::count(move.previous.remote_path, '/');
        }
    );

    MoveSummary summary;
    for (const auto& move : moves) {
        if (summary.blocked.contains(move.item.id)) {
            continue;
        }
        auto& state = plan.state_for(move.item.id);
        const auto& source = move.previous.local_path;
        const auto& destination = state.local_path;
        const auto block = [&](std::string code, std::string message) {
            plan.block(move.item, std::move(code), std::move(message));
            summary.blocked.insert(move.item.id);
            report_blocked(
                plan.blocked(plan.blocked_count() - 1),
                console
            );
        };
        try {
            auto operation =
                operations.acquire(drive_id, move.previous.remote_id);
            auto source_operation =
                operations.acquire_destination(source);
            auto destination_operation =
                operations.acquire_destination(destination);
            static_cast<void>(safe_root.relative_path(source));
            static_cast<void>(safe_root.relative_path(destination));

            std::error_code source_error;
            const auto source_status =
                std::filesystem::symlink_status(source, source_error);
            const bool source_exists =
                !source_error && std::filesystem::exists(source_status);
            if (source_error &&
                source_error != std::errc::no_such_file_or_directory) {
                throw std::runtime_error(
                    "cannot inspect local move source '" + source.string() +
                    "': " + source_error.message()
                );
            }

            std::error_code destination_error;
            const auto destination_status =
                std::filesystem::symlink_status(
                    destination,
                    destination_error
                );
            const bool destination_exists =
                !destination_error &&
                std::filesystem::exists(destination_status);
            if (destination_error &&
                destination_error !=
                    std::errc::no_such_file_or_directory) {
                throw std::runtime_error(
                    "cannot inspect local move destination '" +
                    destination.string() + "': " +
                    destination_error.message()
                );
            }

            const auto expected_type = [&](auto status) {
                return !std::filesystem::is_symlink(status) &&
                       (move.previous.directory ?
                            std::filesystem::is_directory(status) :
                            std::filesystem::is_regular_file(status));
            };
            if (!source_exists) {
                if (!destination_exists ||
                    !expected_type(destination_status) ||
                    (!move.previous.directory &&
                     !detail::local_snapshot_matches(
                         move.previous,
                         destination
                     ))) {
                    block(
                        "local_path_conflict",
                        "remote move source is missing and its destination "
                        "cannot be safely adopted: " + source.string()
                    );
                    continue;
                }
            } else {
                if (!expected_type(source_status)) {
                    block(
                        "local_path_conflict",
                        "remote move source has an unexpected local type: " +
                            source.string()
                    );
                    continue;
                }
                if (!move.previous.directory &&
                    !detail::local_snapshot_matches(
                        move.previous,
                        source
                    )) {
                    block(
                        "local_modification",
                        "local file changed before applying remote move: " +
                            source.string()
                    );
                    continue;
                }
                if (destination_exists) {
                    block(
                        "local_path_conflict",
                        "remote move destination already exists: " +
                            destination.string()
                    );
                    continue;
                }
                safe_root.ensure_directory_tree(
                    destination.parent_path(),
                    private_permissions
                );
                if (!safe_root.rename_no_replace(source, destination)) {
                    block(
                        "local_path_conflict",
                        "remote move destination already exists: " +
                            destination.string()
                    );
                    continue;
                }
                safe_root.fsync_directory(source.parent_path());
                if (source.parent_path() != destination.parent_path()) {
                    safe_root.fsync_directory(destination.parent_path());
                }
                ++summary.moved;
                console.message(
                    cli::MessageKind::information,
                    "local_item_moved",
                    "Moved remotely renamed item from '" + source.string() +
                        "' to '" + destination.string() + "'."
                );
            }

            if (!move.previous.directory) {
                state.local_size = static_cast<std::int64_t>(
                    std::filesystem::file_size(destination)
                );
                state.local_modified_ticks =
                    detail::modified_ticks(destination);
                if (remote_file_content_unchanged(
                        move.item,
                        move.previous,
                        destination
                    )) {
                    summary.reusable_files.insert(move.item.id);
                }
            }
        } catch (const detail::CrossDeviceMoveError& error) {
            block("cross_device_move", error.what());
        } catch (const detail::SafePathConflictError& error) {
            block("local_path_conflict", error.what());
        } catch (const detail::LocalPathConflictError& error) {
            block("local_path_conflict", error.what());
        }
    }
    return summary;
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
        report_blocked(plan.blocked(index), console);
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
    bool private_permissions
) {
    const auto& sync_root = safe_root.path();
    detail::ItemOperationCoordinator operations;
    const auto removed_count = execute_removals(
        plan,
        safe_root,
        drive_id,
        items,
        operations,
        console
    );
    auto move_summary = execute_moves(
        plan,
        safe_root,
        drive_id,
        items,
        operations,
        console,
        private_permissions
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
            report_blocked(
                plan.blocked(plan.blocked_count() - 1),
                console
            );
            continue;
        }
        try {
            safe_root.ensure_directory_tree(
                plan.state_for(item.id).local_path,
                private_permissions
            );
            ++prepared_directory_count;
        } catch (const detail::LocalPathConflictError& error) {
            plan.block(item, "local_path_conflict", error.what());
            blocked_directories.push_back(item.remote_path);
            report_blocked(
                plan.blocked(plan.blocked_count() - 1),
                console
            );
        } catch (const detail::SafePathConflictError& error) {
            plan.block(item, "local_path_conflict", error.what());
            blocked_directories.push_back(item.remote_path);
            report_blocked(
                plan.blocked(plan.blocked_count() - 1),
                console
            );
        }
    }

    std::size_t downloaded_count = 0;
    std::size_t reused_count = 0;
    std::vector<DownloadTask> download_tasks;
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
            report_blocked(
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
                private_permissions
            );
        } catch (const detail::LocalPathConflictError& error) {
            plan.block(item, "local_path_conflict", error.what());
            report_blocked(
                plan.blocked(plan.blocked_count() - 1),
                console
            );
            continue;
        } catch (const detail::SafePathConflictError& error) {
            plan.block(item, "local_path_conflict", error.what());
            report_blocked(
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
            report_blocked(
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
        const bool exists = std::filesystem::exists(destination);
        const bool snapshot_matches =
            exists && previous.has_value() &&
            detail::local_snapshot_matches(*previous, destination);
        const bool current_remote_file =
            snapshot_matches && previous->etag == item.etag;
        bool preserve_local =
            exists && !current_remote_file && !snapshot_matches;
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
            report_blocked(
                plan.blocked(plan.blocked_count() - 1),
                console
            );
            continue;
        }

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
            detail::LocalFileBaseline baseline;
            try {
                baseline =
                    detail::capture_local_file_baseline(destination);
            } catch (const detail::LocalModificationConflictError& error) {
                plan.block(item, "local_modification", error.what());
                report_blocked(
                    plan.blocked(plan.blocked_count() - 1),
                    console
                );
                continue;
            } catch (const detail::LocalPathConflictError& error) {
                plan.block(item, "local_path_conflict", error.what());
                report_blocked(
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
                    report_blocked(
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
        [](const DownloadTask& task) {
            return task.item.size;
        },
        [](const DownloadTask& task) -> const std::string& {
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
    auto downloads = download_files(
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
            report_blocked(
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
        std::filesystem::path sync_root =
            onedrive::detail::normalized_absolute(config_->sync_directory);
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
            const bool private_permissions =
                config_->sync_permissions ==
                config::SyncPermissionsMode::private_access;
            sync_root = detail::prepare_sync_root(
                sync_root,
                private_permissions
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
                private_permissions
            );
            if (config_->upload) {
                detail::recover_pending_uploads(
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
        bool replace_drive_items = !query_delta_link.has_value();
        graph::DeltaResult delta;
        const auto delta_progress =
            [&console](std::size_t pages, std::size_t items, bool completed) {
                console.delta_progress(pages, items, completed);
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
            replace_drive_items = true;
        }
        const auto tracked_items =
            items_.drive_items(config_->drive_id);
        if (replace_drive_items) {
            add_full_refresh_deletions(
                delta,
                tracked_items
            );
        }
        add_deleted_descendants(delta, tracked_items);
        add_moved_descendants(delta, tracked_items);
        const auto previously_blocked =
            items_.blocked_items(config_->drive_id);
        if (!replace_drive_items && !previously_blocked.empty()) {
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
                replace_drive_items
            );
            spdlog::info(
                "Selective synchronization excluded {} remote changes",
                filtered.excluded
            );
            snapshot_removals = std::move(filtered.snapshot_removals);
            delta = std::move(filtered.delta);
        }
        auto plan = detail::SyncPlan::build(
            std::move(delta),
            config_->drive_id,
            sync_root,
            replace_drive_items,
            sync_filter_fingerprint,
            std::move(snapshot_removals),
            tracked_items
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
                    true
                );
                blocked_count += upload_summary.blocked;
                console.section(
                    "upload_plan",
                    "Local upload plan:",
                    {
                        {
                            .label = "upload files:",
                            .key = "upload_files",
                            .value =
                                std::to_string(upload_summary.planned),
                        },
                        {
                            .label = "blocked:",
                            .key = "blocked",
                            .value =
                                std::to_string(upload_summary.blocked),
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
                config_->sync_permissions ==
                    config::SyncPermissionsMode::private_access
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
                    false
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

        record_result(true);
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
