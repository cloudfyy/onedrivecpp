#pragma once

#include "onedrive/config/config.hpp"
#include "onedrive/graph/graph_client.hpp"
#include "onedrive/storage/item_database.hpp"

namespace onedrive::sync {

class SyncEngine {
public:
    SyncEngine(
        const config::Config& config,
        graph::GraphClient& graph,
        storage::ItemDatabase& database
    );

    [[nodiscard]] int synchronize() const;

private:
    const config::Config& config_;
    graph::GraphClient& graph_;
    storage::ItemDatabase& database_;
};

}  // namespace onedrive::sync
