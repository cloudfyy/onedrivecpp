#pragma once

#include "onedrive/storage/item_store.hpp"
#include "onedrive/sync/capabilities.hpp"
#include "sync/filesystem/operations.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace onedrive::events {
class Observer;
}

namespace onedrive::sync::detail {

class SafeSyncRoot;
class SyncList;
struct RemoteDeletionGuard;

struct UploadCandidate {
    std::filesystem::path path;
    std::string remote_path;
    std::optional<storage::ItemState> previous;
    bool directory{false};
    std::uint64_t size{0};
};

[[nodiscard]] std::string upload_blocked_id(std::string_view remote_path);
void save_upload_blocked(
    storage::ItemStore& items,
    const std::string& drive_id,
    const std::string& remote_path,
    bool directory,
    std::string_view reason,
    std::string_view message
);

struct DeletionPlan {
    std::vector<storage::PendingDelete> operations;
    std::size_t affected_items{0};
};

struct LocalMoveDiscovery {
    std::vector<storage::PendingRemoteMove> moves;
    std::unordered_set<std::string> moved_remote_ids;
    std::unordered_set<std::string> moved_local_paths;
    std::unordered_set<std::string> protected_local_paths;
};

[[nodiscard]] bool local_path_is_missing(const std::filesystem::path& path);
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
    RemoteDeletionGuard guard,
    const events::Observer& observer,
    ExecutionMode execution_mode,
    storage::ItemStore* items = nullptr
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
