#pragma once

#include "onedrive/config/config.hpp"
#include "onedrive/graph/graph_client.hpp"
#include "onedrive/storage/item_store.hpp"
#include "onedrive/ui/common/observer.hpp"

#include <stop_token>
#include <string>

namespace onedrive::sync {

[[nodiscard]] int restore_missing_files(
    const config::Config& config,
    const std::string& remote_path,
    graph::GraphClient& graph,
    storage::ItemStore& items,
    const events::Observer& observer,
    std::stop_token stop_token = {}
);

} // namespace onedrive::sync
