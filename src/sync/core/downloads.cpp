#include "sync/core/downloads.hpp"

#include "onedrive/cli/console.hpp"
#include "sync/core/item_ops.hpp"
#include "sync/download/progress.hpp"
#include "sync/download/space.hpp"
#include "sync/download/transaction.hpp"
#include "sync/filesystem/metadata.hpp"
#include "sync/filesystem/safe_sync_root.hpp"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <stop_token>
#include <thread>

namespace onedrive::sync::engine_detail {

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
    enum class DownloadTaskState {
        active,
        completed,
    };
    std::vector<DownloadTaskState> task_states(
        tasks.size(), DownloadTaskState::active
    );
    std::uint64_t downloaded_bytes = 0;
    std::uint64_t total_bytes = 0;
    for (const auto& task : tasks) {
        total_bytes += static_cast<std::uint64_t>(task.item.size);
    }
    std::size_t completed_files = 0;
    unsigned last_reported_percentage = 0;
    detail::DownloadProgressEstimator progress_estimator;
    const auto report_progress =
        [&](std::size_t index,
            std::uint64_t downloaded,
            util::ProgressState state) {
            const std::scoped_lock lock{console_mutex};
            const bool completed = state == util::ProgressState::completed;
            const auto expected_size =
                static_cast<std::uint64_t>(tasks[index].item.size);
            const auto current = std::min(downloaded, expected_size);
            if (current > task_downloaded[index]) {
                downloaded_bytes += current - task_downloaded[index];
                task_downloaded[index] = current;
            }
            if (completed &&
                task_states[index] == DownloadTaskState::active) {
                task_states[index] = DownloadTaskState::completed;
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
                all_completed ?
                    util::ProgressState::completed :
                    util::ProgressState::ongoing,
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
                    task.item.id,
                    stop.get_token()
                );
                auto destination_operation =
                    operations.acquire_destination(
                        task.destination,
                        stop.get_token()
                    );
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
                            report_progress(
                                index,
                                downloaded,
                                util::ProgressState::ongoing
                            );
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
                report_progress(
                    index,
                    expected_size,
                    util::ProgressState::completed
                );
            } catch (const detail::ItemOperationCancelledError&) {
                return;
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

}  // namespace onedrive::sync::engine_detail
