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

void report_delta_summary(
    const graph::DeltaResult& delta,
    std::size_t pages,
    std::size_t scanned_items,
    const events::Observer& observer
) {
    events::DeltaSummary summary{
        .pages = pages,
        .scanned_items = scanned_items,
        .unique_changes = delta.changes.size(),
    };
    for (const auto& change : delta.changes) {
        if (change.deleted) {
            ++summary.deletions;
        } else if (change.directory) {
            ++summary.directories;
        } else {
            ++summary.files;
        }
    }
    observer.delta_summary(summary);
}

void log_sync_filter_configuration(
    const config::Config& config, const detail::SyncList& sync_filter
) {
    if (config.sync_list) {
        spdlog::info(
            "Loaded {} selective synchronization rules from '{}' "
            "(root files: {})",
            sync_filter.rule_count(),
            config.sync_list->string(),
            config.sync_root_files ? "included" : "rule-selected"
        );
    }
    spdlog::info(
        "Synchronization filters: .nosync {}, dotfiles {}, maximum file "
        "size {}",
        config.nosync_enabled ? "enabled" : "disabled",
        config.dotfiles == config::DotfilePolicy::exclude ? "excluded"
                                                          : "included",
        config.maximum_file_size_bytes == 0
            ? std::string{"unlimited"}
            : std::to_string(config.maximum_file_size_bytes) + " bytes"
    );
}

} // namespace

// Synchronize pipeline breaking the end-to-end synchronization lifecycle into
// five explicit stages:
//   1. prepare_context()      - Mount validation, journal recovery, filter &
//                               cursor/cache state resolution
//   2. fetch_remote_delta()   - Incremental/full Graph delta fetch, cursor
//                               fallback, and retained delta hierarchy merge
//   3. build_sync_plan()      - Descendant expansion, blocked retries,
//                               selective filter & upload suppression, and
//                               SyncPlan construction
//   4. execute_or_preview()   - Preview-only upload planning or full local
//                               apply + remote upload orchestration
//   5. finalize_run()         - Metrics, completion reporting, and session
//                               PreviewCache snapshot persistence
struct SyncEngine::Pipeline {
    SyncEngine& engine;
    const events::Observer& observer;
    SyncCapabilities capabilities{capabilities_for(
        SyncMode::bidirectional, DeletePolicy::propagate, ExecutionMode::apply
    )};
    const std::stop_token& stop_token;
    std::chrono::steady_clock::time_point started_at{};
    std::optional<PreviewCache::Snapshot> previous_preview{};

    // Stage 1 output: prepared local filesystem & resolved cursor/cache state.
    struct PreparedContext {
        std::filesystem::path sync_root;
        std::optional<PreviewCache::State> cache_state;
        engine_detail::PreparedSyncFilesystem prepared;
        detail::SyncList sync_filter;
        std::string scope_fingerprint;
        std::string sync_filter_fingerprint;
        std::optional<std::string> previous_delta_link;
        std::optional<std::string> previous_sync_filter_fingerprint;
        std::optional<std::string> query_delta_link;
        std::optional<graph::DeltaResult> retained_delta;
        std::optional<storage::DeltaApplyMode> retained_mode;
    };

    // Stage 2 output: reconciled remote delta and sorted tracked item baseline.
    struct RemoteDeltaBatch {
        graph::DeltaResult delta;
        storage::DeltaApplyMode apply_mode{storage::DeltaApplyMode::replace};
        std::optional<graph::DeltaResult> preview_delta;
        std::vector<storage::ItemState> tracked_items;
    };

    // Stage 3 output: executable downlink SyncPlan and orphaned partial files.
    struct PlannedBatch {
        detail::SyncPlan plan;
        std::vector<storage::PartialDownload> orphaned_partials;
    };

    // Stage 1: Validate mount point, recover interrupted filesystem journals,
    // and resolve whether preview cache or pending delta can be resumed.
    [[nodiscard]] PreparedContext prepare_context() {
        const auto& config = engine.config_;
        auto& items = engine.items_;

        throw_if_cancelled(stop_token);
        onedrive::util::require_sync_mount(
            config.sync_data_directory, config.sync_data_mount_point
        );
        observer.sync_context(config.sync_mode, capabilities.previews());
        observer.sync_phase(events::SyncPhase::preparing);

        std::filesystem::path sync_root =
            onedrive::util::normalized_absolute(config.sync_data_directory);
        auto cache_state =
            engine.preview_cache_ &&
                    (capabilities.previews() || previous_preview)
                ? PreviewCache::capture_state(items, config.drive_id)
                : std::nullopt;
        auto prepared = engine_detail::prepare_sync_filesystem(
            config,
            sync_root,
            capabilities,
            engine.graph_,
            items,
            observer,
            stop_token
        );

        auto sync_filter = detail::SyncList::configured({
            .rules_path = config.sync_list,
            .sync_root = sync_root,
            .include_root_files = config.sync_root_files,
            .nosync_enabled = config.nosync_enabled,
            .dotfiles = config.dotfiles,
            .maximum_file_size_bytes = config.maximum_file_size_bytes,
        });
        std::string sync_filter_fingerprint =
            engine_detail::sync_scope_fingerprint(sync_filter.fingerprint());
        auto previous_delta_link = items.delta_link(config.drive_id);
        auto previous_sync_filter_fingerprint =
            items.sync_filter_fingerprint(config.drive_id);
        const bool sync_filter_changed =
            previous_delta_link.has_value() &&
            previous_sync_filter_fingerprint.value_or("") !=
                sync_filter_fingerprint;
        auto query_delta_link = sync_filter_changed
                                    ? std::optional<std::string>{}
                                    : previous_delta_link;
        auto scope_fingerprint = PreviewCache::scope_fingerprint(config);

        // Validate persisted pending_delta against current scope, filter, and
        // applied cursor.
        auto pending_delta = items.pending_delta(config.drive_id);
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
                items.remove_pending_delta(config.drive_id);
            }
        }

        // Validate in-memory preview cache against current scope, filter, and
        // local database state snapshot.
        std::optional<graph::DeltaResult> retained_delta;
        std::optional<storage::DeltaApplyMode> retained_mode;
        if (previous_preview &&
            (!cache_state ||
             previous_preview->scope != PreviewCache::scope(config) ||
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

        // Prefer session preview cache; otherwise fall back to persisted
        // pending_delta.
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

        log_sync_filter_configuration(config, sync_filter);
        return {
            .sync_root = std::move(sync_root),
            .cache_state = std::move(cache_state),
            .prepared = std::move(prepared),
            .sync_filter = std::move(sync_filter),
            .scope_fingerprint = std::move(scope_fingerprint),
            .sync_filter_fingerprint = std::move(sync_filter_fingerprint),
            .previous_delta_link = std::move(previous_delta_link),
            .previous_sync_filter_fingerprint =
                std::move(previous_sync_filter_fingerprint),
            .query_delta_link = std::move(query_delta_link),
            .retained_delta = std::move(retained_delta),
            .retained_mode = retained_mode,
        };
    }

    // Stage 2: Query Microsoft Graph delta, handle expired cursors, merge
    // retained preview/pending delta changes (or fall back to full scan if
    // directory hierarchy changed), and persist pending_delta for crash safety.
    [[nodiscard]] RemoteDeltaBatch fetch_remote_delta(PreparedContext& ctx) {
        const auto& config = engine.config_;
        auto& items = engine.items_;

        spdlog::debug(
            "Preparing Microsoft Graph delta query for drive '{}': {} tracked "
            "items, saved cursor {}",
            config.drive_id,
            items.size(),
            ctx.query_delta_link ? "present" : "absent"
        );
        observer.message(
            events::MessageKind::information,
            "delta_query_started",
            "Fetching Microsoft Graph changes..."
        );
        throw_if_cancelled(stop_token);

        auto apply_mode = ctx.query_delta_link.has_value()
                              ? storage::DeltaApplyMode::merge
                              : storage::DeltaApplyMode::replace;
        if (ctx.retained_mode) {
            apply_mode = *ctx.retained_mode;
        }

        graph::DeltaResult delta;
        std::size_t delta_pages = 0;
        std::size_t delta_scanned_items = 0;
        observer.sync_phase(events::SyncPhase::scanning);
        const auto delta_progress = [this, &delta_pages, &delta_scanned_items](
                                        std::size_t pages,
                                        std::size_t scanned,
                                        util::ProgressState state
                                    ) {
            delta_pages = pages;
            delta_scanned_items = scanned;
            observer.delta_progress(pages, scanned, state);
        };
        const auto known_directories =
            blocked_ancestor_context(config.drive_id, items);
        try {
            delta = engine.graph_.list_delta(
                ctx.query_delta_link,
                delta_progress,
                stop_token,
                ctx.query_delta_link
                    ? std::span<const graph::RemoteItem>{known_directories}
                    : std::span<const graph::RemoteItem>{}
            );
        } catch (const graph::DeltaCursorInvalidError& error) {
            previous_preview.reset();
            ctx.retained_delta.reset();
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
            delta = engine.graph_.list_delta(
                std::nullopt, delta_progress, stop_token
            );
            apply_mode = storage::DeltaApplyMode::replace;
        }

        throw_if_cancelled(stop_token);
        auto tracked_items = items.drive_items(config.drive_id);
        std::ranges::sort(tracked_items, {}, &storage::ItemState::remote_id);

        // Merge incremental tail changes onto retained preview/pending delta,
        // or re-fetch from scratch if a folder rename/deletion invalidates
        // cached descendant paths.
        if (ctx.retained_delta) {
            graph::client_detail::DeltaChanges changes{
                std::move(ctx.retained_delta->changes)
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
                delta = engine.graph_.list_delta(
                    std::nullopt, delta_progress, stop_token
                );
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
        if (capabilities.previews() && ctx.cache_state) {
            preview_delta = delta;
        }
        if (!capabilities.previews()) {
            items.save_pending_delta({
                .drive_id = config.drive_id,
                .scope_fingerprint = ctx.scope_fingerprint,
                .sync_filter_fingerprint = ctx.sync_filter_fingerprint,
                .applied_delta_link = ctx.previous_delta_link,
                .applied_filter_fingerprint =
                    ctx.previous_sync_filter_fingerprint,
                .delta = delta,
                .apply_mode = apply_mode,
            });
        }
        report_delta_summary(delta, delta_pages, delta_scanned_items, observer);
        return {
            .delta = std::move(delta),
            .apply_mode = apply_mode,
            .preview_delta = std::move(preview_delta),
            .tracked_items = std::move(tracked_items),
        };
    }

    // Stage 3: Expand directory moves/deletions to tracked descendants, merge
    // blocked retry items, apply selective sync filtering, and build SyncPlan.
    [[nodiscard]] PlannedBatch
    build_sync_plan(PreparedContext& ctx, RemoteDeltaBatch& remote_batch) {
        const auto& config = engine.config_;
        auto& items = engine.items_;
        auto& delta = remote_batch.delta;
        const auto apply_mode = remote_batch.apply_mode;
        const auto& tracked_items = remote_batch.tracked_items;

        if (apply_mode == storage::DeltaApplyMode::replace) {
            engine_detail::add_full_refresh_deletions(delta, tracked_items);
        }
        engine_detail::add_deleted_descendants(delta, tracked_items);
        engine_detail::add_moved_descendants(delta, tracked_items);
        auto orphaned_partials = orphaned_partial_downloads(
            delta, apply_mode, items.partial_downloads(config.drive_id)
        );

        const auto previously_blocked = items.blocked_items(config.drive_id);
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

        // Apply selective sync rules and record upload suppressions for locally
        // retained files that were excluded by filter changes.
        std::vector<std::string> snapshot_removals;
        std::vector<storage::UploadSuppression> upload_suppressions;
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
                ctx.sync_filter,
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
            if (ctx.prepared.safe_root) {
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
                    const auto identity = ctx.prepared.safe_root->identity(
                        previous->local_path, detail::FilesystemItemKind::file
                    );
                    upload_suppressions.push_back({
                        .drive_id = config.drive_id,
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
            config.drive_id,
            ctx.sync_root,
            apply_mode,
            ctx.sync_filter_fingerprint,
            std::move(snapshot_removals),
            tracked_items,
            std::move(upload_suppressions),
            &items
        );
        throw_if_cancelled(stop_token);
        for (const auto& partial : orphaned_partials) {
            plan.remove_partial_download(partial.item.remote_id);
        }
        engine_detail::report_plan(
            plan, config.drive_id, observer, capabilities
        );
        return {
            .plan = std::move(plan),
            .orphaned_partials = std::move(orphaned_partials),
        };
    }

    // Stage 4: Dispatch between non-destructive preview mode and real execution
    // (downlink apply -> SQLite cursor commit -> uplink scan & upload).
    [[nodiscard]] std::size_t
    execute_or_preview(PreparedContext& ctx, PlannedBatch& planned) {
        const auto& config = engine.config_;
        auto& items = engine.items_;
        auto& safe_root = ctx.prepared.safe_root;
        auto& metadata = ctx.prepared.metadata;
        auto& plan = planned.plan;

        std::size_t blocked_count = plan.blocked_count();
        detail::UploadSummary upload_summary;
        if (capabilities.previews()) {
            if (capabilities.plans_uploads() && safe_root && metadata) {
                upload_summary = detail::upload_local_changes(
                    *safe_root,
                    config.drive_id,
                    engine.graph_,
                    items,
                    *metadata,
                    &ctx.sync_filter,
                    observer,
                    capabilities,
                    {
                        .maximum_affected_items =
                            config.maximum_remote_deletions,
                        .force = config.force_large_delete,
                    },
                    config.upload_concurrency,
                    stop_token
                );
                blocked_count += upload_summary.blocked;
                engine_detail::report_upload_plan(
                    config, upload_summary, observer
                );
            }
            spdlog::debug(
                "Dry run preserved files, snapshots, and cursor; saved blocked "
                "diagnostics for drive '{}'",
                config.drive_id
            );
            return blocked_count;
        }

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
            config.drive_id,
            engine.graph_,
            items,
            active_metadata,
            observer,
            capabilities,
            config.download_concurrency,
            config.transfer_order,
            config.local_conflict,
            config.sync_permissions,
            stop_token
        );
        throw_if_cancelled(stop_token);
        observer.sync_phase(events::SyncPhase::applying);
        spdlog::debug(
            "Persisting remote delta for drive '{}'", config.drive_id
        );
        blocked_count = plan.blocked_count();
        const auto unsupported_count = plan.unsupported_count();
        throw_if_cancelled(stop_token);
        items.apply_delta(plan.release_state_delta());
        discard_orphaned_partial_files(planned.orphaned_partials);
        if (capabilities.uploads()) {
            upload_summary = detail::upload_local_changes(
                active_safe_root,
                config.drive_id,
                engine.graph_,
                items,
                active_metadata,
                &ctx.sync_filter,
                observer,
                capabilities,
                {
                    .maximum_affected_items = config.maximum_remote_deletions,
                    .force = config.force_large_delete,
                },
                config.upload_concurrency,
                stop_token
            );
            blocked_count += upload_summary.blocked;
        }
        observer.sync_phase(events::SyncPhase::applying);
        upload_summary.uploaded += ctx.prepared.recovered_uploads;
        engine_detail::report_execution_summary(
            summary, upload_summary, blocked_count, unsupported_count, observer
        );
        return blocked_count;
    }

    // Stage 5: Record metrics, emit completion events, and store the session
    // PreviewCache snapshot if the local baseline remained unchanged.
    [[nodiscard]] int finalize_run(
        PreparedContext& ctx,
        RemoteDeltaBatch& remote_batch,
        std::size_t blocked_count
    ) {
        const auto& config = engine.config_;
        auto& items = engine.items_;

        throw_if_cancelled(stop_token);
        if (capabilities.previews()) {
            observer.sync_summary({.blocked = blocked_count});
        }
        if (capabilities.previews() && ctx.cache_state) {
            ctx.cache_state->blocked = items.blocked_items(config.drive_id);
            std::ranges::sort(
                ctx.cache_state->blocked, {}, &storage::BlockedItem::remote_id
            );
        }
        engine.metrics_.record_sync_run(
            metrics::SyncRunOutcome::succeeded,
            std::chrono::steady_clock::now() - started_at
        );
        engine_detail::report_completion(
            blocked_count, capabilities.previews(), started_at, observer
        );
        if (engine.preview_cache_ && remote_batch.preview_delta &&
            ctx.cache_state && !stop_token.stop_requested() &&
            !remote_batch.preview_delta->delta_link.empty() &&
            PreviewCache::capture_state(items, config.drive_id) ==
                ctx.cache_state) {
            engine.preview_cache_->snapshot_.emplace(
                PreviewCache::Snapshot{
                    .scope = PreviewCache::scope(config),
                    .filter = ctx.sync_filter_fingerprint,
                    .state = *ctx.cache_state,
                    .delta = std::move(*remote_batch.preview_delta),
                    .apply_mode = remote_batch.apply_mode,
                }
            );
        } else if (engine.preview_cache_ && capabilities.previews()) {
            observer.message(
                events::MessageKind::information,
                "preview_cache_unavailable",
                "Preview metadata was not retained because synchronization "
                "state needs recovery, changed during preview, has no cursor "
                "or the task was cancelled."
            );
        }
        return blocked_count == 0 ? exit_code::success : exit_code::blocked;
    }
};

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
        Pipeline pipeline{
            .engine = *this,
            .observer = observer,
            .capabilities = capabilities,
            .stop_token = stop_token,
            .started_at = started_at,
            .previous_preview = std::move(previous_preview),
        };
        auto context = pipeline.prepare_context();
        auto remote_batch = pipeline.fetch_remote_delta(context);
        auto planned = pipeline.build_sync_plan(context, remote_batch);
        const auto blocked_count =
            pipeline.execute_or_preview(context, planned);
        return pipeline.finalize_run(context, remote_batch, blocked_count);
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
