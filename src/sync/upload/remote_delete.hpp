#pragma once

#include "onedrive/graph/graph_client.hpp"
#include "onedrive/storage/item_store.hpp"

#include <stop_token>

namespace onedrive::sync::detail {

void execute_new_remote_delete(
    storage::PendingDelete deletion,
    graph::GraphClient& graph,
    storage::ItemStore& items,
    std::stop_token stop_token = {}
);

}  // namespace onedrive::sync::detail
