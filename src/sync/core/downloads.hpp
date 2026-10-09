#pragma once

#include "onedrive/config/config.hpp"
#include "onedrive/graph/graph_client.hpp"
#include "onedrive/storage/item_store.hpp"
#include "sync/filesystem/operations.hpp"

#include <cstddef>
#include <exception>
#include <filesystem>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

namespace onedrive::events {
class Observer;
}

namespace onedrive::sync::detail {
class DownloadSpaceCoordinator;
class FilesystemMetadata;
class ItemOperationCoordinator;
class SafeSyncRoot;
}

namespace onedrive::sync::engine_detail {

struct DownloadTask {
    graph::RemoteItem item;
    storage::ItemState state;
    std::filesystem::path destination;
    detail::LocalFileBaseline destination_baseline;
    bool preserve_local{false};
};

struct DownloadBatch {
    std::vector<std::optional<storage::ItemState>> states;
    std::vector<std::optional<std::string>> conflicts;
    std::vector<std::exception_ptr> errors;
};

struct CompletedDownloadBaseline {
    std::size_t files{0};
    std::uint64_t bytes{0};
};

DownloadBatch download_files(
    const std::vector<DownloadTask>& tasks,
    CompletedDownloadBaseline completed,
    std::size_t concurrency,
    graph::GraphClient& graph,
    storage::ItemStore& items,
    detail::ItemOperationCoordinator& operations,
    detail::DownloadSpaceCoordinator& space,
    const detail::FilesystemMetadata& metadata,
    const events::Observer& observer,
    const detail::SafeSyncRoot& sync_root,
    config::LocalConflictPolicy local_conflict,
    const std::stop_token& stop_token
);

}  // namespace onedrive::sync::engine_detail
