#include "sync/core/reporting.hpp"
#include "sync/core/plan_execute.hpp"
#include "sync/upload/orchestration.hpp"
#include <format>

#include "onedrive/ui/common/observer.hpp"
#include "onedrive/storage/item_store.hpp"

namespace onedrive::sync::engine_detail {

void report_blocked(
    const storage::BlockedItem& item, const events::Observer& observer
) {
    observer.report_blocked_item(
        item.remote_path, item.reason_code, item.reason_message
    );
}

void report_dry_run_configuration(
    const config::Config& config,
    storage::ItemStore& items,
    const events::Observer& observer
) {
    observer.section(
        "dry_run_configuration",
        "Dry run configuration:",
        {
            {
                .label = "sync directory:",
                .key = "sync_data_directory",
                .value = config.sync_data_directory.string(),
            },
            {
                .label = "state directory:",
                .key = "state_directory",
                .value = config.state_directory.string(),
            },
            {
                .label = "drive id:",
                .key = "drive_id",
                .value = config.drive_id,
            },
            {
                .label = "throttle retries:",
                .key = "throttle_retries",
                .value = std::to_string(config.graph_maximum_throttle_retries),
            },
            {
                .label = "throttle delay:",
                .key = "throttle_delay",
                .value = std::format(
                    "{}-{} seconds",
                    config.graph_initial_throttle_delay.count(),
                    config.graph_maximum_throttle_delay.count()
                ),
            },
            {
                .label = "download concurrency:",
                .key = "download_concurrency",
                .value = std::to_string(config.download_concurrency),
            },
            {
                .label = "per-download rate:",
                .key = "download_rate_limit",
                .value = std::to_string(
                    config.download_maximum_rate_bytes_per_second
                ),
            },
            {
                .label = "total download rate:",
                .key = "download_total_rate_limit",
                .value = std::to_string(
                    config.download_maximum_total_rate_bytes_per_second
                ),
            },
            {
                .label = "upload concurrency:",
                .key = "upload_concurrency",
                .value = std::to_string(config.upload_concurrency),
            },
            {
                .label = "upload chunk size:",
                .key = "upload_chunk_size",
                .value = std::to_string(config.upload_chunk_size_bytes),
            },
            {
                .label = "per-upload rate:",
                .key = "upload_rate_limit",
                .value =
                    std::to_string(config.upload_maximum_rate_bytes_per_second),
            },
            {
                .label = "total upload rate:",
                .key = "upload_total_rate_limit",
                .value = std::to_string(
                    config.upload_maximum_total_rate_bytes_per_second
                ),
            },
            {
                .label = "tracked items:",
                .key = "tracked_items",
                .value = std::to_string(items.size()),
            },
        }
    );
}

void report_upload_plan(
    const config::Config& config,
    const detail::UploadSummary& summary,
    const events::Observer& observer
) {
    observer.section(
        "upload_plan",
        "Local upload plan:",
        {
            {
                .label = "move remote items:",
                .key = "move_remote_items",
                .value = std::to_string(summary.planned_moves),
            },
            {
                .label = "delete remote items:",
                .key = "delete_remote_items",
                .value = std::to_string(summary.planned_deletions),
            },
            {
                .label = "affected tracked items:",
                .key = "affected_remote_deletions",
                .value = std::to_string(summary.affected_deletions),
            },
            {
                .label = "large-delete limit:",
                .key = "maximum_remote_deletions",
                .value = std::to_string(config.maximum_remote_deletions),
            },
            {
                .label = "large-delete blocked:",
                .key = "large_delete_blocked",
                .value = summary.large_delete_blocked ? "true" : "false",
            },
            {
                .label = "create directories:",
                .key = "create_directories",
                .value = std::to_string(summary.planned_directories),
            },
            {
                .label = "upload files:",
                .key = "upload_files",
                .value = std::to_string(summary.planned),
            },
            {
                .label = "blocked:",
                .key = "blocked",
                .value = std::to_string(summary.blocked),
            },
        }
    );
}

void report_execution_summary(
    const engine_detail::ExecutionSummary& execution,
    const detail::UploadSummary& upload,
    std::size_t blocked,
    std::size_t unsupported,
    const events::Observer& observer
) {
    observer.sync_summary({
        .downloaded = execution.downloaded,
        .uploaded = upload.uploaded,
        .moved = execution.moved + upload.moved,
        .deleted = execution.removed + upload.deleted,
        .blocked = blocked,
    });
    observer.section(
        "execution_summary",
        "Synchronization summary:",
        {
            {
                .label = "downloaded:",
                .key = "downloaded",
                .value = std::to_string(execution.downloaded),
            },
            {
                .label = "reused:",
                .key = "reused",
                .value = std::to_string(execution.reused),
            },
            {
                .label = "directories prepared:",
                .key = "directories_prepared",
                .value = std::to_string(execution.directories),
            },
            {
                .label = "local moves:",
                .key = "local_moves",
                .value = std::to_string(execution.moved),
            },
            {
                .label = "local removals:",
                .key = "local_removals",
                .value = std::to_string(execution.removed),
            },
            {
                .label = "remote moves:",
                .key = "remote_moves",
                .value = std::to_string(upload.moved),
            },
            {
                .label = "remote removals:",
                .key = "remote_removals",
                .value = std::to_string(upload.deleted),
            },
            {
                .label = "remote items affected:",
                .key = "remote_deletion_affected_items",
                .value = std::to_string(upload.affected_deletions),
            },
            {
                .label = "remote directories created:",
                .key = "remote_directories_created",
                .value = std::to_string(upload.created_directories),
            },
            {
                .label = "uploaded:",
                .key = "uploaded",
                .value = std::to_string(upload.uploaded),
            },
            {
                .label = "blocked:",
                .key = "blocked",
                .value = std::to_string(blocked),
            },
            {
                .label = "unsupported format items:",
                .key = "unsupported_items",
                .value = std::to_string(unsupported),
            },
        }
    );
}

void report_completion(
    std::size_t blocked,
    bool preview,
    std::chrono::steady_clock::time_point started_at,
    const events::Observer& observer
) {
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started_at
    );
    if (blocked != 0) {
        observer.report_message(
            events::MessageKind::warning,
            "sync_completed_with_issues",
            std::format(
                "Synchronization completed with {} blocked items in {} "
                "milliseconds.",
                blocked,
                elapsed.count()
            )
        );
        return;
    }
    const std::string_view message =
        preview ? "Synchronization dry run completed"
                : "Synchronization state update completed";
    observer.report_message(
        events::MessageKind::information,
        "sync_completed",
        std::format("{} in {} milliseconds", message, elapsed.count())
    );
}

}  // namespace onedrive::sync::engine_detail
