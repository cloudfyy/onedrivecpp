#pragma once

#include "onedrive/graph/graph_client.hpp"
#include "onedrive/storage/item_store.hpp"

namespace onedrive::sync::detail {

class SafeSyncRoot;

void execute_new_remote_move(
    const SafeSyncRoot& sync_root,
    storage::PendingRemoteMove move,
    storage::ItemState previous,
    graph::GraphClient& graph,
    storage::ItemStore& items
);

}  // namespace onedrive::sync::detail
