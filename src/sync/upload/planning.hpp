#pragma once

#include "onedrive/storage/item_store.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

namespace onedrive::cli {
class Console;
}

namespace onedrive::sync::detail {

class SafeSyncRoot;
class SyncList;
struct RemoteDeletionPolicy;

struct UploadCandidate {
    std::filesystem::path path;
    std::string remote_path;
    std::optional<storage::ItemState> previous;
    bool directory{false};
};

struct DeletionPlan {
    std::vector<storage::PendingDelete> operations;
    std::size_t affected_items{0};
};

struct LocalMoveDiscovery {
    std::vector<storage::PendingRemoteMove> moves;
    std::unordered_set<std::string> moved_remote_ids;
    std::unordered_set<std::string> moved_local_paths;
};

[[nodiscard]] inline bool reserved_local_name(
    const std::filesystem::path& path
) {
    const auto name = path.filename().string();
    return name.contains(".safeBackup-") ||
           name.contains(".onedrive-partial-") ||
           name.contains(".onedrive-upload-") ||
           name.contains(".onedrive-move-");
}

[[nodiscard]] bool local_path_is_missing(
    const std::filesystem::path& path
);
[[nodiscard]] DeletionPlan discover_deletions(
    const std::string& drive_id,
    storage::ItemStore& items,
    const SyncList* sync_list,
    const std::unordered_set<std::string>& skipped_remote_ids = {}
);
[[nodiscard]] DeletionPlan deletion_plan_for(
    std::vector<storage::PendingDelete> operations,
    const std::vector<storage::ItemState>& tracked_items
);
[[nodiscard]] bool enforce_remote_deletion_limit(
    const DeletionPlan& plan,
    RemoteDeletionPolicy policy,
    const cli::Console& console,
    bool dry_run
);
[[nodiscard]] LocalMoveDiscovery discover_local_moves(
    const SafeSyncRoot& sync_root,
    const std::string& drive_id,
    storage::ItemStore& items,
    const SyncList* sync_list
);
[[nodiscard]] std::vector<UploadCandidate> discover_move_parent_uploads(
    const SafeSyncRoot& sync_root,
    const std::string& drive_id,
    storage::ItemStore& items,
    const std::vector<storage::PendingRemoteMove>& moves
);

}  // namespace onedrive::sync::detail
