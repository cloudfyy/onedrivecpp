#pragma once

#include "onedrive/events/observer.hpp"
#include "onedrive/config/config.hpp"
#include "onedrive/graph/graph_client.hpp"
#include "onedrive/storage/item_store.hpp"

#include <string>

namespace onedrive::sync {

[[nodiscard]] int plan_single_file_download(
    const config::Config& config,
    const std::string& remote_path,
    graph::GraphClient& graph,
    const events::Observer& observer
);
[[nodiscard]] int download_single_file(
    const config::Config& config,
    const std::string& remote_path,
    graph::GraphClient& graph,
    storage::ItemStore& items,
    const events::Observer& observer
);

}  // namespace onedrive::sync
