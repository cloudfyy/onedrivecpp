#pragma once

#include "onedrive/graph/graph_client.hpp"
#include "onedrive/storage/item_store.hpp"

#include <filesystem>
#include <optional>
#include <string>

namespace onedrive::sync::detail {

struct DownloadTargetStatus {
    bool current_remote_file{false};
    bool preserve_local{false};
};

[[nodiscard]] storage::ItemState item_state_for(
    const std::string& drive_id,
    const graph::RemoteItem& item,
    std::filesystem::path local_path
);
[[nodiscard]] DownloadTargetStatus inspect_download_target(
    const std::optional<storage::ItemState>& previous,
    const graph::RemoteItem& item,
    const std::filesystem::path& destination
);

}  // namespace onedrive::sync::detail
