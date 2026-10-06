#pragma once

#include "onedrive/graph/graph_client.hpp"
#include "onedrive/storage/item_store.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace onedrive::sync::engine_detail {

void add_blocked_retries(
    graph::DeltaResult& delta,
    const std::vector<storage::BlockedItem>& blocked
);
void add_full_refresh_deletions(
    graph::DeltaResult& delta,
    const std::vector<storage::ItemState>& tracked
);
void add_deleted_descendants(
    graph::DeltaResult& delta,
    const std::vector<storage::ItemState>& tracked
);
void add_moved_descendants(
    graph::DeltaResult& delta,
    const std::vector<storage::ItemState>& tracked
);
[[nodiscard]] bool below_blocked_directory(
    std::string_view path,
    const std::vector<std::string>& blocked_directories
);

}  // namespace onedrive::sync::engine_detail
