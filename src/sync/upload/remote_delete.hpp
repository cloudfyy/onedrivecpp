#pragma once

#include "onedrive/graph/graph_client.hpp"
#include "onedrive/storage/item_store.hpp"

namespace onedrive::sync::detail {

void execute_new_remote_delete(
    storage::PendingDelete deletion,
    graph::GraphClient& graph,
    storage::ItemStore& items
);

}  // namespace onedrive::sync::detail
