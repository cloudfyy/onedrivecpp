#include "onedrive/sync/core/engine.hpp"

#include "onedrive/cli/console.hpp"
#include "onedrive/util/path_security.hpp"
#include "sync/core/delta_plan.hpp"
#include "sync/core/downloads.hpp"
#include "sync/core/plan_execute.hpp"
#include "sync/core/plan_report.hpp"
#include "sync/core/item_ops.hpp"
#include "sync/core/local_move.hpp"
#include "sync/core/reporting.hpp"
#include "sync/core/remote_delete.hpp"
#include "sync/core/plan.hpp"
#include "sync/core/transfer_order.hpp"
#include "sync/download/recovery.hpp"
#include "sync/download/space.hpp"
#include "sync/download/target.hpp"
#include "sync/filesystem/operations.hpp"
#include "sync/filesystem/metadata.hpp"
#include "sync/filesystem/safe_sync_root.hpp"
#include "sync/filter/remote_path.hpp"
#include "sync/filter/selective.hpp"
#include "sync/upload/orchestration.hpp"

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
            engine_detail::add_full_refresh_deletions(
                delta,
                tracked_items
            );
        }
        engine_detail::add_deleted_descendants(delta, tracked_items);
        engine_detail::add_moved_descendants(delta, tracked_items);
        const auto previously_blocked =
            items_.blocked_items(config_->drive_id);
        if (apply_mode == storage::DeltaApplyMode::merge &&
            !previously_blocked.empty()) {
            engine_detail::add_blocked_retries(delta, previously_blocked);
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
        engine_detail::report_plan(plan, config_->drive_id, console);

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
            const auto summary = engine_detail::execute_plan(
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
