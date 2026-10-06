#include "onedrive/sync/download/single_file.hpp"

#include "onedrive/util/path_security.hpp"
#include "sync/download/recovery.hpp"
#include "sync/download/space_coordinator.hpp"
#include "sync/download/target.hpp"
#include "sync/download/transaction.hpp"
#include "sync/filesystem/local.hpp"
#include "sync/filesystem/metadata.hpp"
#include "sync/filesystem/safe_sync_root.hpp"

#include <spdlog/spdlog.h>

#include <filesystem>
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

SingleFileTarget resolve_target(
    const config::Config& config,
    const std::string& remote_path,
    graph::GraphClient& graph
) {
    const auto configured_root =
        onedrive::util::normalized_absolute(config.sync_directory);
    static_cast<void>(detail::local_path_for(configured_root, remote_path));

    auto item = graph.item_by_path(remote_path);
    if (item.directory) {
        throw std::runtime_error(
            "remote path identifies a directory, not a file: " + remote_path
        );
    }
    if (item.malware) {
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
    auto destination =
        detail::local_path_for(configured_root, item.remote_path);
    return {
        .item = std::move(item),
        .configured_root = configured_root,
        .destination = std::move(destination),
    };
}

}  // namespace

int plan_single_file_download(
    const config::Config& config,
    const std::string& remote_path,
    graph::GraphClient& graph,
    const cli::Console& console
) {
    const auto target = resolve_target(config, remote_path, graph);
    console.section(
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
                    config.local_conflict ==
                            config::LocalConflictPolicy::backup ?
                        "preserve as safeBackup" :
                        "block",
            },
        }
    );
    return 0;
}

int download_single_file(
    const config::Config& config,
    const std::string& remote_path,
    graph::GraphClient& graph,
    storage::ItemStore& items,
    const cli::Console& console
) {
    auto target = resolve_target(config, remote_path, graph);
    auto& item = target.item;
    const auto& configured_root = target.configured_root;
    const auto& destination = target.destination;

    const auto sync_root =
        detail::prepare_sync_root(configured_root, config.sync_permissions);
    detail::SafeSyncRoot safe_root{sync_root};
    safe_root.ensure_directory_tree(
        destination.parent_path(),
        config.sync_permissions
    );
    const auto metadata = detail::FilesystemMetadata::detect(
        config.filesystem_metadata,
        sync_root
    );
    detail::recover_pending_downloads(
        items,
        safe_root,
        config.drive_id,
        metadata,
        config.sync_permissions
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
        console.message(
            cli::MessageKind::success,
            "single_download_reused",
            "Local file is already current: " + destination.string()
        );
        return 0;
    }
    const auto baseline = detail::capture_local_file_baseline(destination);
    detail::DownloadSpaceCoordinator space{
        sync_root,
        detail::download_safety_reserve(
            static_cast<std::uintmax_t>(item.size)
        )
    };
    auto state =
        detail::item_state_for(config.drive_id, item, destination);
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
                console.download_progress(
                    downloaded >= total ? 1U : 0U,
                    1,
                    downloaded,
                    total,
                    downloaded >= total,
                    {}
                );
            }
        ),
        {
            .local_conflict = config.local_conflict,
            .preserve_local = target_status.preserve_local,
            .backup_created =
                [&](const std::filesystem::path& backup) {
                    console.message(
                        cli::MessageKind::warning,
                        "local_conflict_backed_up",
                        "Preserved local conflict as '" +
                            backup.string() + "'."
                    );
                },
        }
    );
    spdlog::info(
        "Downloaded single remote file '{}' to '{}'",
        item.remote_path,
        installed.local_path.string()
    );
    console.message(
        cli::MessageKind::success,
        "single_download_completed",
        std::format(
            "Downloaded '{}' to '{}'.",
            item.remote_path,
            installed.local_path.string()
        )
    );
    return 0;
}

}  // namespace onedrive::sync
