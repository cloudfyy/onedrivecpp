#include "onedrive/sync/core/engine.hpp"
#include "onedrive/exit_code.hpp"
#include "graph/delta_changes.hpp"

#include "onedrive/sync/cancellation.hpp"
#include "onedrive/ui/common/observer.hpp"
#include "onedrive/util/mount.hpp"
#include "onedrive/util/path_security.hpp"
#include "sync/core/delta_plan.hpp"
#include "onedrive/sync/blocked_policy.hpp"
#include "onedrive/sync/capabilities.hpp"
#include "sync/core/plan_execute.hpp"
#include "sync/core/plan_report.hpp"
#include "sync/core/reporting.hpp"
#include "sync/core/preparation.hpp"
#include "sync/core/plan.hpp"
#include "sync/filesystem/operations.hpp"
#include "sync/filesystem/safe_sync_root.hpp"
#include "sync/filter/remote_path.hpp"
#include "sync/filter/selective.hpp"
#include "sync/upload/orchestration.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <exception>
#include <filesystem>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace onedrive::sync {
namespace {

struct TransparentStringHash {
    using is_transparent = void;

    [[nodiscard]] std::size_t
    operator()(std::string_view value) const noexcept {
        return std::hash<std::string_view>{}(value);
    }
};

std::vector<storage::PartialDownload> orphaned_partial_downloads(
    const graph::DeltaResult& delta,
    storage::DeltaApplyMode apply_mode,
    std::vector<storage::PartialDownload> partials
) {
    std::unordered_set<std::string, TransparentStringHash, std::equal_to<>>
        item_ids;
    item_ids.reserve(delta.changes.size() + partials.size());
    if (apply_mode == storage::DeltaApplyMode::replace) {
        for (const auto& item : delta.changes) {
            if (!item.deleted) {
                item_ids.insert(item.id);
            }
        }
        std::erase_if(partials, [&item_ids](const auto& partial) {
            return item_ids.contains(partial.item.remote_id);
        });
        return partials;
    }

    for (const auto& item : delta.changes) {
        if (item.deleted) {
            item_ids.insert(item.id);
        }
    }
    bool expanded = true;
    while (expanded) {
        expanded = false;
        for (const auto& partial : partials) {
            if (item_ids.contains(partial.item.parent_id)) {
                if (item_ids.insert(partial.item.remote_id).second) {
                    expanded = true;
                }
            }
        }
    }
    std::erase_if(partials, [&item_ids](const auto& partial) {
        return !item_ids.contains(partial.item.remote_id);
    });
    return partials;
}

void discard_orphaned_partial_files(
    const std::vector<storage::PartialDownload>& partials
) noexcept {
    for (const auto& partial : partials) {
        try {
            detail::remove_no_symlinks(partial.temporary_path);
            spdlog::info(
                "Removed partial download for deleted remote item '{}'",
                partial.item.remote_path
            );
        } catch (const std::exception& error) {
            spdlog::warn(
                "Could not remove orphaned partial download '{}': {}",
                partial.temporary_path.string(),
                error.what()
            );
        }
    }
}

} // namespace

int SyncEngine::synchronize(const std::stop_token& stop_token) {
    std::optional<PreviewCache::Snapshot> previous_preview;
    if (preview_cache_) {
        previous_preview = std::move(preview_cache_->snapshot_);
        preview_cache_->clear();
    }
    const auto started_at = std::chrono::steady_clock::now();
    const auto record_result = [this,
                                started_at](metrics::SyncRunOutcome outcome) {
        metrics_.record_sync_run(
            outcome, std::chrono::steady_clock::now() - started_at
        );
    };
    const auto& observer = *observer_;
    const auto capabilities = capabilities_for(
        config_.sync_mode,
        config_.delete_policy,
        config_.dry_run ? ExecutionMode::preview : ExecutionMode::apply
    );

    try {
        throw_if_cancelled(stop_token);
        onedrive::util::require_sync_mount(
            config_.sync_data_directory, config_.sync_data_mount_point
        );
        observer.sync_context(config_.sync_mode, capabilities.previews());
        observer.sync_phase(events::SyncPhase::preparing);
        std::filesystem::path sync_root =
            onedrive::util::normalized_absolute(config_.sync_data_directory);
        auto cache_state =
            preview_cache_ && (capabilities.previews() || previous_preview)
                ? PreviewCache::capture_state(items_, config_.drive_id)
                : std::nullopt;
        auto prepared = engine_detail::prepare_sync_filesystem(
            config_,
            sync_root,
            capabilities,
            graph_,
            items_,
            observer,
            stop_token
        );
        auto& safe_root = prepared.safe_root;
        auto& metadata = prepared.metadata;
        const auto recovered_uploads = prepared.recovered_uploads;

        const auto sync_filter = detail::SyncList::configured({
            .rules_path = config_.sync_list,
            .sync_root = sync_root,
            .include_root_files = config_.sync_root_files,
            .nosync_enabled = config_.nosync_enabled,
            .dotfiles = config_.dotfiles,
            .maximum_file_size_bytes = config_.maximum_file_size_bytes,
        });
        const std::string sync_filter_fingerprint =
            engine_detail::sync_scope_fingerprint(sync_filter.fingerprint());
        const auto previous_delta_link = items_.delta_link(config_.drive_id);
        const auto previous_sync_filter_fingerprint =
            items_.sync_filter_fingerprint(config_.drive_id);
        const bool sync_filter_changed =
            previous_delta_link.has_value() &&
            previous_sync_filter_fingerprint.value_or("") !=
                sync_filter_fingerprint;
        auto query_delta_link = sync_filter_changed
                                    ? std::optional<std::string>{}
                                    : previous_delta_link;
        const auto scope_fingerprint = PreviewCache::scope_fingerprint(config_);
        auto pending_delta = items_.pending_delta(config_.drive_id);
        if (pending_delta &&
            (pending_delta->scope_fingerprint != scope_fingerprint ||
             pending_delta->sync_filter_fingerprint !=
                 sync_filter_fingerprint ||
             pending_delta->applied_delta_link != previous_delta_link ||
             pending_delta->applied_filter_fingerprint !=
                 previous_sync_filter_fingerprint)) {
            observer.message(
                events::MessageKind::information,
                "pending_delta_invalidated",
                "Saved remote changes no longer match the account, filters, "
                "hash policy, unsupported formats or applied cursor; fetching "
                "remote changes "
                "again..."
            );
            pending_delta.reset();
            if (!capabilities.previews()) {
                items_.remove_pending_delta(config_.drive_id);
            }
        }
        std::optional<graph::DeltaResult> retained_delta;
        std::optional<storage::DeltaApplyMode> retained_mode;
        if (previous_preview &&
            (!cache_state ||
             previous_preview->scope != PreviewCache::scope(config_) ||
             previous_preview->filter != sync_filter_fingerprint ||
             previous_preview->state != *cache_state)) {
            previous_preview.reset();
            observer.message(
                events::MessageKind::information,
                "preview_cache_invalidated",
                "Preview metadata no longer matches the account, filters or "
                "synchronization state; fetching remote changes again..."
            );
        }
        if (previous_preview) {
            query_delta_link = previous_preview->delta.delta_link;
            retained_delta = std::move(previous_preview->delta);
            retained_mode = previous_preview->apply_mode;
            observer.message(
                events::MessageKind::information,
                "preview_cache_reused",
                "Reusing preview metadata; checking only subsequent remote "
                "changes and rebuilding the local plan..."
            );
        } else if (pending_delta) {
            query_delta_link = pending_delta->delta.delta_link;
            retained_delta = std::move(pending_delta->delta);
            retained_mode = pending_delta->apply_mode;
            observer.message(
                events::MessageKind::information,
                "pending_delta_resumed",
                "Restoring saved remote changes; checking only subsequent "
                "changes and rebuilding the local plan..."
            );
        }
        if (sync_filter_changed && !retained_delta) {
            observer.report_message(
                events::MessageKind::information,
                "sync_filter_changed",
                "Synchronization filters or unsupported-format rules changed; "
                "fetching the full "
                "remote state..."
            );
        }
        std::vector<std::string> snapshot_removals;
        if (config_.sync_list) {
            spdlog::info(
                "Loaded {} selective synchronization rules from '{}' "
                "(root files: {})",
                sync_filter.rule_count(),
                config_.sync_list->string(),
                config_.sync_root_files ? "included" : "rule-selected"
            );
        }
        spdlog::info(
            "Synchronization filters: .nosync {}, dotfiles {}, maximum file "
            "size {}",
            config_.nosync_enabled ? "enabled" : "disabled",
            config_.dotfiles == config::DotfilePolicy::exclude ? "excluded"
                                                               : "included",
            config_.maximum_file_size_bytes == 0
                ? std::string{"unlimited"}
                : std::to_string(config_.maximum_file_size_bytes) + " bytes"
        );
        spdlog::debug(
            "Preparing Microsoft Graph delta query for drive '{}': {} tracked "
            "items, saved cursor {}",
            config_.drive_id,
            items_.size(),
            query_delta_link ? "present" : "absent"
        );
        observer.message(
            events::MessageKind::information,
            "delta_query_started",
            "Fetching Microsoft Graph changes..."
        );
        throw_if_cancelled(stop_token);
        auto apply_mode = query_delta_link.has_value()
                              ? storage::DeltaApplyMode::merge
                              : storage::DeltaApplyMode::replace;
        if (retained_mode) {
            apply_mode = *retained_mode;
        }
        graph::DeltaResult delta;
        std::size_t delta_pages = 0;
        std::size_t delta_scanned_items = 0;
        observer.sync_phase(events::SyncPhase::scanning);
        const auto delta_progress =
            [&observer, &delta_pages, &delta_scanned_items](
                std::size_t pages, std::size_t items, util::ProgressState state
            ) {
                delta_pages = pages;
                delta_scanned_items = items;
                observer.delta_progress(pages, items, state);
            };
        const auto known_directories =
            blocked_ancestor_context(config_.drive_id, items_);
        try {
            delta = graph_.list_delta(
                query_delta_link,
                delta_progress,
                stop_token,
                query_delta_link
                    ? std::span<const graph::RemoteItem>{known_directories}
                    : std::span<const graph::RemoteItem>{}
            );
        } catch (const graph::DeltaCursorInvalidError& error) {
            previous_preview.reset();
            retained_delta.reset();
            spdlog::warn(
                "{}; retrying with a full Microsoft Graph delta query",
                error.what()
            );
            observer.message(
                events::MessageKind::warning,
                "delta_cursor_invalid",
                "The saved Microsoft Graph cursor is no longer valid; "
                "fetching the full remote state..."
            );
            delta = graph_.list_delta(std::nullopt, delta_progress, stop_token);
            apply_mode = storage::DeltaApplyMode::replace;
        }
        throw_if_cancelled(stop_token);
        auto tracked_items = items_.drive_items(config_.drive_id);
        std::ranges::sort(tracked_items, {}, &storage::ItemState::remote_id);
        if (retained_delta) {
            graph::client_detail::DeltaChanges changes{
                std::move(retained_delta->changes)
            };
            const bool hierarchy_changed = std::ranges::any_of(
                delta.changes, [&](const graph::RemoteItem& change) {
                    const auto* cached = changes.find(change.id);
                    const auto tracked_position = std::ranges::lower_bound(
                        tracked_items,
                        change.id,
                        {},
                        &storage::ItemState::remote_id
                    );
                    const auto* tracked =
                        tracked_position != tracked_items.end() &&
                                tracked_position->remote_id == change.id
                            ? &*tracked_position
                            : nullptr;
                    if (change.deleted && !cached && !tracked) {
                        // A tombstone does not identify whether an unknown
                        // item was a directory containing cached children.
                        return true;
                    }
                    if (cached && cached->directory &&
                        (change.deleted ||
                         cached->unsupported_format !=
                             change.unsupported_format ||
                         cached->remote_path != change.remote_path)) {
                        return true;
                    }
                    return tracked && tracked->directory &&
                           (change.deleted ||
                            tracked->remote_path != change.remote_path);
                }
            );
            if (hierarchy_changed) {
                observer.message(
                    events::MessageKind::information,
                    previous_preview ? "preview_hierarchy_changed"
                                     : "pending_delta_hierarchy_changed",
                    "Remote directory changes or unidentified deletions may "
                    "affect saved paths; fetching the full remote state..."
                );
                delta =
                    graph_.list_delta(std::nullopt, delta_progress, stop_token);
                apply_mode = storage::DeltaApplyMode::replace;
            } else {
                for (auto& change : delta.changes) {
                    changes.upsert(std::move(change));
                }
                delta.changes = std::move(changes).release();
            }
        }
        throw_if_cancelled(stop_token);
        std::optional<graph::DeltaResult> preview_delta;
        if (capabilities.previews() && cache_state) {
            preview_delta = delta;
        }
        if (!capabilities.previews()) {
            items_.save_pending_delta({
                .drive_id = config_.drive_id,
                .scope_fingerprint = scope_fingerprint,
                .sync_filter_fingerprint = sync_filter_fingerprint,
                .applied_delta_link = previous_delta_link,
                .applied_filter_fingerprint = previous_sync_filter_fingerprint,
                .delta = delta,
                .apply_mode = apply_mode,
            });
        }
        events::DeltaSummary delta_summary{
            .pages = delta_pages,
            .scanned_items = delta_scanned_items,
            .unique_changes = delta.changes.size(),
        };
        for (const auto& change : delta.changes) {
            if (change.deleted) {
                ++delta_summary.deletions;
            } else if (change.directory) {
                ++delta_summary.directories;
            } else {
                ++delta_summary.files;
            }
        }
        observer.delta_summary(delta_summary);
        std::vector<storage::UploadSuppression> upload_suppressions;
        if (apply_mode == storage::DeltaApplyMode::replace) {
            engine_detail::add_full_refresh_deletions(delta, tracked_items);
        }
        engine_detail::add_deleted_descendants(delta, tracked_items);
        engine_detail::add_moved_descendants(delta, tracked_items);
        auto orphaned_partials = orphaned_partial_downloads(
            delta, apply_mode, items_.partial_downloads(config_.drive_id)
        );
        const auto previously_blocked = items_.blocked_items(config_.drive_id);
        engine_detail::preserve_blocked_deletions(delta, previously_blocked);
        if (!previously_blocked.empty()) {
            engine_detail::add_blocked_retries(
                delta,
                previously_blocked,
                apply_mode == storage::DeltaApplyMode::replace
            );
            spdlog::debug(
                "Added {} blocked items to the synchronization retry plan",
                previously_blocked.size()
            );
        }
        {
            std::unordered_set<
                std::string,
                TransparentStringHash,
                std::equal_to<>>
                tracked_ids;
            tracked_ids.reserve(
                tracked_items.size() + previously_blocked.size()
            );
            for (const auto& item : tracked_items) {
                tracked_ids.insert(item.remote_id);
            }
            for (const auto& item : previously_blocked) {
                tracked_ids.insert(item.remote_id);
            }
            auto filtered = detail::filter_delta(
                std::move(delta),
                sync_filter,
                [&](std::string_view remote_id) {
                    return tracked_ids.contains(remote_id);
                },
                apply_mode
            );
            spdlog::info(
                "Selective synchronization excluded {} remote changes",
                filtered.excluded
            );
            snapshot_removals = std::move(filtered.snapshot_removals);
            if (safe_root) {
                for (const auto& remote_id : filtered.retained_remote_ids) {
                    const auto previous = std::ranges::find(
                        tracked_items, remote_id, &storage::ItemState::remote_id
                    );
                    if (previous == tracked_items.end() ||
                        previous->directory) {
                        continue;
                    }
                    std::error_code error;
                    const auto status = std::filesystem::symlink_status(
                        previous->local_path, error
                    );
                    if (error == std::errc::no_such_file_or_directory) {
                        continue;
                    }
                    if (error) {
                        throw std::runtime_error(
                            "cannot inspect selectively retained local file '" +
                            previous->local_path.string() +
                            "': " + error.message()
                        );
                    }
                    if (!std::filesystem::is_regular_file(status)) {
                        continue;
                    }
                    const auto identity = safe_root->identity(
                        previous->local_path, detail::FilesystemItemKind::file
                    );
                    upload_suppressions.push_back({
                        .drive_id = config_.drive_id,
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
            config_.drive_id,
            sync_root,
            apply_mode,
            sync_filter_fingerprint,
            std::move(snapshot_removals),
            tracked_items,
            std::move(upload_suppressions),
            &items_
        );
        throw_if_cancelled(stop_token);
        for (const auto& partial : orphaned_partials) {
            plan.remove_partial_download(partial.item.remote_id);
        }
        engine_detail::report_plan(
            plan, config_.drive_id, observer, capabilities
        );

        std::size_t blocked_count = plan.blocked_count();
        detail::UploadSummary upload_summary;
        if (capabilities.previews()) {
            if (capabilities.plans_uploads() && safe_root && metadata) {
                upload_summary = detail::upload_local_changes(
                    *safe_root,
                    config_.drive_id,
                    graph_,
                    items_,
                    *metadata,
                    &sync_filter,
                    observer,
                    capabilities,
                    {
                        .maximum_affected_items =
                            config_.maximum_remote_deletions,
                        .force = config_.force_large_delete,
                    },
                    config_.upload_concurrency,
                    stop_token
                );
                blocked_count += upload_summary.blocked;
                engine_detail::report_upload_plan(
                    config_, upload_summary, observer
                );
            }
            spdlog::debug(
                "Dry run preserved files, snapshots, and cursor; saved blocked "
                "diagnostics for drive '{}'",
                config_.drive_id
            );
        } else {
            if (!safe_root || !metadata) {
                throw std::logic_error{
                    "Synchronization filesystem was not prepared"
                };
            }
            auto& active_safe_root = *safe_root;
            auto& active_metadata = *metadata;
            observer.message(
                events::MessageKind::information,
                "execution_started",
                "Executing synchronization plan..."
            );
            observer.sync_phase(events::SyncPhase::applying);
            const auto summary = engine_detail::execute_plan(
                plan,
                active_safe_root,
                config_.drive_id,
                graph_,
                items_,
                active_metadata,
                observer,
                capabilities,
                config_.download_concurrency,
                config_.transfer_order,
                config_.local_conflict,
                config_.sync_permissions,
                stop_token
            );
            throw_if_cancelled(stop_token);
            observer.sync_phase(events::SyncPhase::applying);
            spdlog::debug(
                "Persisting remote delta for drive '{}'", config_.drive_id
            );
            blocked_count = plan.blocked_count();
            const auto unsupported_count = plan.unsupported_count();
            throw_if_cancelled(stop_token);
            items_.apply_delta(plan.release_state_delta());
            discard_orphaned_partial_files(orphaned_partials);
            if (capabilities.uploads()) {
                upload_summary = detail::upload_local_changes(
                    active_safe_root,
                    config_.drive_id,
                    graph_,
                    items_,
                    active_metadata,
                    &sync_filter,
                    observer,
                    capabilities,
                    {
                        .maximum_affected_items =
                            config_.maximum_remote_deletions,
                        .force = config_.force_large_delete,
                    },
                    config_.upload_concurrency,
                    stop_token
                );
                blocked_count += upload_summary.blocked;
            }
            observer.sync_phase(events::SyncPhase::applying);
            upload_summary.uploaded += recovered_uploads;
            engine_detail::report_execution_summary(
                summary,
                upload_summary,
                blocked_count,
                unsupported_count,
                observer
            );
        }

        throw_if_cancelled(stop_token);
        if (capabilities.previews()) {
            observer.sync_summary({.blocked = blocked_count});
        }
        if (capabilities.previews() && cache_state) {
            cache_state->blocked = items_.blocked_items(config_.drive_id);
            std::ranges::sort(
                cache_state->blocked, {}, &storage::BlockedItem::remote_id
            );
        }
        record_result(metrics::SyncRunOutcome::succeeded);
        engine_detail::report_completion(
            blocked_count, capabilities.previews(), started_at, observer
        );
        if (preview_cache_ && preview_delta && cache_state &&
            !stop_token.stop_requested() &&
            !preview_delta->delta_link.empty() &&
            PreviewCache::capture_state(items_, config_.drive_id) ==
                cache_state) {
            preview_cache_->snapshot_.emplace(
                PreviewCache::Snapshot{
                    .scope = PreviewCache::scope(config_),
                    .filter = sync_filter_fingerprint,
                    .state = *cache_state,
                    .delta = std::move(*preview_delta),
                    .apply_mode = apply_mode,
                }
            );
        } else if (preview_cache_ && capabilities.previews()) {
            observer.message(
                events::MessageKind::information,
                "preview_cache_unavailable",
                "Preview metadata was not retained because synchronization "
                "state needs recovery, changed during preview, has no cursor "
                "or the task was cancelled."
            );
        }
        return blocked_count == 0 ? exit_code::success : exit_code::blocked;
    } catch (const SyncCancelledError&) {
        record_result(metrics::SyncRunOutcome::cancelled);
        observer.message(
            events::MessageKind::information,
            "sync_cancelled",
            "Synchronization cancelled."
        );
        return exit_code::cancelled;
    } catch (const graph::RequestCancelledError&) {
        record_result(metrics::SyncRunOutcome::cancelled);
        observer.message(
            events::MessageKind::information,
            "sync_cancelled",
            "Synchronization cancelled."
        );
        return exit_code::cancelled;
    } catch (const detail::LocalScanChangedError& error) {
        record_result(metrics::SyncRunOutcome::failed);
        spdlog::warn(
            "Local filesystem changed during scanning; synchronization "
            "deferred: {}",
            error.what()
        );
        observer.message(
            events::MessageKind::warning,
            "local_scan_changed",
            "Local filesystem changed during scanning; synchronization "
            "deferred: " + std::string{error.what()}
        );
        return exit_code::failure;
    } catch (const onedrive::util::SyncMountUnavailableError& error) {
        record_result(metrics::SyncRunOutcome::failed);
        const auto elapsed = std::chrono::steady_clock::now() - started_at;
        spdlog::warn(
            "Synchronization blocked by unavailable mount after {} "
            "milliseconds: {}",
            std::chrono::duration_cast<std::chrono::milliseconds>(elapsed)
                .count(),
            error.what()
        );
        observer.message(
            events::MessageKind::error, "sync_mount_unavailable", error.what()
        );
        return exit_code::failure;
    } catch (const std::exception& error) {
        record_result(metrics::SyncRunOutcome::failed);
        const auto elapsed = std::chrono::steady_clock::now() - started_at;
        spdlog::warn(
            "Synchronization failed after {} milliseconds: {}",
            std::chrono::duration_cast<std::chrono::milliseconds>(elapsed)
                .count(),
            error.what()
        );
        throw;
    } catch (...) {
        record_result(metrics::SyncRunOutcome::failed);
        const auto elapsed = std::chrono::steady_clock::now() - started_at;
        spdlog::warn(
            "Synchronization failed after {} milliseconds",
            std::chrono::duration_cast<std::chrono::milliseconds>(elapsed)
                .count()
        );
        throw;
    }
}

} // namespace onedrive::sync
