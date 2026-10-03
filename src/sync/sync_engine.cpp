#include "onedrive/sync/sync_engine.hpp"

#include "onedrive/cli/console.hpp"
#include "download_recovery.hpp"
#include "download_space_coordinator.hpp"
#include "download_transaction.hpp"
#include "filesystem_metadata.hpp"
#include "item_operation_coordinator.hpp"
#include "local_filesystem.hpp"
#include "sync_plan.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <atomic>
#include <chrono>
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

namespace onedrive::sync {
namespace {

struct ExecutionSummary {
    std::size_t downloaded{0};
    std::size_t reused{0};
    std::size_t directories{0};
};

struct DownloadTask {
    graph::RemoteItem item;
    storage::ItemState state;
    std::filesystem::path destination;
    detail::LocalFileBaseline destination_baseline;
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
    const cli::Console& console
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
            console.download_progress(
                completed_files,
                tasks.size(),
                downloaded_bytes,
                total_bytes,
                all_completed
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
            const auto expected_size =
                static_cast<std::uint64_t>(task.item.size);
            try {
                auto operation = operations.acquire(
                    task.state.drive_id,
                    task.item.id
                );
                batch.states[index].emplace(detail::commit_download(
                    items,
                    detail::prepare_download(
                        graph,
                        items,
                        task.item,
                        task.state,
                        task.destination,
                        task.destination_baseline,
                        metadata,
                        space,
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
                    )
                ));
                report_progress(index, expected_size, true);
            } catch (const detail::DownloadSpaceCancelledError&) {
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
        .deleted = false,
        .root = false,
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
            "Remote delta contains {} changes ({} upserts, {} removals, {} "
            "blocked).",
            plan.change_count(),
            upsert_count,
            plan.removal_count(),
            plan.blocked_count()
        )
    );
    spdlog::info(
        "Remote delta prepared for drive '{}': {} upserts, {} removals, {} "
        "blocked",
        drive_id,
        upsert_count,
        plan.removal_count(),
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
                .label = "blocked items:",
                .key = "blocked_items",
                .value = std::to_string(plan.blocked_count()),
            },
            {
                .label = "local removals:",
                .key = "local_removals",
                .value = "0",
            },
        }
    );
    for (std::size_t index = 0; index < plan.blocked_count(); ++index) {
        report_blocked(plan.blocked(index), console);
    }
}

std::uintmax_t download_safety_reserve(std::uintmax_t transfer_bytes) {
    constexpr std::uintmax_t minimum_reserve =
        std::uintmax_t{256} * 1024U * 1024U;
    return std::max(minimum_reserve, transfer_bytes / 20U);
}

ExecutionSummary execute_plan(
    detail::SyncPlan& plan,
    const std::filesystem::path& sync_root,
    const std::string& drive_id,
    graph::GraphClient& graph,
    storage::ItemStore& items,
    const detail::FilesystemMetadata& metadata,
    const cli::Console& console,
    std::size_t download_concurrency
) {
    std::vector<std::string> blocked_directories;
    for (std::size_t index = 0; index < plan.blocked_count(); ++index) {
        const auto& blocked = plan.blocked(index);
        if (blocked.directory) {
            blocked_directories.push_back(blocked.remote_path);
        }
    }

    std::size_t prepared_directory_count = 0;
    for (std::size_t index = 0; index < plan.directory_count(); ++index) {
        const auto& item = plan.directory(index);
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
            detail::ensure_directory_tree(
                sync_root,
                plan.state_for(item.id).local_path
            );
            ++prepared_directory_count;
        } catch (const detail::LocalPathConflictError& error) {
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
            detail::ensure_directory_tree(sync_root, destination.parent_path());
        } catch (const detail::LocalPathConflictError& error) {
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
        const bool exists = std::filesystem::exists(destination);
        const bool snapshot_matches =
            exists && previous.has_value() &&
            detail::local_snapshot_matches(*previous, destination);
        const bool current_remote_file =
            snapshot_matches && previous->etag == item.etag;
        if (exists && !current_remote_file && !snapshot_matches) {
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
            });
        }
    }

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
        download_safety_reserve(transfer_bytes)
    };
    detail::ItemOperationCoordinator operations;
    auto downloads = download_files(
        download_tasks,
        download_concurrency,
        graph,
        items,
        operations,
        space,
        metadata,
        console
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
        std::filesystem::path sync_root = config_->sync_directory;
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
        } else {
            sync_root = prepare_sync_root(config_->sync_directory);
            metadata.emplace(detail::FilesystemMetadata::detect(
                config_->filesystem_metadata,
                sync_root
            ));
            detail::recover_pending_downloads(
                items_,
                sync_root,
                config_->drive_id,
                *metadata
            );
        }

        const auto previous_delta_link = items_.delta_link(config_->drive_id);
        spdlog::debug(
            "Preparing Microsoft Graph delta query for drive '{}': {} tracked "
            "items, saved cursor {}",
            config_->drive_id,
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
        const auto delta_progress =
            [&console](std::size_t pages, std::size_t items, bool completed) {
                console.delta_progress(pages, items, completed);
            };
        try {
            delta = graph_.list_delta(previous_delta_link, delta_progress);
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
        const auto previously_blocked =
            items_.blocked_items(config_->drive_id);
        if (!replace_drive_items && !previously_blocked.empty()) {
            add_blocked_retries(delta, previously_blocked);
            spdlog::debug(
                "Added {} blocked items to the synchronization retry plan",
                previously_blocked.size()
            );
        }
        auto plan = detail::SyncPlan::build(
            std::move(delta),
            config_->drive_id,
            config_->sync_directory,
            replace_drive_items
        );
        report_plan(plan, config_->drive_id, console);

        std::size_t blocked_count = plan.blocked_count();
        if (config_->dry_run) {
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
                sync_root,
                config_->drive_id,
                graph_,
                items_,
                *metadata,
                console,
                config_->download_concurrency
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
                    {
                        .label = "blocked:",
                        .key = "blocked",
                        .value = std::to_string(plan.blocked_count()),
                    },
                }
            );
            spdlog::debug(
                "Persisting remote delta for drive '{}'",
                config_->drive_id
            );
            blocked_count = plan.blocked_count();
            items_.apply_delta(plan.release_state_delta());
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
