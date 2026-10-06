#pragma once

#include "onedrive/graph/graph_client.hpp"
#include "onedrive/storage/item_store.hpp"
#include "onedrive/sync/capabilities.hpp"

#include <cstddef>
#include <filesystem>
#include <string>

namespace onedrive::cli {
class Console;
}

namespace onedrive::sync::detail {

class FilesystemMetadata;
class SafeSyncRoot;
class SyncList;

struct UploadSummary {
    std::size_t planned{0};
    std::size_t planned_directories{0};
    std::size_t uploaded{0};
    std::size_t created_directories{0};
    std::size_t planned_deletions{0};
    std::size_t affected_deletions{0};
    bool large_delete_blocked{false};
    std::size_t deleted{0};
    std::size_t planned_moves{0};
    std::size_t moved{0};
    std::size_t blocked{0};
};

struct RemoteDeletionGuard {
    std::size_t maximum_affected_items{1000};
    bool force{false};
};

[[nodiscard]] UploadSummary upload_local_changes(
    const SafeSyncRoot& sync_root,
    const std::string& drive_id,
    graph::GraphClient& graph,
    storage::ItemStore& items,
    const FilesystemMetadata& metadata,
    const SyncList* sync_list,
    const cli::Console& console,
    SyncCapabilities capabilities,
    RemoteDeletionGuard deletion_guard,
    std::size_t upload_concurrency
);
void recover_pending_uploads(
    const SafeSyncRoot& sync_root,
    const std::string& drive_id,
    graph::GraphClient& graph,
    storage::ItemStore& items,
    const FilesystemMetadata& metadata,
    const cli::Console& console
);
void recover_pending_deletes(
    const std::string& drive_id,
    graph::GraphClient& graph,
    storage::ItemStore& items,
    const cli::Console& console,
    RemoteDeletionGuard deletion_guard
);
void recover_pending_remote_moves(
    const SafeSyncRoot& sync_root,
    const std::string& drive_id,
    graph::GraphClient& graph,
    storage::ItemStore& items,
    const cli::Console& console
);

}  // namespace onedrive::sync::detail
