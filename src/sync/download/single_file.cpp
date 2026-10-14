#include "onedrive/sync/download/single_file.hpp"
#include "onedrive/exit_code.hpp"
#include "onedrive/sync/blocked_policy.hpp"
#include "graph/support.hpp"

#include "onedrive/util/mount.hpp"
#include "onedrive/util/path_security.hpp"
#include "sync/download/progress.hpp"
#include "sync/download/recovery.hpp"
#include "sync/download/space.hpp"
#include "sync/download/target.hpp"
#include "sync/download/transaction.hpp"
#include "sync/filesystem/operations.hpp"
#include "sync/filesystem/metadata.hpp"
#include "sync/filesystem/safe_sync_root.hpp"

#include <spdlog/spdlog.h>

#include <filesystem>
#include <algorithm>
#include <format>
#include <limits>
#include <stdexcept>
#include <utility>

namespace onedrive::sync {
namespace {

struct SingleFileTarget {
    graph::RemoteItem item;
    std::filesystem::path configured_root;
    std::filesystem::path destination;
};

bool report_unknown_blocker(
    const SingleFileTarget& target,
    const std::string& drive_id,
    storage::ItemStore& items,
    const events::Observer& observer
) {
    const auto blocked = items.blocked_items(drive_id);
    const auto existing = std::ranges::find(
        blocked, target.item.id, &storage::BlockedItem::remote_id
    );
    if (existing == blocked.end() ||
        blocked_item_strategy(existing->reason_code).traits().kind !=
            BlockedPolicyKind::unknown) {
        return false;
    }
    observer.blocked_item(
        existing->remote_path, existing->reason_code, existing->reason_message
    );
    return true;
}

void save_blocked(
    storage::ItemStore& items,
    const std::string& drive_id,
    const graph::RemoteItem& item,
    std::string reason,
    std::string message
) {
    items.save_blocked_item({
        .drive_id = drive_id,
        .remote_id = item.id,
        .parent_id = item.parent_id,
        .name = item.name,
        .etag = item.etag,
        .ctag = item.ctag,
        .remote_path = item.remote_path,
        .last_modified = item.last_modified,
        .size = item.size,
        .directory = item.directory,
        .reason_code = std::move(reason),
        .reason_message = std::move(message),
        .content_hash = item.content_hash,
    });
}

SingleFileTarget resolve_target(
    const config::Config& config,
    const std::string& remote_path,
    graph::GraphClient& graph,
    storage::ItemStore& items
) {
    onedrive::util::require_sync_mount(
        config.sync_data_directory,
        config.sync_data_mount_point
    );
    const auto configured_root =
        onedrive::util::normalized_absolute(config.sync_data_directory);
    static_cast<void>(detail::local_path_for(configured_root, remote_path));

    const auto known_directories =
        blocked_ancestor_context(config.drive_id, items);
    auto item = graph.item_by_path(remote_path, {}, known_directories);
    if (item.directory && item.unsupported_format.empty()) {
        throw std::runtime_error(
            "remote path identifies a directory, not a file: " + remote_path
        );
    }
    if (item.malware) {
        save_blocked(
            items,
            config.drive_id,
            item,
            "malware_detected",
            "Microsoft Graph marked the remote file as malware"
        );
        throw std::runtime_error(
            "Microsoft Graph marked the remote file as malware: " +
            remote_path
        );
    }
    if (item.size < 0 ||
        static_cast<std::uint64_t>(item.size) >
            std::numeric_limits<std::uintmax_t>::max()) {
        throw std::runtime_error(
            "remote file size exceeds the supported range: " + remote_path
        );
    }
    std::filesystem::path destination;
    try {
        destination = detail::local_path_for(configured_root, item.remote_path);
    } catch (const detail::InvalidRemotePathError& error) {
        save_blocked(
            items, config.drive_id, item, "invalid_remote_path", error.what()
        );
        throw;
    }
    return {
        .item = std::move(item),
        .configured_root = configured_root,
        .destination = std::move(destination),
    };
}

bool report_unsupported(
    const SingleFileTarget& target,
    const std::string& drive_id,
    storage::ItemStore& items,
    const events::Observer& observer
) {
    if (target.item.unsupported_format.empty()) {
        return false;
    }
    const auto message = graph::client_detail::unsupported_format_message(
        target.item.unsupported_format
    );
    save_blocked(
        items,
        drive_id,
        target.item,
        "unsupported_" + target.item.unsupported_format,
        message
    );
    spdlog::warn(
        "Unsupported remote item '{}': {}", target.item.remote_path, message
    );
    observer.blocked_item(
        target.item.remote_path,
        "unsupported_" + target.item.unsupported_format,
        message
    );
    observer.section(
        "single_download_plan",
        "Single-file download plan:",
        {{.label = "unsupported format items:",
          .key = "unsupported_items",
          .value = "1"}}
    );
    return true;
}

}  // namespace

int plan_single_file_download(
    const config::Config& config,
    const std::string& remote_path,
    graph::GraphClient& graph,
    storage::ItemStore& items,
    const events::Observer& observer
) {
    const auto target = resolve_target(config, remote_path, graph, items);
    if (report_unsupported(target, config.drive_id, items, observer) ||
        report_unknown_blocker(target, config.drive_id, items, observer)) {
        return exit_code::blocked;
    }
    observer.section(
        "single_download_plan",
        "Single-file download plan:",
        {
            {
                .label = "remote path:",
                .key = "remote_path",
                .value = target.item.remote_path,
            },
            {
                .label = "local path:",
                .key = "local_path",
                .value = target.destination.string(),
            },
            {
                .label = "size:",
                .key = "size",
                .value = std::to_string(target.item.size),
            },
            {
                .label = "local conflict:",
                .key = "local_conflict",
                .value =
                    config.local_conflict == config::LocalConflictPolicy::backup
                        ? "preserve as safeBackup"
                        : "block",
            },
        }
    );
    return exit_code::success;
}

int download_single_file(
    const config::Config& config,
    const std::string& remote_path,
    graph::GraphClient& graph,
    storage::ItemStore& items,
    const events::Observer& observer
) {
    auto target = resolve_target(config, remote_path, graph, items);
    try {
        if (report_unsupported(target, config.drive_id, items, observer) ||
            report_unknown_blocker(target, config.drive_id, items, observer)) {
            return exit_code::blocked;
        }
        auto& item = target.item;
        const auto& configured_root = target.configured_root;
        const auto& destination = target.destination;

        const auto sync_root =
            detail::prepare_sync_root(configured_root, config.sync_permissions);
        detail::SafeSyncRoot safe_root{sync_root};
        safe_root.ensure_directory_tree(
            destination.parent_path(), config.sync_permissions
        );
        const auto metadata = detail::FilesystemMetadata::detect(
            config.filesystem_metadata, sync_root
        );
        detail::recover_pending_downloads(
            items, safe_root, config.drive_id, metadata, config.sync_permissions
        );
        const auto previous = items.find(config.drive_id, item.id);
        const auto target_status =
            detail::inspect_download_target(previous, item, destination);
        if (target_status.preserve_local &&
            config.local_conflict == config::LocalConflictPolicy::block) {
            throw detail::LocalModificationConflictError(
                "local modification conflict: " + destination.string()
            );
        }
        if (target_status.current_remote_file) {
            items.remove_blocked_item(config.drive_id, item.id);
            observer.message(
                events::MessageKind::information,
                "single_download_reused",
                "Local file is already current: " + destination.string()
            );
            return exit_code::success;
        }
        const auto baseline = detail::capture_local_file_baseline(destination);
        detail::DownloadSpaceCoordinator space{
            sync_root,
            detail::download_safety_reserve(
                static_cast<std::uintmax_t>(item.size)
            )
        };
        auto state = detail::item_state_for(config.drive_id, item, destination);
        detail::DownloadProgressEstimator progress_estimator;
        const auto installed = detail::commit_download(
            items,
            safe_root,
            metadata,
            detail::prepare_download(
                graph,
                items,
                item,
                std::move(state),
                destination,
                baseline,
                metadata,
                space,
                {},
                [&](std::uint64_t downloaded, std::uint64_t total) {
                    observer.download_progress(
                        downloaded >= total ? 1U : 0U,
                        1,
                        downloaded,
                        total,
                        downloaded >= total ? util::ProgressState::completed
                                            : util::ProgressState::ongoing,
                        progress_estimator.sample(downloaded, total)
                    );
                }
            ),
            {
                .local_conflict = config.local_conflict,
                .preserve_local = target_status.preserve_local,
                .backup_created = [&](const std::filesystem::path& backup) {
                    observer.message(
                        events::MessageKind::warning,
                        "local_conflict_backed_up",
                        "Preserved local conflict as '" + backup.string() + "'."
                    );
                },
            }
        );
        spdlog::info(
            "Downloaded single remote file '{}' to '{}'",
            item.remote_path,
            installed.local_path.string()
        );
        observer.message(
            events::MessageKind::information,
            "single_download_completed",
            std::format(
                "Downloaded '{}' to '{}'.",
                item.remote_path,
                installed.local_path.string()
            )
        );
        items.remove_blocked_item(config.drive_id, item.id);
        return exit_code::success;
    } catch (const detail::LocalModificationConflictError& error) {
        save_blocked(
            items,
            config.drive_id,
            target.item,
            "local_modification",
            error.what()
        );
        throw;
    } catch (const detail::LocalPathConflictError& error) {
        save_blocked(
            items,
            config.drive_id,
            target.item,
            "local_path_conflict",
            error.what()
        );
        throw;
    } catch (const detail::SafePathConflictError& error) {
        save_blocked(
            items,
            config.drive_id,
            target.item,
            "local_path_conflict",
            error.what()
        );
        throw;
    }
}

}  // namespace onedrive::sync
