#pragma once

#include "filesystem_metadata.hpp"
#include "onedrive/graph/graph_client.hpp"
#include "onedrive/storage/item_store.hpp"

#include <filesystem>

namespace onedrive::sync::detail {

[[nodiscard]] storage::ItemState download_atomically(
    graph::GraphClient& graph,
    storage::ItemStore& items,
    const graph::RemoteItem& item,
    storage::ItemState state,
    const std::filesystem::path& destination,
    const FilesystemMetadata& metadata
);

}  // namespace onedrive::sync::detail
