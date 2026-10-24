#include "sync/core/preparation.hpp"
#include "sync/core/reporting.hpp"
#include "onedrive/sync/cancellation.hpp"
#include "sync/download/recovery.hpp"
#include "sync/filesystem/operations.hpp"
#include "sync/upload/orchestration.hpp"
#include <spdlog/spdlog.h>

namespace onedrive::sync::engine_detail {

PreparedSyncFilesystem prepare_sync_filesystem(
    const config::Config& config,
    std::filesystem::path& sync_root,
    SyncCapabilities capabilities,
    graph::GraphClient& graph,
    storage::ItemStore& items,
    const events::Observer& observer,
    std::stop_token stop_token
) {
    PreparedSyncFilesystem prepared;
    if (capabilities.previews()) {
        report_dry_run_configuration(config, items, observer);
        const auto pending = items.pending_downloads(config.drive_id);
        if (!pending.empty()) {
            spdlog::info(
                "Dry run found {} pending downloads; recovery is deferred",
                pending.size()
            );
        }
        if (std::filesystem::is_directory(sync_root)) {
            prepared.safe_root.emplace(sync_root);
            prepared.metadata.emplace(
                detail::FilesystemMetadata::from_detected_support(
                    config.filesystem_metadata, false
                )
            );
        }
    } else {
        sync_root =
            detail::prepare_sync_root(sync_root, config.sync_permissions);
        prepared.safe_root.emplace(sync_root);
        prepared.metadata.emplace(
            detail::FilesystemMetadata::detect(
                config.filesystem_metadata, sync_root
            )
        );
        if (capabilities.downloads()) {
            throw_if_cancelled(stop_token);
            detail::recover_pending_downloads(
                items,
                *prepared.safe_root,
                config.drive_id,
                *prepared.metadata,
                config.sync_permissions,
                &observer,
                config.local_conflict
            );
        }
        if (capabilities.uploads()) {
            throw_if_cancelled(stop_token);
            detail::recover_pending_remote_moves(
                *prepared.safe_root,
                config.drive_id,
                graph,
                items,
                observer,
                stop_token
            );
            if (capabilities.removes_remote_items()) {
                detail::recover_pending_deletes(
                    config.drive_id,
                    graph,
                    items,
                    observer,
                    {
                        .maximum_affected_items =
                            config.maximum_remote_deletions,
                        .force = config.force_large_delete,
                    },
                    stop_token
                );
            }
            prepared.recovered_uploads = detail::recover_pending_uploads(
                *prepared.safe_root,
                config.drive_id,
                graph,
                items,
                *prepared.metadata,
                observer,
                stop_token
            );
        }
    }
    return prepared;
}

} // namespace onedrive::sync::engine_detail
