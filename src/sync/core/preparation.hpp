#pragma once

#include "onedrive/config/config.hpp"
#include "onedrive/graph/graph_client.hpp"
#include "onedrive/storage/item_store.hpp"
#include "onedrive/sync/capabilities.hpp"
#include "sync/filesystem/metadata.hpp"
#include "sync/filesystem/safe_sync_root.hpp"

#include <cstddef>
#include <filesystem>
#include <optional>
#include <stop_token>

namespace onedrive::events {
class Observer;
}

namespace onedrive::sync::engine_detail {

struct PreparedSyncFilesystem {
    std::optional<detail::SafeSyncRoot> safe_root;
    std::optional<detail::FilesystemMetadata> metadata;
    std::size_t recovered_uploads{0};
};

PreparedSyncFilesystem prepare_sync_filesystem(
    const config::Config& config,
    std::filesystem::path& sync_root,
    SyncCapabilities capabilities,
    graph::GraphClient& graph,
    storage::ItemStore& items,
    const events::Observer& observer,
    std::stop_token stop_token
);

} // namespace onedrive::sync::engine_detail
